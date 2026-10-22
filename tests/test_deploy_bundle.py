import hashlib
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

spec = importlib.util.spec_from_file_location('bundle', Path(__file__).resolve().parents[1] / 'deploy/staging/download-bundle.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)
DATA = bytes(range(256)) * 1024


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/not-followed')
            self.end_headers()
            return
        with self.server.lock:
            self.server.attempts += 1
            attempt = self.server.attempts
        if self.path == '/retry' and attempt == 1:
            self.send_response(503)
            self.end_headers()
            return
        start, end = map(int, self.headers['Range'].removeprefix('bytes=').split('-'))
        self.send_response(200 if self.path == '/ignore-range' else 206)
        self.send_header('Content-Range', f'bytes {start}-{end}/{len(DATA)}')
        self.end_headers()
        try:
            self.wfile.write(DATA[start:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass


class BundleDownloadTests(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.attempts = 0
        self.server.lock = threading.Lock()
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.target = Path(self.temporary.name) / 'bundle.zip'

    def transfer(self, path='/', digest=None):
        bundle.download(f'http://127.0.0.1:{self.server.server_port}{path}', len(DATA),
                        digest or 'sha256:' + hashlib.sha256(DATA).hexdigest(), self.target,
                        workers=4, chunk_size=32768, timeout=5)

    def test_parallel_ranges_preserve_all_bytes(self):
        self.transfer()
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertEqual(self.server.attempts, 8)
        self.assertFalse(self.target.with_suffix('.partial').exists())

    def test_transient_failure_is_retried(self):
        self.transfer('/retry')
        self.assertEqual(self.target.read_bytes(), DATA)
        self.assertEqual(self.server.attempts, 9)

    def test_digest_failure_preserves_previous_file(self):
        self.target.write_bytes(b'previous')
        with self.assertRaisesRegex(RuntimeError, 'digest does not match'):
            self.transfer(digest='sha256:' + '0' * 64)
        self.assertEqual(self.target.read_bytes(), b'previous')
        self.assertFalse(self.target.with_suffix('.partial').exists())

    def test_server_ignoring_ranges_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'honor'):
            self.transfer('/ignore-range')
        self.assertFalse(self.target.exists())

    def test_bearer_request_does_not_follow_redirect(self):
        request = urllib.request.Request(f'http://127.0.0.1:{self.server.server_port}/redirect',
                                         headers={'Authorization': 'Bearer fixture'})
        with self.assertRaises(urllib.error.HTTPError) as result:
            urllib.request.build_opener(bundle.NoRedirect).open(request)
        self.assertEqual(result.exception.code, 302)
        result.exception.close()
        self.assertEqual(self.server.attempts, 0)


if __name__ == '__main__':
    unittest.main()
