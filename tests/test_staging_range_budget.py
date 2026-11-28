"""Only new budget calibration contracts; no network/Docker or repeated full suite."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ranges_budget', ROOT / 'deploy/staging/ranged-image.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RangeBudgetTests(unittest.TestCase):
    def test_measured_default_remains_a_hard_deadline(self):
        now = [100]
        budget = module.Budget(clock=lambda: now[0])
        self.assertEqual(budget.end, 640)
        now[0] = 639
        self.assertEqual(budget.check(), 1)
        now[0] = 640
        with self.assertRaises(module.Failure):
            budget.check()

    def test_entire_fetch_fits_existing_thirty_minute_ceiling(self):
        workflow = (ROOT / '.github/workflows/staging-e2e.yml').read_text()
        fetch = workflow.split('- name: Fetch private images', 1)[1].split('- name: Deploy stack', 1)[0]
        self.assertIn('timeout-minutes: 30', fetch)
        self.assertEqual(fetch.count('fetch_with_budget 720s'), 2)
        self.assertIn('fetch_with_budget 120s', fetch)
        self.assertIn('20s docker login', fetch)
        self.assertEqual(2 * 730 + 130 + 30 + 25 + 10 + 15 + 25 + 30, 1725)
        self.assertLess(1725, 30 * 60)
        self.assertIn('20s rm -rf -- "$DOCKER_CONFIG"', fetch)
        self.assertIn('20s python3 deploy/staging/guard.py', fetch)
        script = (ROOT / 'deploy/staging/pull-images.sh').read_text()
        section = script.split('    range)', 1)[1].split('    crane)', 1)[0]
        for expected in ('ranges 570s', 'verify 45s', 'load 45s', 'inspect 10s'):
            self.assertIn(expected, section)
        self.assertIn('20s docker create', script)
        self.assertIn('5s docker rm', script)


if __name__ == '__main__':
    unittest.main()

class FetchCleanupTests(unittest.TestCase):
    def exercise(self, fault):
        import os
        import shutil
        import subprocess
        import tempfile
        workflow = (ROOT / '.github/workflows/staging-e2e.yml').read_text()
        step = workflow.split('- name: Fetch private images', 1)[1].split('- name: Deploy stack', 1)[0]
        body = step.split('        run: |\n', 1)[1]
        body = '\n'.join(line[10:] if line.startswith('          ') else line for line in body.splitlines())
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'deploy/staging').mkdir(parents=True)
            (root / 'deploy/staging/pull-images.sh').write_text('''pull_image() {
  echo image >> "$TRACE"
  [[ "$FAULT" != image_error ]] || return 23
}
''')
            bins = root / 'bin'
            bins.mkdir()
            scripts = {
                'python3': '#!/bin/bash\nexit 0\n',
                'docker': '''#!/bin/bash
echo "docker-$1" >> "$TRACE"
if [[ "$1" == login ]]; then
  cat >/dev/null
  printf 'fixture-only' > "$DOCKER_CONFIG/config.json"
  [[ "$FAULT" != login_error ]] || exit 21
fi
exit 0
''',
                'timeout': '''#!/bin/bash
shift 2
if [[ "$1" == bash && "$FAULT" == image_timeout ]]; then
  "$REAL_TIMEOUT" --kill-after=0.05s 0.05s /bin/sleep 1
  exit $?
fi
if [[ "$1" == docker && "$2" == logout && "$FAULT" == logout_timeout ]]; then echo timeout-logout >> "$TRACE"; exit 124; fi
if [[ "$1" == rm && "$FAULT" == removal_timeout ]]; then exit 124; fi
exec "$@"
'''}
            for name, value in scripts.items():
                path = bins / name
                path.write_text(value)
                path.chmod(0o700)
            config = root / 'auth'
            trace = root / 'trace'
            env = {**os.environ, 'PATH': str(bins) + ':' + os.environ['PATH'],
                   'TRACE': str(trace), 'FAULT': fault, 'DOCKER_CONFIG': str(config),
                   'GHCR_TOKEN': 'FAKE_TOKEN_NOT_LOGGABLE', 'GITHUB_ACTOR': 'fixture',
                   'BACKEND_IMAGE': 'fixture-backend', 'MCP_IMAGE': 'fixture-mcp',
                   'REAL_TIMEOUT': shutil.which('timeout')}
            result = subprocess.run(['/bin/bash', '-c', body], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=3)
            self.assertNotIn('FAKE_TOKEN_NOT_LOGGABLE', result.stdout + result.stderr)
            return result.returncode, trace.read_text().splitlines(), config.exists()

    def test_success_cleans_original_job_auth(self):
        rc, calls, remains = self.exercise('ok')
        self.assertEqual(rc, 0)
        self.assertEqual(calls.count('image'), 3)
        self.assertIn('docker-logout', calls)
        self.assertFalse(remains)

    def test_login_error_image_error_and_real_timeout_all_run_cleanup(self):
        for fault, expected in [('login_error', 21), ('image_error', 23), ('image_timeout', 124)]:
            with self.subTest(fault=fault):
                rc, calls, remains = self.exercise(fault)
                self.assertEqual(rc, expected)
                self.assertIn('docker-logout', calls)
                self.assertFalse(remains)

    def test_logout_timeout_still_removes_auth(self):
        rc, calls, remains = self.exercise('logout_timeout')
        self.assertEqual(rc, 0)
        self.assertIn('timeout-logout', calls)
        self.assertFalse(remains)

    def test_removal_timeout_is_failure_not_false_clean(self):
        rc, calls, remains = self.exercise('removal_timeout')
        self.assertEqual(rc, 124)
        self.assertTrue(remains)
        self.assertIn('docker-logout', calls)

    def test_probe_timeout_still_attempts_own_container_removal(self):
        import os
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            timeout = root / 'timeout'
            timeout.write_text('''#!/bin/bash
shift 2
if [[ "$1" == docker && "$2" == create ]]; then exit 124; fi
exec "$@"
''')
            docker = root / 'docker'
            docker.write_text('#!/bin/bash\necho "$1" >> "$TRACE"\n')
            for path in (timeout, docker):
                path.chmod(0o700)
            env = {**os.environ, 'PATH': str(root) + ':' + os.environ['PATH'], 'TRACE': str(root / 'trace')}
            result = subprocess.run(['/bin/bash', '-c', 'set -Eeuo pipefail; source "$1"; probe_image fixture',
                                     '_', str(ROOT / 'deploy/staging/pull-images.sh')],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 124)
            self.assertIn('rm', (root / 'trace').read_text().splitlines())
