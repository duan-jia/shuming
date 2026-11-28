"""Release selection must match validated master images; no GitHub calls."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_checks', ROOT / 'deploy/server/verify-release-checks.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
SHA = 'a' * 40


def run(**updates):
    return dict({'id': 10, 'head_sha': SHA, 'head_branch': 'master', 'event': 'push',
                 'status': 'completed', 'conclusion': 'success'}, **updates)


class ReleaseChecksTests(unittest.TestCase):
    def test_exact_master_revision_success_and_manual_staging_allowed(self):
        self.assertEqual(gate.require_success({'workflow_runs': [run()]}, SHA, {'push'}), 10)
        self.assertEqual(gate.require_success({'workflow_runs': [run(event='workflow_dispatch')]},
                                              SHA, {'push', 'workflow_dispatch'}), 10)

    def test_wrong_revision_branch_pr_pending_failure_and_empty_blocked(self):
        for updates in ({'head_sha': 'b' * 40}, {'head_branch': 'feature'}, {'event': 'pull_request'},
                        {'status': 'in_progress', 'conclusion': None}, {'conclusion': 'failure'},
                        {'conclusion': 'cancelled'}, {'conclusion': 'skipped'}):
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                gate.require_success({'workflow_runs': [run(**updates)]}, SHA, {'push', 'workflow_dispatch'})
        with self.assertRaises(ValueError):
            gate.require_success({'workflow_runs': []}, SHA, {'push'})
        with self.assertRaises(ValueError):
            gate.require_success({'workflow_runs': [run(), run(id=11, conclusion='failure')]}, SHA, {'push'})

    def test_lookup_checks_both_workflows_with_full_sha_and_long_token(self):
        requests = []
        class Opener:
            def open(self, request, timeout):
                requests.append(request)
                return io.BytesIO(json.dumps({'workflow_runs': [run()]}).encode())
        with contextlib.redirect_stdout(io.StringIO()) as output:
            gate.verify(SHA, 'ghs_' + 'X' * 520, opener=Opener())
        self.assertEqual(len(requests), 2)
        self.assertIn('/ci.yml/runs?', requests[0].full_url)
        self.assertIn('/staging-e2e.yml/runs?', requests[1].full_url)
        for request in requests:
            self.assertIn('head_sha=' + SHA, request.full_url)
            self.assertIn('branch=master', request.full_url)
            self.assertEqual(request.get_header('Authorization'), 'Bearer ghs_' + 'X' * 520)
        self.assertNotIn('ghs_', output.getvalue())

    def test_network_error_and_invalid_revision_fail_closed(self):
        class Broken:
            def open(self, *args, **kwargs):
                raise OSError('fixture failure')
        with self.assertRaises(OSError):
            gate.verify(SHA, 'fake', opener=Broken())
        with self.assertRaises(ValueError):
            gate.verify('latest', 'fake', opener=Broken())

    def test_production_gate_precedes_login_and_release(self):
        source = (ROOT / '.github/workflows/production-release.yml').read_text()
        self.assertIn('  actions: read', source)
        gate_position = source.index('run: python3 deploy/server/verify-release-checks.py')
        self.assertLess(gate_position, source.index('name: Log in to GHCR'))
        self.assertLess(gate_position, source.index('run: sudo'))


if __name__ == '__main__':
    unittest.main()
