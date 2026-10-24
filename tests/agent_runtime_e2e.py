#!/usr/bin/env python3
"""Real muduo + remote streaming adapter + agent loop, using local model/search fixtures."""
import argparse
import http.client
import json
import shlex
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import native_mcp_e2e as native


class Model(BaseHTTPRequestHandler):
    requests = []
    waiting = threading.Event()
    condition = threading.Condition()

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert self.headers['Authorization'] == 'Bearer fixture-model-key'
        with self.condition:
            self.requests.append(body)
            self.condition.notify_all()
        prompt = next(item['content'] for item in reversed(body['messages']) if item['role'] == 'user' and isinstance(item['content'], str))
        tools = [item for item in body['messages'] if item['role'] == 'tool']
        if prompt == 'resume':
            previous = [item for item in body['messages'] if item['role'] == 'assistant']
            assert previous and previous[0]['reasoning_content'] == 'fixture reasoningfixture reasoning'
        if prompt == 'failure' and tools:
            self.send_response(503)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Connection', 'close')
        self.end_headers()
        if prompt in ('slow-model', 'disconnect'):
            self.waiting.set()
            time.sleep(2)
        try:
            def send(value):
                self.wfile.write(('data: ' + json.dumps(value, ensure_ascii=False) + '\n\n').encode())
                self.wfile.flush()
            def delta(value, finish=None):
                send({'choices': [{'index': 0, 'delta': value, 'finish_reason': finish}], 'usage': None})
            if prompt == 'partial-timeout':
                delta({'content': 'Partial output'})
                time.sleep(2)
            if prompt == 'malformed-stream':
                self.wfile.write(b'data: {bad}\n\n')
            elif prompt == 'truncated-stream':
                delta({'content': 'partial'})
                return
            elif prompt in ('direct', 'slow-model', 'disconnect', 'missing-usage', 'partial-timeout', 'resume', 'length-answer') or (tools and prompt != 'loop' and (prompt not in ('both', 'search-fetch') or len(tools) >= 2)):
                if tools:
                    assistant = next(item for item in reversed(body['messages']) if item['role'] == 'assistant')
                    assert assistant['reasoning_content'] == 'fixture reasoning'
                    assert assistant['tool_calls'][-1]['id'] == tools[-1]['tool_call_id']
                for text in ('Final ', 'answer ', 'with source.'):
                    delta({'content': text})
                delta({}, 'length' if prompt == 'length-answer' else 'stop')
            else:
                name = 'search_web' if prompt in ('web', 'unauthorized') or (prompt == 'both' and tools) else 'search_knowledge'
                arguments = '{invalid' if prompt == 'invalid-arguments' else json.dumps({'query': 'evidence'})
                if prompt == 'fetch-web':
                    name, arguments = 'fetch_web', json.dumps({'url': 'https://example.org/paper'})
                elif prompt in ('fetch-text', 'fetch-pages', 'fetch-source') or (prompt == 'search-fetch' and tools):
                    name = 'fetch_knowledge'
                    representation = 'pages' if prompt == 'fetch-pages' else 'source' if prompt == 'fetch-source' else 'text'
                    arguments = json.dumps({'doc_id': 7, 'representation': representation, 'start_page': 2, 'page_count': 1})
                delta({'reasoning_content': 'fixture reasoning'})
                delta({'tool_calls': [{'index': 0, 'id': 'call_' + str(len(tools)), 'type': 'function',
                                       'function': {'name': name[:7], 'arguments': arguments[:5]}}]})
                delta({'tool_calls': [{'index': 0, 'function': {'name': name[7:], 'arguments': arguments[5:]}}]})
                delta({}, 'length' if prompt == 'truncated-tool' else 'tool_calls')
            if prompt != 'missing-usage':
                send({'choices': [], 'usage': {'prompt_tokens': 25000 if prompt == 'high-usage' else 100, 'completion_tokens': 10}})
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class AgentTest(native.NativeMcpTest):
    @classmethod
    def setUpClass(cls):
        cls.model = ThreadingHTTPServer(('127.0.0.1', 0), Model)
        threading.Thread(target=cls.model.serve_forever, daemon=True).start()
        native.SERVER_EXTRA_ARGS = [f'http://127.0.0.1:{cls.model.server_port}/v1']
        super().setUpClass()
        cls.addClassCleanup(cls.model.server_close)
        cls.addClassCleanup(cls.model.shutdown)

    def stream(self, prompt, token='ADMIN_TOKEN', **limits):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=8)
        connection.request('POST', '/api/chat/stream', json.dumps({'message': prompt, **limits}),
                           {'Authorization': 'Bearer ' + self.tokens[token], 'Content-Type': 'application/json'})
        response = connection.getresponse()
        events = []
        try:
            for raw in response:
                line = raw.decode().strip()
                if line == 'data: [DONE]':
                    break
                if line.startswith('data: '):
                    events.append(json.loads(line[6:]))
        finally:
            connection.close()
        return events

    def test_agent_fetch_tools_and_pdf_images(self):
        for prompt, tool in [('fetch-web', 'fetch_web'), ('fetch-text', 'fetch_knowledge'),
                             ('fetch-source', 'fetch_knowledge'), ('fetch-pages', 'fetch_knowledge')]:
            events = self.stream(prompt)
            self.assertEqual(events[-1]['type'], 'done', events)
            self.assertEqual([e['name'] for e in events if e['type'] == 'tool_start'], [tool])
            self.assertFalse(next(e for e in events if e['type'] == 'tool_end')['is_error'])
            messages = Model.requests[-1]['messages']
            evidence = json.loads(next(m['content'] for m in messages if m['role'] == 'tool'))
            if prompt == 'fetch-text':
                self.assertIn('Original PDF page 2', evidence['content'])
            elif prompt == 'fetch-web':
                self.assertIn('Original fetched Markdown', evidence['content'])
            elif prompt == 'fetch-source':
                self.assertIn('/api/knowledge/download?', evidence['download_url'])
            else:
                self.assertNotIn('images', evidence)
                image_message = messages[-1]
                self.assertEqual(image_message['role'], 'user')
                self.assertEqual(image_message['content'][0]['type'], 'text')
                self.assertTrue(image_message['content'][1]['image_url']['url'].startswith('data:image/png;base64,'))
                self.assertEqual(messages[-2]['role'], 'tool')
        events = self.stream('search-fetch')
        self.assertEqual(events[-1]['type'], 'done')
        self.assertEqual([e['name'] for e in events if e['type'] == 'tool_start'], ['search_knowledge', 'fetch_knowledge'])

    def test_agent_text_only_model_rejects_image_fetch(self):
        events = self.stream('fetch-pages', image_inputs=False)
        self.assertEqual(events[-1]['type'], 'done')
        self.assertTrue(next(e for e in events if e['type'] == 'tool_end')['is_error'])
        self.assertFalse(any(isinstance(m['content'], list) for m in Model.requests[-1]['messages']))
        tool = next(t for t in Model.requests[-1]['tools'] if t['function']['name'] == 'fetch_knowledge')
        self.assertEqual(tool['function']['parameters']['properties']['representation']['enum'], ['source', 'text'])

    def test_agent_visible_output_limit_is_a_truncated_answer(self):
        events = self.stream('length-answer')
        self.assertEqual(events[-1]['type'], 'done')
        self.assertTrue(events[-1]['truncated'])
        self.assertEqual(events[-1]['finish_reason'], 'length')
        self.assertEqual(''.join(e['content'] for e in events if e['type'] == 'answer'), 'Final answer with source.')
        self.assertNotIn('tool_start', [event['type'] for event in events])

    def test_agent_direct_answer_and_usage(self):
        events = self.stream('direct')
        self.assertEqual(''.join(e['content'] for e in events if e['type'] == 'answer'), 'Final answer with source.')
        self.assertEqual(events[-1]['type'], 'done')
        self.assertEqual(events[-1]['usage'], {'prompt_tokens': 100, 'completion_tokens': 10, 'estimated': False})
        self.assertNotIn('tool_start', [e['type'] for e in events])

    def test_agent_output_limit_is_omitted_unless_explicit(self):
        self.assertEqual(self.stream("direct")[-1]["type"], "done")
        self.assertNotIn("max_tokens", Model.requests[-1])
        self.assertEqual(self.stream("direct", max_tokens=64)[-1]["type"], "done")
        self.assertEqual(Model.requests[-1]["max_tokens"], 64)

    def test_agent_two_tool_rounds_and_reasoning_replay(self):
        events = self.stream('both')
        starts = [e['name'] for e in events if e['type'] == 'tool_start']
        self.assertEqual(starts, ['search_knowledge', 'search_web'])
        self.assertEqual(events[-1]['type'], 'done')
        self.assertEqual(events[-1]['usage'], {'prompt_tokens': 300, 'completion_tokens': 30, 'estimated': False})
        final_request = Model.requests[-1]
        self.assertNotIn("max_tokens", final_request)
        messages = final_request['messages']
        self.assertEqual([m['role'] for m in messages][-5:], ['user', 'assistant', 'tool', 'assistant', 'tool'])
        self.assertIn('tenant-7.md', messages[2]['content'] if messages[2]['role'] == 'tool' else messages[3]['content'])

    def test_agent_cumulative_budget_is_disabled_by_default_and_zero(self):
        for limits in ({}, {'max_total_tokens': 0}):
            events = self.stream('high-usage', **limits)
            self.assertEqual(events[-1]['type'], 'done', events)
            self.assertEqual(events[-1]['usage']['prompt_tokens'], 50000)
            self.assertEqual(len([e for e in events if e['type'] == 'agent_turn']), 2)
        for limits in ({'max_total_tokens': 24000}, {'remaining_quota': 24000}):
            events = self.stream('high-usage', **limits)
            self.assertEqual(events[-1]['stop_reason'], 'token_limit', events)
            self.assertEqual(events[-1]['usage']['prompt_tokens'], 25000)

    def test_agent_tool_failure_and_permission_recovery(self):
        for prompt, token in [('invalid-arguments', 'ADMIN_TOKEN'), ('unauthorized', 'USER_TOKEN')]:
            events = self.stream(prompt, token)
            self.assertTrue(next(e for e in events if e['type'] == 'tool_end')['is_error'])
            self.assertEqual(events[-1]['type'], 'done')
            if token == 'USER_TOKEN':
                self.assertNotIn('search_web', [t['function']['name'] for t in Model.requests[-1]['tools']])
                self.assertIn('error', Model.requests[-1]['messages'][-1]['content'])

    def test_agent_budget_error_identifies_the_limiting_resource(self):
        for limits, message in [
            ({'remaining_quota': 1}, 'Daily user token quota'),
            ({'max_total_tokens': 1}, 'Agent per-request token budget'),
            ({'remaining_quota': 1, 'max_total_tokens': 2}, 'Daily user token quota'),
            ({'remaining_quota': 2, 'max_total_tokens': 1}, 'Agent per-request token budget'),
        ]:
            before = len(Model.requests)
            events = self.stream('direct', **limits)
            self.assertEqual(events[-1]['stop_reason'], 'token_limit', events)
            self.assertIn(message, events[-1]['error'])
            self.assertIn('remaining=1, estimated_input=', events[-1]['error'])
            self.assertEqual(events[-1]['usage']['prompt_tokens'], 0)
            self.assertEqual(len(Model.requests), before)

        events = self.stream('high-usage', remaining_quota=25011)
        self.assertEqual(events[-1]['stop_reason'], 'token_limit', events)
        self.assertIn('Daily user token quota', events[-1]['error'])
        self.assertIn('remaining=1, estimated_input=', events[-1]['error'])
        self.assertIn('request_used=25010', events[-1]['error'])
        self.assertEqual(len([e for e in events if e['type'] == 'agent_turn']), 1)

    def test_agent_limits_and_partial_usage_on_failure(self):
        for limits, reason in [({'max_turns': 1}, 'turn_limit'), ({'max_total_tokens': 1}, 'token_limit'), ({'remaining_quota': 1}, 'token_limit')]:
            events = self.stream('loop', **limits)
            self.assertEqual(events[-1]['stop_reason'], reason)
            self.assertNotIn('tool_start', [e['type'] for e in events])
        truncated = self.stream('truncated-tool')
        self.assertEqual(truncated[-1]['stop_reason'], 'output_limit')
        self.assertNotIn('tool_start', [event['type'] for event in truncated])
        events = self.stream('failure')
        self.assertEqual(events[-1]['stop_reason'], 'model_error')
        self.assertEqual(events[-1]['usage'], {'prompt_tokens': 100, 'completion_tokens': 10, 'estimated': False})
        events = self.stream('loop', max_turns=2)
        self.assertEqual(events[-1]['stop_reason'], 'turn_limit')
        self.assertEqual(events[-1]['usage']['prompt_tokens'], 200)

    def test_agent_timeout_interrupts_blocked_model_read(self):
        started = time.monotonic()
        events = self.stream('slow-model', timeout_seconds=1)
        self.assertLess(time.monotonic() - started, 1.8)
        self.assertEqual(events[-1]['stop_reason'], 'timeout')
        self.assertEqual(events[-1]['type'], 'error')

    def test_agent_disconnect_releases_runtime_worker(self):
        Model.waiting.clear()
        connections = []
        for _ in range(2):
            connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
            connection.request('POST', '/api/chat/stream', json.dumps({'message': 'disconnect'}),
                               {'Authorization': 'Bearer ' + self.tokens['ADMIN_TOKEN'], 'Content-Type': 'application/json'})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            connections.append((connection, response))
        self.assertTrue(Model.waiting.wait(2))
        for connection, response in connections:
            response.close()
            connection.close()
        started = time.monotonic()
        self.assertEqual(self.stream('direct')[-1]['type'], 'done')
        self.assertLess(time.monotonic() - started, 1.2)

    def test_agent_estimated_usage_when_provider_usage_is_missing(self):
        events = self.stream('missing-usage')
        self.assertEqual(events[-1]['type'], 'done')
        self.assertTrue(events[-1]['usage']['estimated'])
        self.assertGreater(events[-1]['usage']['prompt_tokens'], 0)
        events = self.stream('partial-timeout', timeout_seconds=1)
        self.assertEqual(events[-1]['stop_reason'], 'timeout')
        self.assertTrue(events[-1]['usage']['estimated'])
        self.assertGreater(events[-1]['usage']['completion_tokens'], 0)
        self.assertIn('Partial output', ''.join(event.get('content', '') for event in events))

    def test_agent_incomplete_stream_is_not_success(self):
        for prompt in ('malformed-stream', 'truncated-stream'):
            self.assertEqual(self.stream(prompt)[-1]['stop_reason'], 'model_error')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--server-command', default='build/bin/native-mcp-test-server')
    parser.add_argument('--sdk-command', default='')
    options, remaining = parser.parse_known_args()
    native.COMMAND = shlex.split(options.server_command)
    native.SDK_COMMAND = shlex.split(options.sdk_command)
    unittest.main(argv=['agent_runtime_e2e'] + remaining)
