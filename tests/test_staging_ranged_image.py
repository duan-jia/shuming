"""Protocol fault injection: no external registry, Docker, or real token."""
import base64
import contextlib
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ranged_image', ROOT / 'deploy/staging/ranged-image.py')
ranged = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ranged)
spec = importlib.util.spec_from_file_location('verify_image', ROOT / 'deploy/staging/verify-image.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
IMAGE = 'ghcr.io/duan-jia/shuming-backend:' + 'a' * 40
BASIC = base64.b64encode(b'fixture:FAKE_SECRET').decode()
sha = lambda body: 'sha256:' + hashlib.sha256(body).hexdigest()


class Response(io.BytesIO):
    def __init__(self, body=b'', status=200, headers=None, interrupted=False):
        super().__init__(body)
        self.status, self.headers = status, headers or {}
        self.interrupted, self.calls = interrupted, 0

    def read(self, size=-1):
        self.calls += 1
        if self.interrupted:
            if self.calls == 1:
                return super().read(min(7, size))
            raise OSError('https://storage.invalid/?sig=FAKE_SECRET')
        return super().read(size)


    def read1(self, size=-1):
        return self.read(size)


class Server:
    def __init__(self, fault=None):
        self.fault, self.requests, self.ranges = fault, [], []
        self.lock = threading.Lock()
        self.triggered = False
        layer = b'\0' * 2048
        if fault == 'large':
            import tarfile
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode='w') as archive:
                payload = os.urandom(ranged.CHUNK * 3 + 17)
                info = tarfile.TarInfo('fixture')
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))
            layer = stream.getvalue()
        compressed = gzip.compress(layer, mtime=0)
        config = json.dumps({'os': 'linux', 'architecture': 'amd64',
                             'rootfs': {'type': 'layers', 'diff_ids': [sha(layer)]}}).encode()
        self.blobs = {sha(config): config, sha(compressed): compressed}
        self.manifest = {'schemaVersion': 2, 'mediaType': 'application/vnd.oci.image.manifest.v1+json',
                         'config': {'digest': sha(config), 'size': len(config),
                                    'mediaType': 'application/vnd.oci.image.config.v1+json'},
                         'layers': [{'digest': sha(compressed), 'size': len(compressed),
                                     'mediaType': 'application/vnd.oci.image.layer.v1.tar+gzip'}]}
        self.raw = json.dumps(self.manifest).encode()

    def open(self, request, timeout):
        url, headers = request.full_url, dict(request.header_items())
        parsed = urlsplit(url)
        with self.lock:
            self.requests.append((url, headers, timeout))
        self.assert_safe(headers, parsed)
        if parsed.path == '/token':
            self.assert_equal(headers.get('Authorization'), 'Basic ' + BASIC)
            self.assert_equal(parse_qs(parsed.query)['scope'], ['repository:duan-jia/shuming-backend:pull'])
            return Response(json.dumps({'token': 'FAKE_BEARER', 'expires_in': 60}).encode())
        if parsed.hostname == 'ghcr.io' and headers.get('Authorization') != 'Bearer FAKE_BEARER':
            realm = 'https://evil.invalid/token' if self.fault == 'realm' else 'https://ghcr.io/token'
            return Response(status=401, headers={'WWW-Authenticate': 'Bearer realm="' + realm + '",service="ghcr.io",scope="repository:duan-jia/shuming-backend:pull"'})
        if '/manifests/' in parsed.path:
            if self.fault == 'platform':
                bad = json.loads(self.raw)
                bad['mediaType'] = 'application/vnd.oci.image.index.v1+json'
                bad['manifests'] = []
                return Response(json.dumps(bad).encode())
            return Response(self.raw, headers={'Docker-Content-Digest': sha(self.raw)})
        if '/blobs/' in parsed.path:
            digest = parsed.path.rsplit('/', 1)[-1]
            host = {'http': 'http://pkg-containers.githubusercontent.com',
                    'evil': 'https://evil.invalid',
                    'userinfo': 'https://secret@pkg-containers.githubusercontent.com',
                    'port': 'https://pkg-containers.githubusercontent.com:8443'}.get(self.fault, 'https://pkg-containers.githubusercontent.com')
            return Response(status=307, headers={'Location': host + '/' + digest + '?sig=FAKE_SECRET'})
        digest = parsed.path.rsplit('/', 1)[-1]
        body = self.blobs[digest]
        match = re.fullmatch(r'bytes=(\d+)-(\d+)', headers['Range'])
        start, end = map(int, match.groups())
        with self.lock:
            self.ranges.append((digest, start, end))
            first = not self.triggered
            self.triggered = True
        if self.fault == 'expired' and first:
            return Response(status=403)
        chunk = body[start:end + 1]
        if self.fault == 'corrupt':
            chunk = bytes([chunk[0] ^ 1]) + chunk[1:]
        return Response(chunk, status=200 if self.fault == 'ignored' else 206, headers={
            'Content-Range': f'bytes {start}-{end}/{len(body) + (1 if self.fault == "total" else 0)}',
            'Content-Length': str(len(chunk))}, interrupted=self.fault == 'interrupt' and first)

    @staticmethod
    def assert_safe(headers, parsed):
        if parsed.hostname != 'ghcr.io' and 'Authorization' in headers:
            raise AssertionError('Credential forwarded across origins')

    @staticmethod
    def assert_equal(left, right):
        if left != right:
            raise AssertionError('Wrong fixture authentication')


