import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import replay_agent_trace as replay


def fixture():
    return {'request_id': 'fixture-request', 'events': [
        {'type': 'model_request', 'data': {
            'model': 'fixture-model', 'stream': True, 'stream_options': {'include_usage': True},
            'tools': [{'type': 'function', 'function': {'name': 'search_knowledge', 'parameters': {'type': 'object'}}}],
            'messages': [
                {'role': 'system', 'content': 'Fixture instructions'},
                {'role': 'user', 'content': 'Original question'},
                {'role': 'assistant', 'content': '', 'tool_calls': [{'id': 'call-1', 'type': 'function',
                    'function': {'name': 'search_knowledge', 'arguments': '{"query":"fixture"}'}}]},
                {'role': 'tool', 'tool_call_id': 'call-1', 'content': '{"results":[]}'},
                {'role': 'assistant', 'content': 'Original answer'},
                {'role': 'user', 'content': 'Which tools did you use?'},
            ]}},
        {'type': 'model_response', 'data': {'text': 'Recorded response'}},
    ]}


class Provider(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append(body)
        assert self.path == '/v1/chat/completions'
        assert self.headers['Authorization'] == 'Bearer fixture-secret'
        result = {'model': 'fixture-reported-model', 'choices': [{'message': {
            'role': 'assistant', 'content': 'Tool history visible: ' + str(any(m['role'] == 'tool' for m in body['messages']))},
            'finish_reason': 'stop'}], 'usage': {'prompt_tokens': 10, 'completion_tokens': 5}}
        data = json.dumps(result).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class ReplayTests(unittest.TestCase):
    def test_offline_export_is_private_and_contains_request_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = replay.output_directory(Path(temporary) / 'export')
            replay.export_artifacts(fixture(), out)
            self.assertEqual(json.loads((out / 'model-request-01.json').read_text()), fixture()['events'][0]['data'])
            self.assertIn('Pairing', (out / 'report.md').read_text())
            self.assertEqual((out / 'trace.json').stat().st_mode & 0o777, 0o600)
            self.assertEqual(out.stat().st_mode & 0o777, 0o700)
            with self.assertRaises(ValueError):
                replay.output_directory(out)

    def test_missing_old_trace_is_not_fabricated(self):
        with self.assertRaisesRegex(ValueError, 'legacy'):
            replay.trace_requests({'events': [{'type': 'message', 'data': {'role': 'assistant', 'content': 'Answer'}}]})

    def test_early_failure_can_be_exported_without_inventing_model_requests(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = replay.output_directory(Path(temporary) / 'early-failure')
            trace = {'request_id': 'early-failure', 'events': [
                {'type': 'request_end', 'data': {'stop_reason': 'token_limit'}}]}
            self.assertEqual(replay.export_artifacts(trace, out), [])
            self.assertIn('No model HTTP request was recorded', (out / 'report.md').read_text())
            self.assertEqual(list(out.glob('model-request-*.json')), [])

    def test_fixed_results_are_preserved_without_mutating_original(self):
        original = fixture()['events'][0]['data']
        full = replay.replay_body(original, 256)
        self.assertEqual(full['messages'], original['messages'])
        self.assertFalse(full['stream'])
        self.assertEqual(full['tool_choice'], 'none')
        self.assertEqual(full['max_tokens'], 256)
        self.assertTrue(original['stream'])
        ablation = replay.replay_body(original, 256, True)
        self.assertEqual([m['role'] for m in ablation['messages']], ['system', 'user', 'assistant', 'user'])
        self.assertEqual(ablation['messages'][-1], original['messages'][-1])

    def test_unpaired_messages_and_omitted_images_are_rejected(self):
        body = fixture()['events'][0]['data']
        del body['messages'][3]
        self.assertTrue(replay.check_messages(body['messages']))
        with self.assertRaisesRegex(ValueError, 'malformed'):
            replay.replay_body(body, 256)
        body = fixture()['events'][0]['data']
        body['messages'][0]['content'] = '[image data omitted]'
        with self.assertRaisesRegex(ValueError, 'images'):
            replay.replay_body(body, 256)

    def test_credentials_and_nested_signed_links_are_redacted(self):
        value = {'API_KEY': 'secret-key', 'content': json.dumps({'download_url': 'secret-link', 'evidence': 'Retain me'}),
                 'text': 'https://host/api/knowledge/download?token=secret-token', 'image': 'data:image/png;base64,secret-pixels'}
        safe = replay.redact(value)
        self.assertNotIn('secret', json.dumps(safe))
        self.assertIn('Retain me', json.dumps(safe))

    def test_endpoint_validation(self):
        self.assertEqual(replay.api_base('https://example.org/v1/'), 'https://example.org/v1')
        for url in ('http://example.org', 'https://key@example.org/v1', 'https://example.org/v1?key=secret'):
            with self.assertRaises(ValueError):
                replay.api_base(url)

    def test_default_replay_never_calls_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace = root / 'trace.json'
            trace.write_text(json.dumps(fixture()))
            with patch.object(replay, 'request_json', side_effect=AssertionError('Unexpected network call')):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(replay.main(['replay', str(trace), '--output', str(root / 'offline')]), 0)

    def test_live_replay_and_ablation_call_only_the_model(self):
        Provider.requests = []
        server = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                trace = root / 'trace.json'
                trace.write_text(json.dumps(fixture()))
                config = root / 'config.json'
                config.write_text(json.dumps({'remote_engine': {'endpoint': f'http://127.0.0.1:{server.server_port}/v1',
                    'model': 'fixture-model', 'api_key': 'fixture-secret'}}))
                with patch.dict(os.environ, {'REMOTE_ENGINE_ENDPOINT': '', 'REMOTE_ENGINE_MODEL': '', 'REMOTE_ENGINE_API_KEY': ''}):
                    with contextlib.redirect_stdout(io.StringIO()):
                        status = replay.main(['replay', str(trace), '--model-config', str(config), '--live',
                            '--compare-without-tool-history', '--output', str(root / 'live')])
                self.assertEqual(status, 0)
                self.assertEqual(len(Provider.requests), 2)
                self.assertEqual(Provider.requests[0]['tool_choice'], 'none')
                self.assertTrue(any(m['role'] == 'tool' for m in Provider.requests[0]['messages']))
                self.assertFalse(any(m['role'] == 'tool' for m in Provider.requests[1]['messages']))
                report = (root / 'live/replay.md').read_text()
                self.assertIn('Tool history visible: True', report)
                self.assertIn('Tool history visible: False', report)
                self.assertNotIn('fixture-secret', ''.join(p.read_text() for p in (root / 'live').iterdir()))
        finally:
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
