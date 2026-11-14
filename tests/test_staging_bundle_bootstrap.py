"""Offline regression tests for the pre-checkout downloader bootstrap in CI."""
import base64
import io
import json
import os
from pathlib import Path
import ssl
import tempfile
import unittest
import urllib.error
from unittest.mock import patch, Mock

REPO = Path(__file__).resolve().parents[1]


def bootstrap():
    text = (REPO / '.github/workflows/staging-e2e.yml').read_text()
    block = text.split("          python3 - <<'PY'\n", 1)[1].split('          PY\n', 1)[0]
    return '\n'.join(line[10:] for line in block.splitlines())


class BootstrapTests(unittest.TestCase):
    def run_bootstrap(self, failures, expected_error=None):
        payload = b'# test fixture, never executed\n'
        response = io.BytesIO(json.dumps({'content': base64.b64encode(payload).decode()}).encode())
        opener = Mock()
        opener.open.side_effect = [*failures, response]
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, {
            'GITHUB_REPOSITORY': 'example/repo', 'GITHUB_SHA': 'a' * 40,
            'GITHUB_TOKEN': 'fixture-not-a-secret', 'BUNDLE_DIR': str(Path(root) / 'bundle'),
        }), patch('urllib.request.build_opener', return_value=opener) as factory, \
                patch('time.sleep') as sleep, patch('builtins.print') as output:
            if expected_error:
                with self.assertRaisesRegex(SystemExit, expected_error):
                    exec(bootstrap(), {})
                self.assertFalse((Path(root) / 'bundle/download-bundle.py').exists())
            else:
                exec(bootstrap(), {})
                self.assertEqual((Path(root) / 'bundle/download-bundle.py').read_bytes(), payload)
            # No redirect-following handler or unbounded network attempts.
            handler = factory.call_args.args[0]()
            self.assertIsNone(handler.redirect_request(None, None, 302, '', {}, 'https://other.invalid'))
            self.assertTrue(all(call.kwargs['timeout'] == 30 for call in opener.open.call_args_list))
            self.assertNotIn('fixture-not-a-secret', str(output.call_args_list))
            return opener.open.call_count, sleep.call_count

    def test_success_uses_expected_bundle_location_without_retry(self):
        self.assertEqual(self.run_bootstrap([]), (1, 0))

    def test_logged_tls_eof_then_recovery(self):
        eof = urllib.error.URLError(ssl.SSLEOFError('UNEXPECTED_EOF_WHILE_READING'))
        self.assertEqual(self.run_bootstrap([eof]), (2, 1))

    def test_retryable_http_error_then_recovery(self):
        error = urllib.error.HTTPError('https://example.invalid', 502, 'bad gateway', {}, None)
        self.assertEqual(self.run_bootstrap([error]), (2, 1))

    def test_permanent_failure_is_bounded_and_does_not_write_script(self):
        errors = [urllib.error.URLError('offline') for _ in range(3)]
        self.assertEqual(self.run_bootstrap(errors, 'after 3 attempts'), (3, 2))

    def test_authorization_denial_is_not_retried(self):
        for status in (401, 403, 404, 302):
            with self.subTest(status=status):
                error = urllib.error.HTTPError('https://example.invalid', status, 'denied', {}, None)
                self.assertEqual(self.run_bootstrap([error], f'HTTP {status}'), (1, 0))


class OfflineFallbackTests(unittest.TestCase):
    def test_fallback_requires_run_specific_zip_and_copies_to_attempt_directory(self):
        text = (REPO / '.github/workflows/staging-e2e.yml').read_text()
        block = text.split('- name: Use separately transferred bundle if download failed', 1)[1]
        block = block.split("          python3 - <<'PY'\n", 1)[1].split('          PY\n', 1)[0]
        code = '\n'.join(line[10:] for line in block.splitlines())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workspace = root / 'workspace'
            workspace.mkdir()
            bundles = root / 'bundles'
            bundles.mkdir()
            real_path = type(root)

            def isolated_path(value):
                return bundles if value == '/srv/fastllm-staging/bundles' else real_path(value)

            with patch.dict(os.environ, {'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '2',
                                         'GITHUB_WORKSPACE': str(workspace)}), \
                    patch('pathlib.Path', side_effect=isolated_path):
                with self.assertRaisesRegex(SystemExit, '123.zip'):
                    exec(code, {})
                (bundles / '123.zip').write_bytes(b'fixture')
                exec(code, {})
            self.assertEqual((workspace / '.staging-123-2/bundle.zip').read_bytes(), b'fixture')


if __name__ == '__main__':
    unittest.main()
