#!/usr/bin/env python3
"""Exercise native MCP over real HTTP, without production DB/models/API keys.

python3 tests/native_mcp_e2e.py --server-command build/bin/native-mcp-test-server
The command receives two appended arguments: listen port and fake provider URL.
"""
import argparse
import concurrent.futures
import http.client
import json
import os
import shlex
import socket
import subprocess
import threading
import time
import tempfile
import base64
import struct
import urllib.parse
from pathlib import Path
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

COMMAND = []
SDK_COMMAND = []
SERVER_EXTRA_ARGS = []


def fixture_pdf():
    """Small real, four-page PDF; page 1 exercises text pagination, page 3 is blank."""
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R 10 0 R] /Count 4 >>',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    for page in range(1, 5):
        stream_id = len(objects) + 2
        objects.append(f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 3 0 R >> >> /Contents {stream_id} 0 R >>'.encode())
        lines = ([f'Original PDF page one row {i}: ' + 'source evidence ' * 5 for i in range(60)] if page == 1 else
                 [] if page == 3 else [f'Original PDF page {page}: Figure 1 source evidence'])
        stream = ('BT /F1 4 Tf 20 780 Td 12 TL ' + ' '.join(f'({line}) Tj T*' for line in lines) + ' ET').encode()
        objects.append(f'<< /Length {len(stream)} >>\nstream\n'.encode() + stream + b'\nendstream')
    output = b'%PDF-1.4\n'
    offsets = [0]
    for index, item in enumerate(objects, 1):
        offsets.append(len(output))
        output += f'{index} 0 obj\n'.encode() + item + b'\nendobj\n'
    xref = len(output)
    output += f'xref\n0 {len(objects) + 1}\n0000000000 65535 f \n'.encode()
    output += b''.join(f'{offset:010d} 00000 n \n'.encode() for offset in offsets[1:])
    output += f'trailer << /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
    return output


class Provider(BaseHTTPRequestHandler):
    started = threading.Event()
    received = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers['content-length'])))
        self.received.append((self.path, self.headers.get('authorization'), data))
        if self.path.endswith('/fetch'):
            query = data['url'].rsplit('/', 1)[-1]
            if query == 'timeout':
                time.sleep(1.8)
            if query == 'error':
                self.send_response(429)
                self.end_headers()
                self.wfile.write(b'secret-provider-error')
                return
            body = (b'{"content": 1}' if query == 'malformed' else b'x' * (2 * 1024 * 1024 + 1) if query == 'huge' else
                    json.dumps({'title': 'Fetched page', 'content': '# Original fetched Markdown', 'provider_secret': 'must-not-leak'}).encode())
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        query = data['query']
        if query in ('slow', 'timeout'):
            self.started.set()
            time.sleep(.65 if query == 'slow' else 1.8)
        if query == 'error':
            self.send_response(429)
            self.end_headers()
            self.wfile.write(b'secret-provider-error')
            return
        if query == 'malformed':
            body = b'{"results": "bad"}'
        elif query == 'huge':
            body = b'x' * (2 * 1024 * 1024 + 1)
        elif query == 'empty':
            body = b'{"results": []}'
        else:
            body = json.dumps({'results': [{'title': 'Search evidence', 'url': 'https://example.org/paper',
                                           'summary': 'Source summary', 'content': 'Source content' if data['need_content'] else '',
                                           'provider_secret': 'must-not-leak'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class NativeMcpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        threading.Thread(target=cls.provider.serve_forever, daemon=True).start()
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            cls.port = sock.getsockname()[1]
        cls.pdf_directory = tempfile.TemporaryDirectory(prefix='fetch-fixture-')
        cls.addClassCleanup(cls.pdf_directory.cleanup)
        cls.pdf_bytes = fixture_pdf()
        cls.pdf_path = Path(cls.pdf_directory.name) / '00000000-0000-0000-0000-000000000001.pdf'
        cls.pdf_path.write_bytes(cls.pdf_bytes)
        cls.process = subprocess.Popen(COMMAND + [str(cls.port), f'http://127.0.0.1:{cls.provider.server_port}/v1/tools/search'] + SERVER_EXTRA_ARGS,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=dict(os.environ, FIXTURE_PDF_DIR=cls.pdf_directory.name))
        cls.addClassCleanup(cls.cleanup_process)
        cls.addClassCleanup(cls.provider.shutdown)
        cls.addClassCleanup(cls.provider.server_close)
        cls.tokens = {}
        for line in cls.process.stdout:
            for key in ('ADMIN_TOKEN', 'USER_TOKEN', 'GUEST_TOKEN'):
                if line.startswith(key + '='):
                    cls.tokens[key] = line.strip().split('=', 1)[1]
            if len(cls.tokens) == 3:
                break
        if len(cls.tokens) != 3:
            raise RuntimeError('Fixture failed to start')
        # Drain logs so pipe capacity cannot stall a worker in concurrency tests.
        cls.logs = []
        threading.Thread(target=lambda: cls.logs.extend(cls.process.stdout), daemon=True).start()
        for _ in range(100):
            try:
                if cls.request('GET', '/healthz')[0] == 200:
                    return
            except OSError:
                time.sleep(.05)
        raise RuntimeError('Fixture did not become ready')

    @classmethod
    def cleanup_process(cls):
        cls.process.terminate()
        try:
            cls.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait()

    @classmethod
    def request(cls, method, path='/mcp', payload=None, token='ADMIN_TOKEN', headers=None, raw=None):
        conn = http.client.HTTPConnection('127.0.0.1', cls.port, timeout=5)
        request_headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream',
                           'MCP-Protocol-Version': '2025-11-25'}
        if token:
            request_headers['Authorization'] = 'Bearer ' + cls.tokens.get(token, token)
        request_headers.update(headers or {})
        body = raw if raw is not None else json.dumps(payload) if payload is not None else None
        try:
            conn.request(method, path, body=body, headers=request_headers)
            response = conn.getresponse()
            content = response.read()
            return response.status, dict(response.getheaders()), content if response.getheader('Content-Type') == 'application/pdf' else json.loads(content) if content else None
        finally:
            conn.close()

    def rpc(self, method, params=None, **kwargs):
        return self.request('POST', payload={'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params or {}}, **kwargs)

    def call(self, name, arguments, **kwargs):
        return self.rpc('tools/call', {'name': name, 'arguments': arguments}, **kwargs)

    def test_initialize_and_notifications(self):
        for path in ('/mcp', '/mcp/knowledge', '/mcp/web'):
            status, headers, body = self.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                                          'clientInfo': {'name': 'test', 'version': '1'}}, path=path)
            self.assertEqual(status, 200)
            self.assertEqual(headers['Content-Type'], 'application/json')
            self.assertEqual(body['result']['protocolVersion'], '2025-11-25')
            self.assertNotIn('Mcp-Session-Id', headers)
        status, _, body = self.request('POST', payload={'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.assertEqual((status, body), (202, None))
        self.assertEqual(self.rpc('ping')[2]['result'], {})
        for method in ('GET', 'DELETE', 'PUT'):
            self.assertEqual(self.request(method)[0], 405)

    def test_official_sdk(self):
        if not SDK_COMMAND:
            self.skipTest('Pass --sdk-command to exercise the official MCP SDK')
        env = dict(os.environ, NATIVE_MCP_URL=f'http://127.0.0.1:{self.port}', NATIVE_MCP_TOKEN=self.tokens['ADMIN_TOKEN'])
        subprocess.run(SDK_COMMAND, check=True, timeout=30, env=env)

    def test_scoped_endpoints(self):
        expected = {'/mcp': ['search_knowledge', 'fetch_knowledge', 'search_web', 'fetch_web'], '/mcp/knowledge': ['search_knowledge', 'fetch_knowledge'], '/mcp/web': ['search_web', 'fetch_web']}
        for path, names in expected.items():
            self.assertEqual([tool['name'] for tool in self.rpc('tools/list', path=path)[2]['result']['tools']], names)
        self.assertEqual(self.call('search_web', {'query': 'test'}, path='/mcp/knowledge')[2]['error']['code'], -32602)
        self.assertEqual(self.call('search_knowledge', {'query': 'test'}, path='/mcp/web')[2]['error']['code'], -32602)

    def test_auth_origin_and_permissions(self):
        self.assertEqual(self.rpc('tools/list', token=None)[0], 401)
        self.assertEqual(self.rpc('tools/list', token='invalid')[0], 401)
        for path in ('/mcp', '/mcp/knowledge', '/mcp/web'):
            self.assertEqual(self.rpc('tools/list', path=path, token='GUEST_TOKEN')[0], 200)
        self.assertEqual(self.rpc('tools/list', headers={'Origin': 'https://evil.example'})[0], 403)
        status, headers, _ = self.rpc('tools/list', headers={'Origin': 'https://allowed.example'})
        self.assertEqual(status, 200)
        self.assertEqual(headers['Access-Control-Allow-Origin'], 'https://allowed.example')
        self.assertEqual(self.rpc('tools/list', path='/mcp/web', token='USER_TOKEN')[2]['result']['tools'], [])
        self.assertEqual(self.call('search_web', {'query': 'test'}, token='USER_TOKEN')[2]['error']['code'], -32602)
        self.assertEqual(self.rpc('tools/list', path='/mcp/web', token='GUEST_TOKEN')[2]['result']['tools'], [])
        self.assertEqual(self.call('search_web', {'query': 'test'}, token='GUEST_TOKEN')[2]['error']['code'], -32602)
        self.assertEqual(self.request('OPTIONS', headers={'Origin': 'https://evil.example'})[0], 403)
        self.assertEqual(self.request('OPTIONS', token=None, headers={'Origin': 'https://allowed.example'})[0], 204)

    def test_tenant_and_shared_evidence(self):
        for token, tenant in [('ADMIN_TOKEN', 7), ('USER_TOKEN', 8), ('GUEST_TOKEN', 9)]:
            for mode in ('chunks', 'parents'):
                body = self.call('search_knowledge', {'query': '论文', 'mode': mode}, token=token)[2]['result']
                self.assertFalse(body['isError'])
                evidence = body['structuredContent']['results'][0]
                self.assertEqual(evidence['doc_id'], tenant)
                self.assertEqual(evidence['filename'], f'tenant-{tenant}.md')
                self.assertIn('matched_chunks' if mode == 'parents' else 'chunk_index', evidence)
                self.assertEqual(json.loads(body['content'][0]['text']), body['structuredContent'])
        injected = self.call('search_knowledge', {'query': 'test', 'tenant_id': 99})[2]
        self.assertEqual(injected['error']['code'], -32602)

    def test_web_provider_contract(self):
        result = self.call('search_web', {'query': '论文搜索', 'need_content': True, 'count': 3, 'ori_question': '原问题'})[2]['result']
        self.assertFalse(result['isError'])
        self.assertEqual(result['structuredContent']['results'][0]['content'], 'Source content')
        self.assertNotIn('provider_secret', json.dumps(result))
        path, authorization, data = Provider.received[-1]
        self.assertEqual(path, '/v1/tools/search')
        self.assertEqual(authorization, 'Bearer fixture-provider-key')
        self.assertEqual(data['count'], 3)
        self.assertEqual(data['ori_question'], '原问题')
        self.assertEqual(self.call('search_web', {'query': 'empty'})[2]['result']['structuredContent']['results'], [])
        self.assertEqual(self.call('search_web', {'query': '中' * 128})[2]['result']['isError'], False)

    def test_fetch_web_contract(self):
        result = self.call('fetch_web', {'url': 'https://example.org/paper'})[2]['result']
        self.assertFalse(result['isError'])
        self.assertEqual(result['structuredContent']['content'], '# Original fetched Markdown')
        self.assertNotIn('provider_secret', json.dumps(result))
        path, authorization, data = Provider.received[-1]
        self.assertEqual((path, authorization, data), ('/v1/tools/fetch', 'Bearer fixture-provider-key', {'url': 'https://example.org/paper'}))
        for url in ('file:///etc/passwd', 'http://user:pass@example.org/', 'https://example.org/\n', 'https://', 'http://example.org/a b'):
            self.assertEqual(self.call('fetch_web', {'url': url})[2]['error']['code'], -32602)
        self.assertEqual(self.call('fetch_web', {'url': 'https://example.org', 'endpoint': 'http://evil'})[2]['error']['code'], -32602)
        self.assertEqual(self.call('fetch_web', {'url': 'https://example.org'}, token='USER_TOKEN')[2]['error']['code'], -32602)
        self.assertEqual(self.call('fetch_web', {'url': 'https://example.org'}, path='/mcp/knowledge')[2]['error']['code'], -32602)
        for suffix in ('malformed', 'error', 'huge', 'timeout'):
            result = self.call('fetch_web', {'url': 'https://example.org/' + suffix})[2]['result']
            self.assertTrue(result['isError'])
            self.assertNotIn('secret-provider-error', json.dumps(result))

    def test_fetch_knowledge_pdf_text_pages_and_tenant(self):
        args = {'doc_id': 7, 'representation': 'text', 'start_page': 1, 'page_count': 1}
        result = self.call('fetch_knowledge', args)[2]['result']
        self.assertFalse(result['isError'])
        data = result['structuredContent']
        self.assertEqual(data['total_pages'], 4)
        self.assertIn('Original PDF page one', data['content'])
        self.assertNotIn('Tenant evidence', data['content'])
        self.assertIsNotNone(data['next_offset'])
        following = self.call('fetch_knowledge', {**args, 'offset': data['next_offset']})[2]['result']['structuredContent']
        self.assertIsNone(following['next_offset'])
        self.assertEqual(following['next_page'], 2)
        blank = self.call('fetch_knowledge', {**args, 'start_page': 3})[2]['result']['structuredContent']
        self.assertFalse(blank['text_available'])
        result = self.call('fetch_knowledge', {**args, 'representation': 'pages', 'start_page': 2})[2]['result']
        self.assertFalse(result['isError'])
        image = result['content'][1]
        self.assertEqual((image['type'], image['mimeType']), ('image', 'image/png'))
        png = base64.b64decode(image['data'])
        self.assertTrue(png.startswith(b'\x89PNG\r\n\x1a\n'))
        self.assertEqual(max(struct.unpack('>II', png[16:24])), 1600)
        self.assertNotIn('data', result['structuredContent'])
        self.assertTrue(self.call('fetch_knowledge', {'doc_id': 7}, token='USER_TOKEN')[2]['result']['isError'])
        self.assertFalse(self.call('fetch_knowledge', {'doc_id': 8, 'page_count': 1}, token='USER_TOKEN')[2]['result']['isError'])
        self.assertTrue(self.call('fetch_knowledge', {'doc_id': 99})[2]['result']['isError'])
        self.assertEqual(self.call('fetch_knowledge', {'doc_id': 7}, path='/mcp/web')[2]['error']['code'], -32602)
        for args in ({'doc_id': 7, 'tenant_id': 8}, {'doc_id': 7, 'representation': 'bad'}, {'doc_id': 7, 'page_count': 4}, {'doc_id': True}):
            self.assertEqual(self.call('fetch_knowledge', args)[2]['error']['code'], -32602)
        self.assertTrue(self.call('fetch_knowledge', {'doc_id': 7, 'start_page': 9})[2]['result']['isError'])

    def test_fetch_knowledge_rejects_symlinks_and_invalid_pdf(self):
        alternate = self.pdf_path.with_name('alternate.pdf')
        alternate.write_bytes(self.pdf_bytes)
        self.pdf_path.unlink()
        self.pdf_path.symlink_to(alternate)
        try:
            self.assertTrue(self.call('fetch_knowledge', {'doc_id': 7})[2]['result']['isError'])
        finally:
            self.pdf_path.unlink()
            self.pdf_path.write_bytes(self.pdf_bytes)
            alternate.unlink()
        self.pdf_path.write_bytes(b'%PDF-corrupt but has expected magic')
        try:
            self.assertTrue(self.call('fetch_knowledge', {'doc_id': 7})[2]['result']['isError'])
        finally:
            self.pdf_path.write_bytes(self.pdf_bytes)

    def test_fetch_knowledge_signed_download(self):
        result = self.call('fetch_knowledge', {'doc_id': 7, 'representation': 'source'})[2]['result']
        data = result['structuredContent']
        self.assertFalse(result['isError'])
        self.assertEqual(result['content'][1]['type'], 'resource_link')
        url = urllib.parse.urlsplit(data['download_url'])
        path = url.path + '?' + url.query
        status, headers, content = self.request('GET', path, token=None)
        self.assertEqual(status, 200)
        self.assertEqual(content, self.pdf_bytes)
        self.assertIn('attachment;', headers['Content-Disposition'])
        self.assertEqual(headers['Cache-Control'], 'private, no-store')
        self.assertEqual(self.request('GET', path.replace('doc_id=7', 'doc_id=8'), token=None)[0], 403)
        self.assertEqual(self.request('GET', path.replace('tenant_id=7', 'tenant_id=8'), token=None)[0], 403)
        self.assertEqual(self.request('GET', '/api/knowledge/download', token=None)[0], 403)
        self.pdf_path.unlink()
        try:
            self.assertEqual(self.request('GET', path, token=None)[0], 404)
        finally:
            self.pdf_path.write_bytes(self.pdf_bytes)
        # A validly signed URL must stop working after its short fixture lifetime.
        time.sleep(3)
        self.assertEqual(self.request('GET', path, token=None)[0], 403)

    def test_protocol_and_argument_errors(self):
        self.assertEqual(self.request('POST', raw='{')[2]['error']['code'], -32700)
        for payload in ([], {'jsonrpc': '2.0', 'method': 42}, {'jsonrpc': '2.0', 'id': [], 'method': 'ping'}):
            self.assertEqual(self.request('POST', payload=payload)[2]['error']['code'], -32600)
        self.assertEqual(self.rpc('unknown')[2]['error']['code'], -32601)
        self.assertEqual(self.rpc('tools/list', headers={'MCP-Protocol-Version': 'bad'})[0], 400)
        self.assertEqual(self.rpc('tools/list', headers={'Accept': 'application/json'})[0], 406)
        self.assertEqual(self.rpc('tools/list', headers={'Content-Type': 'text/plain'})[0], 415)
        self.assertEqual(self.request('POST', raw='x' * 65537)[0], 413)
        for name, args in [('search_web', {'query': '中' * 129}), ('search_web', {'query': ' '}),
                           ('search_web', {'query': 'x', 'count': 1.5}), ('search_web', {'query': 'x', 'need_content': 'true'}),
                           ('search_web', {'query': 'x', 'endpoint': 'http://evil'}), ('search_knowledge', {'query': 'x', 'top_k': 21}),
                           ('search_knowledge', {'query': 'x', 'mode': 'bad'}), ('unknown', {})]:
            self.assertEqual(self.call(name, args)[2]['error']['code'], -32602)

    def test_upstream_failures_are_tool_errors(self):
        for query in ('error', 'malformed', 'huge', 'timeout'):
            status, _, body = self.call('search_web', {'query': query})
            self.assertEqual(status, 200)
            self.assertTrue(body['result']['isError'])
            self.assertNotIn('secret-provider-error', json.dumps(body))
            self.assertNotIn('fixture-provider-key', json.dumps(body))

    def test_slow_search_does_not_block_network_or_knowledge(self):
        Provider.started.clear()
        with concurrent.futures.ThreadPoolExecutor() as pool:
            slow = pool.submit(self.call, 'search_web', {'query': 'slow'})
            self.assertTrue(Provider.started.wait(2))
            started = time.monotonic()
            self.assertEqual(self.request('GET', '/healthz')[0], 200)
            self.assertFalse(self.call('search_knowledge', {'query': 'test'})[2]['result']['isError'])
            self.assertLess(time.monotonic() - started, .4)
            self.assertTrue(self.call('search_web', {'query': 'busy'})[2]['result']['isError'])
            self.assertFalse(slow.result()[2]['result']['isError'])

    def test_bounded_queue_rejects_excess_work(self):
        barrier = threading.Barrier(8)
        def query():
            barrier.wait(timeout=3)
            return self.call('search_knowledge', {'query': 'slow-knowledge'})
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            work = [pool.submit(query) for _ in range(8)]
            results = [item.result() for item in work]
        statuses = [item[0] for item in results]
        self.assertIn(200, statuses)
        self.assertIn(503, statuses)
        self.assertTrue(all(status in (200, 503) for status in statuses))
        for status, headers, _ in results:
            if status == 503:
                self.assertEqual(headers['Retry-After'], '1')
        self.assertFalse(self.call('search_knowledge', {'query': 'recovered'})[2]['result']['isError'])

    def test_z_rate_limit_stops_provider_requests(self):
        results = [self.call('search_web', {'query': 'empty'}) for _ in range(35)]
        self.assertTrue(results[-1][2]['result']['isError'])
        self.assertIn('rate limit', results[-1][2]['result']['content'][0]['text'])
        count = len(Provider.received)
        self.call('search_web', {'query': 'limited'})
        self.assertEqual(len(Provider.received), count)

    def test_keep_alive_and_pipelined_mcp_requests(self):
        payload = json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'ping'})
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as conn:
            request = (f'POST /mcp HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {self.tokens["ADMIN_TOKEN"]}\r\n'
                       f'Accept: application/json, text/event-stream\r\nContent-Type: application/json\r\n'
                       f'Content-Length: {len(payload)}\r\n\r\n{payload}').encode()
            conn.sendall(request + request)
            result = b''
            while result.count(b'"result":{}') < 2:
                chunk = conn.recv(4096)
                self.assertTrue(chunk)
                result += chunk
            self.assertEqual(result.count(b'HTTP/1.1 200'), 2)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--server-command', default='build/bin/native-mcp-test-server')
    parser.add_argument('--sdk-command', default='')
    options, remaining = parser.parse_known_args()
    COMMAND = shlex.split(options.server_command)
    SDK_COMMAND = shlex.split(options.sdk_command)
    unittest.main(argv=['native_mcp_e2e'] + remaining)
