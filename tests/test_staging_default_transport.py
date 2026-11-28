"""No real proxy, registry, token or Docker is used by these regression tests."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
PROXY_KEYS = ('HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy', 'NO_PROXY', 'no_proxy')


class DefaultTransportTests(unittest.TestCase):
    def run_transfer_until_tool_preparation(self, inherited):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            private = root / 'private'
            private.mkdir(mode=0o700)
            mockbin = root / 'bin'
            mockbin.mkdir()
            # Pretend a bad loopback proxy is listening: old code would select it.
            for name, text in {'timeout': '#!/bin/bash\necho listener >> "$TRACE"\nexit 0\n',
                               'curl': '#!/bin/bash\necho unexpected-network >> "$TRACE"\nexit 99\n'}.items():
                path = mockbin / name
                path.write_text(text)
                path.chmod(0o700)
            env = {k: v for k, v in os.environ.items() if k not in PROXY_KEYS}
            env.update(inherited)
            env.update(PATH=str(mockbin) + ':' + os.environ['PATH'],
                       DOCKER_CONFIG=str(private), TRACE=str(root / 'trace'))
            code = '''set -Eeuo pipefail
source "$1"
prepare_crane() {
  for key in HTTP_PROXY HTTPS_PROXY http_proxy https_proxy NO_PROXY no_proxy; do
    printf '%s=%s\n' "$key" "${!key-<unset>}"
  done
  return 17
}
pull_private_image ghcr.io/example/fixture:immutable
'''
            result = subprocess.run(['bash', '-c', code, '_', str(REPO / 'deploy/staging/pull-images.sh')],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 17, result.stderr)
            self.assertFalse((root / 'trace').exists(), 'must not probe/select loopback or contact network')
            self.assertEqual(list(private.iterdir()), [], 'private workspace cleanup retained')
            return dict(line.split('=', 1) for line in result.stdout.splitlines())

    def test_default_remains_default_even_if_listener_would_be_present(self):
        values = self.run_transfer_until_tool_preparation({})
        self.assertEqual(values, dict.fromkeys(PROXY_KEYS, '<unset>'))

    def test_explicit_inherited_environment_is_not_overwritten(self):
        values = {key: ('localhost,fixture.internal' if 'no_proxy' == key.lower()
                        else 'http://existing-fixture.invalid:8080') for key in PROXY_KEYS}
        self.assertEqual(self.run_transfer_until_tool_preparation(values), values)

    def test_precheckout_bootstrap_does_not_force_loopback_proxy(self):
        text = (REPO / '.github/workflows/staging-e2e.yml').read_text()
        step = text.split('- name: Download staging bundle', 1)[1].split('- name: Use separately transferred', 1)[0]
        self.assertNotIn('7890', step)
        self.assertNotIn('export http', step)
        self.assertNotIn('export HTTP', step)
        self.assertNotIn('export no_proxy', step)
        self.assertIn('timeout=30', step)
        self.assertIn('for attempt in range(3)', step)
        self.assertIn('NoRedirect', step)


if __name__ == '__main__':
    unittest.main()
