#!/usr/bin/env python3
"""Run the full backend against an isolated MySQL and local model/search fixtures.

Requires a fresh database initialized with sql/init.sql. No real model credentials.
MYSQL_TEST_HOST=<isolated host> python3 tests/agent_history_e2e.py
"""
import concurrent.futures
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
import uuid


class Fixture(BaseHTTPRequestHandler):
    model_requests = []
    searches = []
    slow_started = threading.Event()

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/search':
            self.searches.append(body['query'])
            if body['query'] == 'slow':
                self.slow_started.set()
                time.sleep(4)
            result = {'results': [{'title': 'Fixture source', 'url': 'https://example.org/source',
                                   'content': 'source evidence ' * 500}]}
            data = json.dumps(result).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        assert self.path == '/v1/chat/completions', self.path
        assert self.headers['Authorization'] == 'Bearer fixture-model-key'
        self.model_requests.append(body)
        messages = body['messages']
        user_index = max(i for i, m in enumerate(messages) if m['role'] == 'user')
        prompt = messages[user_index]['content']
        current_tools = [m for m in messages[user_index + 1:] if m['role'] == 'tool']
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Connection', 'close')
        self.end_headers()
        if prompt.startswith('inspect') or current_tools:
            delta, finish = {'content': 'Fixture final answer'}, 'stop'
        else:
            queries = ['slow', 'must-not-execute'] if prompt == 'cancel-batch' else ['slow'] if prompt == 'crash' else ['evidence']
            calls = []
            for i, query in enumerate(queries):
                calls.append({'index': i, 'id': 'call-' + str(i), 'type': 'function', 'function': {
                    'name': 'search_web', 'arguments': '{bad' if prompt == 'invalid' else json.dumps({'query': query, 'need_content': True})}})
            delta, finish = {'tool_calls': calls, 'reasoning_content': 'Fixture reasoning'}, 'tool_calls'
        chunks = [{'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]},
                  {'choices': [{'index': 0, 'delta': {}, 'finish_reason': finish}]},
                  {'choices': [], 'usage': {'prompt_tokens': 100, 'completion_tokens': 20}}]
        try:
            for chunk in chunks:
                self.wfile.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    host = os.environ['MYSQL_TEST_HOST']
    root = Path(__file__).resolve().parents[1]
    fixture = ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    port = free_port()
    base = f'http://127.0.0.1:{port}'
    token = ''

    def api(method, path, body=None, auth=None):
        req = urllib.request.Request(base + path, method=method,
            data=None if body is None else json.dumps(body).encode(),
            headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + (token if auth is None else auth)})
        try:
            with opener.open(req, timeout=15) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def stream(session, prompt, request_id=None):
        request_id = request_id or str(uuid.uuid4())
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=15)
        conn.request('POST', '/api/chat/stream', json.dumps({'session_id': session, 'message': prompt}),
                     {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json', 'X-Request-Id': request_id})
        events = []
        try:
            response = conn.getresponse()
            for line in response:
                if line.startswith(b'data: ') and line.strip() != b'data: [DONE]':
                    events.append(json.loads(line[6:]))
        finally:
            conn.close()
        return request_id, events

    with tempfile.TemporaryDirectory(prefix='agent-history-e2e-') as directory:
        directory = Path(directory)
        config = {
            'mysql': {'host': host, 'port': int(os.environ.get('MYSQL_TEST_PORT', '3306')),
                      'user': 'root', 'password': os.environ['MYSQL_TEST_PASSWORD'], 'database': 'llm_platform', 'pool_size': 4},
            'jwt': {'secret': 'fixture-jwt-secret-with-at-least-32-bytes', 'expiry_seconds': 3600},
            'admin': {'default_password': 'fixture-pass1234'},
            'rag': {'storage_dir': str(directory / 'rag'), 'query_rewrite': {'enabled': False}},
            'remote_engine': {'endpoint': f'http://127.0.0.1:{fixture.server_port}/v1',
                              'model': 'fixture-model', 'api_key': 'fixture-model-key', 'context_window_tokens': 32768},
            'web_search': {'enabled': True, 'endpoint': f'http://127.0.0.1:{fixture.server_port}/search',
                           'api_key': 'fixture-search-key', 'timeout_seconds': 10},
            'agent': {'timeout_seconds': 12, 'max_tool_result_bytes': 4096},
            'rate_limiter': {'rules': [{'action': 'chat', 'max_requests': 200, 'window_seconds': 60}]},
        }
        config_path = directory / 'config.json'
        config_path.write_text(json.dumps(config))
        log = (directory / 'backend.log').open('w+')
        process = None

        def start():
            proc = subprocess.Popen([str(root / 'build/bin/fast-llm-reasoner'), '--config', str(config_path), '--port', str(port)],
                                    cwd=directory, stdout=log, stderr=subprocess.STDOUT)
            for _ in range(100):
                if proc.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read()[-5000:])
                try:
                    if api('GET', '/api/health')[0] == 200:
                        return proc
                except (OSError, urllib.error.URLError):
                    time.sleep(.05)
            proc.kill()
            proc.wait()
            raise AssertionError('Backend did not start')

        try:
            process = start()
            status, login = api('POST', '/api/auth/login', {'username': 'admin', 'password': 'fixture-pass1234'})
            assert status == 200, login
            token = login['token']
            status, created = api('POST', '/api/sessions', {'title': 'Agent history fixture'})
            assert status == 200, created
            session = created['id']
            request, events = stream(session, 'search')
            assert events[-1]['type'] == 'done' and events[-1]['trace']['persisted'], events
            status, trace = api('GET', f'/api/sessions/{session}/requests/{request}')
            assert status == 200, trace
            rows = trace['events']
            assert [e['data']['role'] for e in rows if e['type'] == 'message'] == ['user', 'assistant', 'tool', 'assistant']
            tool = next(e['data'] for e in rows if e['type'] == 'tool_end')
            assert tool['execution_started'] and not tool['is_error'] and tool['duration_ms'] >= 0
            assert len(tool['raw_result']['results'][0]['content']) > len(json.loads(tool['model_content'])['results'][0]['content'])
            assert 'fixture-model-key' not in json.dumps(trace) and 'fixture-search-key' not in json.dumps(trace)
            print('PASS: durable model requests, tool arguments, raw/delivered results, timing, redaction')

            process.terminate()
            process.wait(timeout=5)
            process = start()
            _, events = stream(session, 'inspect-history')
            assert events[-1]['type'] == 'done', events
            sent = Fixture.model_requests[-1]['messages']
            assert [m['role'] for m in sent] == ['system', 'user', 'assistant', 'tool', 'assistant', 'user'], sent
            assert sent[2]['tool_calls'][0]['id'] == sent[3]['tool_call_id']
            assert sent[2]['reasoning_content'] == 'Fixture reasoning'
            assert sent[1]['content'] == 'search' and sent[-1]['content'] == 'inspect-history'
            print('PASS: restart restores actual call/result messages without duplicating display history')

            request, events = stream(session, 'invalid')
            assert events[-1]['type'] == 'done', events
            trace = api('GET', f'/api/sessions/{session}/requests/{request}')[1]
            tool = next(e['data'] for e in trace['events'] if e['type'] == 'tool_end')
            assert tool['is_error'] and not tool['execution_started'] and tool['stage'] == 'arguments', tool
            print('PASS: invalid arguments retained as failed calls, not successful execution')

            Fixture.slow_started.clear()
            cancel_id = str(uuid.uuid4())
            with concurrent.futures.ThreadPoolExecutor() as pool:
                pending = pool.submit(stream, session, 'cancel-batch', cancel_id)
                assert Fixture.slow_started.wait(3)
                assert api('POST', '/api/chat/cancel', {'session_id': session, 'request_id': cancel_id})[0] == 200
                _, events = pending.result(timeout=5)
            assert events[-1]['stop_reason'] == 'cancelled', events
            trace = api('GET', f'/api/sessions/{session}/requests/{cancel_id}')[1]
            tools = [e['data'] for e in trace['events'] if e['type'] == 'tool_end']
            assert len(tools) == 2 and not tools[1]['execution_started'] and 'must-not-execute' not in Fixture.searches
            print('PASS: cancellation closes the batch and never executes remaining calls')

            Fixture.slow_started.clear()
            crash_id = str(uuid.uuid4())
            with concurrent.futures.ThreadPoolExecutor() as pool:
                pending = pool.submit(stream, session, 'crash', crash_id)
                assert Fixture.slow_started.wait(3)
                process.kill()
                process.wait(timeout=5)
                try:
                    pending.result(timeout=5)
                except (OSError, http.client.HTTPException):
                    pass
            before_searches = len(Fixture.searches)
            process = start()
            _, events = stream(session, 'inspect-crash')
            assert events[-1]['type'] == 'done', events
            sent = Fixture.model_requests[-1]['messages']
            crash_index = next(i for i, m in enumerate(sent) if m['role'] == 'user' and m['content'] == 'crash')
            assert sent[crash_index + 2]['role'] == 'tool'
            assert 'outcome is unknown' in sent[crash_index + 2]['content']
            assert len(Fixture.searches) == before_searches
            print('PASS: hard crash recovery reports unknown outcome without retrying the tool')

            username = 'trace-other-user-' + uuid.uuid4().hex[:8]
            status, member = api('POST', '/api/users', {'username': username, 'password': 'fixture-pass1234', 'tenant_id': 1})
            assert status == 200, member
            status, other = api('POST', '/api/auth/login', {'username': username, 'password': 'fixture-pass1234'})
            assert status == 200, other
            assert api('GET', f'/api/sessions/{session}/requests', auth=other['token'])[0] == 404
            assert api('GET', f'/api/sessions/{session}/requests/{request}', auth=other['token'])[0] == 404
            assert api('GET', f'/api/sessions/{session}/requests/{request}', auth='invalid')[0] == 401
            unknown = str(uuid.uuid4())
            assert api('GET', f'/api/sessions/{session}/requests/{unknown}')[0] == 404
            print('PASS: trace access is limited to the authenticated session owner')

            config['agent']['enabled'] = False
            config_path.write_text(json.dumps(config))
            process.terminate()
            process.wait(timeout=5)
            process = start()
            status, direct = api('POST', '/api/sessions', {'title': 'Non-Agent journal fixture'})
            assert status == 200, direct
            direct_id, events = stream(direct['id'], 'inspect-direct')
            assert events[-1]['type'] == 'done' and events[-1]['trace']['persisted'], events
            trace = api('GET', f"/api/sessions/{direct['id']}/requests/{direct_id}")[1]
            assert [e['data']['role'] for e in trace['events'] if e['type'] == 'message'] == ['user', 'assistant']
            assert not any(e['type'] == 'tool_start' for e in trace['events'])
            print('PASS: ordinary non-Agent chat also retains its transcript and request JSON')
        finally:
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            log.close()
            fixture.shutdown()
            fixture.server_close()


if __name__ == '__main__':
    main()