class RangedImageTests(unittest.TestCase):
    def transfer(self, fault=None, budget=None):
        server = Server(fault)
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            with contextlib.redirect_stdout(io.StringIO()) as output:
                ranged.transfer(IMAGE, work, BASIC, budget=budget, opener=server)
            verify.verify(work / 'image.tar', server.manifest)
            self.assertFalse((work / 'blobs').exists())
            self.assertNotIn('FAKE_SECRET', output.getvalue())
            return server, output.getvalue()

    def test_protocol_and_assembled_archive_full_diff_ids(self):
        server, _ = self.transfer()
        self.assertTrue(server.ranges)
        self.assertTrue(all(0 < timeout <= 10 for _, _, timeout in server.requests))

    def test_multiple_disjoint_ranges_and_final_whole_blob_sha(self):
        server, _ = self.transfer('large')
        self.assertGreater(len(server.ranges), 4)
        self.assertTrue(all(end - start + 1 <= ranged.CHUNK for _, start, end in server.ranges))
        self.assertEqual(len(server.ranges), len(set(server.ranges)))

    def test_interrupted_body_resumes_at_retained_offset(self):
        server, _ = self.transfer('interrupt')
        self.assertTrue(any(start == 7 for _, start, _ in server.ranges))

    def test_expired_address_restarts_from_authenticated_registry(self):
        server, _ = self.transfer('expired')
        self.assertGreaterEqual(len([url for url, _, _ in server.requests if '/blobs/' in url]), 3)

    def test_cross_origin_never_receives_authorization(self):
        server, _ = self.transfer()
        for url, headers, _ in server.requests:
            if urlsplit(url).hostname != 'ghcr.io':
                self.assertNotIn('Authorization', headers)

    def test_ignore_range_wrong_total_corrupt_blob_and_platform_fail_closed(self):
        for fault, label in [('ignored', 'range_status'), ('total', 'range_protocol'),
                             ('corrupt', 'blob_digest'), ('platform', 'platform')]:
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                work = Path(tmp)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ranged.Failure, label):
                    ranged.transfer(IMAGE, work, BASIC, opener=Server(fault))
                self.assertFalse((work / 'image.tar').exists())
                self.assertFalse((work / 'blobs').exists())

    def test_http_evil_userinfo_port_redirects_and_foreign_auth_realm_rejected(self):
        for fault in ('http', 'evil', 'userinfo', 'port', 'realm'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                server = Server(fault)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(ranged.Failure):
                    ranged.transfer(IMAGE, Path(tmp), BASIC, opener=server)
                self.assertFalse(any(urlsplit(url).hostname == 'evil.invalid' for url, _, _ in server.requests))

    def test_total_budget_exhaustion_never_publishes(self):
        clock = iter(range(10000))
        budget = ranged.Budget(2, clock=lambda: next(clock))
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ranged.Failure, 'deadline'):
                ranged.transfer(IMAGE, Path(tmp), BASIC, budget=budget, opener=Server())
            self.assertFalse((Path(tmp) / 'image.tar').exists())

    def test_only_current_private_job_auth_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            root.chmod(0o700)
            config = root / 'config.json'
            config.write_text(json.dumps({'auths': {'ghcr.io': {'auth': BASIC}}}))
            config.chmod(0o600)
            self.assertEqual(ranged.read_auth(root), BASIC)
            config.chmod(0o644)
            with self.assertRaises(ranged.Failure):
                ranged.read_auth(root)
            config.unlink()
            config.symlink_to('/dev/null')
            with self.assertRaises(ranged.Failure):
                ranged.read_auth(root)

    def test_cli_errors_never_echo_secret_arguments(self):
        result = subprocess.run([os.environ.get('PYTHON', 'python3'), str(ROOT / 'deploy/staging/ranged-image.py'),
                                 'https://FAKE_SECRET.invalid/?token=FAKE_SECRET', '/nonexistent'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('FAKE_SECRET', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

class RangedShellTests(unittest.TestCase):
    def run_shell(self, fault):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            private = root / 'auth'
            private.mkdir(mode=0o700)
            config = private / 'config.json'
            config.write_text(json.dumps({'auths': {'ghcr.io': {'auth': BASIC}}}))
            config.chmod(0o600)
            bins = root / 'bin'
            bins.mkdir()
            driver = root / 'driver.py'
            driver.write_text('''import sys
from unittest.mock import patch
sys.path.insert(0, sys.argv.pop(1))
from test_staging_ranged_image import ranged, Server
fault=sys.argv.pop(1)
with patch.object(ranged.urllib.request, 'build_opener', return_value=Server(None if fault=='ok' else fault)):
    raise SystemExit(ranged.main())
''')
            real_python = os.path.realpath(__import__('sys').executable)
            wrapper = bins / 'python3'
            wrapper.write_text('#!/bin/bash\nif [[ "$1" == *ranged-image.py ]]; then shift; exec "' + real_python +
                               '" "' + str(driver) + '" "' + str(ROOT / 'tests') +
                               '" "$FAULT" "$@"; fi\nexec "' + real_python + '" "$@"\n')
            wrapper.chmod(0o700)
            docker = bins / 'docker'
            docker.write_text('#!/bin/bash\necho "$1" >> "$TRACE"\nexit 0\n')
            docker.chmod(0o700)
            env = {**os.environ, 'PATH': str(bins) + ':' + os.environ['PATH'], 'FAULT': fault,
                   'TRACE': str(root / 'trace'), 'DOCKER_CONFIG': str(private), 'STAGING_IMAGE_TRANSPORT': 'range'}
            result = subprocess.run(['bash', '-c', 'set -Eeuo pipefail; source "$1"; prepare_crane(){ exit 99; }; pull_private_image "$2"',
                                     '_', str(ROOT / 'deploy/staging/pull-images.sh'), IMAGE],
                                    env=env, capture_output=True, text=True)
            self.assertNotIn('FAKE_SECRET', result.stdout + result.stderr)
            self.assertEqual(set(private.iterdir()), {config}, 'only original job auth remains for outer cleanup')
            return result, (root / 'trace').read_text().splitlines() if (root / 'trace').exists() else []

    def test_range_selector_uses_current_job_auth_then_full_verify_and_import(self):
        result, trace = self.run_shell('ok')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Image archive verified', result.stdout)
        self.assertIn('load', trace)
        self.assertIn('create', trace)
        self.assertIn('stage=ranges rc=0', result.stderr)

    def test_protocol_failure_never_falls_back_or_imports(self):
        result, trace = self.run_shell('ignored')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(trace, [])
        self.assertIn('category=range_status', result.stderr)
