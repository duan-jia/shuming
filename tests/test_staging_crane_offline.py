"""Local shell tests; only the successful fixture's SHA verifier is mocked."""
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'deploy/staging/pull-images.sh'
PIN = '0ab7a1d6932a213aed964ce97666c3077fe691c8606413674a8b3e0b9ec4cda0'


class OfflineCraneTests(unittest.TestCase):
    def run_case(self, case, *, mock_digest=False, private_pull=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root / 'offline.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                data = b'#!/bin/bash\necho fixture-only\n'
                info = tarfile.TarInfo('crane')
                info.mode, info.size = 0o755, len(data)
                tar.addfile(info, io.BytesIO(data))
            archive.chmod(0o600)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            source = archive
            if case == 'missing':
                source = root / 'missing.tar.gz'
            elif case == 'symlink':
                source = root / 'link.tar.gz'
                source.symlink_to(archive)
            elif case == 'permissions':
                archive.chmod(0o644)
            elif case == 'owner':
                pass  # stat wrapper supplies a foreign owner, without chown.
            bin_dir, work, config = root / 'bin', root / 'work', root / 'config'
            for p in (bin_dir, work, config):
                p.mkdir(mode=0o700)
            trace = root / 'trace'
            scripts = {
                'curl': '#!/bin/bash\necho curl >> "$TRACE"\nexit 97\n',
                'docker': '#!/bin/bash\necho docker >> "$TRACE"\nexit 98\n',
                'timeout': '#!/bin/bash\necho timeout >> "$TRACE"\nexit 99\n',
            }
            if case == 'owner':
                scripts['stat'] = '#!/bin/bash\necho 999999:600\n'
            if mock_digest:
                # Verify pinned expected value AND bytes of the copied fixture;
                # no real upstream digest is weakened in production source.
                scripts['sha256sum'] = '''#!/bin/bash
read -r expected file
[[ "$expected" == "$PIN" ]] || exit 1
[[ "$(/usr/bin/sha256sum "$file" | cut -d' ' -f1)" == "$FIXTURE_DIGEST" ]] || exit 1
'''
            for name, text in scripts.items():
                p = bin_dir / name
                p.write_text(text)
                p.chmod(0o700)
            env = {**os.environ, 'PATH': str(bin_dir) + ':' + os.environ['PATH'],
                   'TRACE': str(trace), 'PIN': PIN, 'FIXTURE_DIGEST': digest,
                   'WORK': str(work), 'DOCKER_CONFIG': str(config)}
            env.pop('STAGING_CRANE_ARCHIVE', None)
            if case != 'online':
                env['STAGING_CRANE_ARCHIVE'] = str(source)
            action = 'pull_private_image ghcr.io/example/fixture:immutable' if private_pull else 'prepare_crane "$WORK"'
            result = subprocess.run(['bash', '-c', 'set -Eeuo pipefail; source "$1"; ' + action,
                                     '_', str(SCRIPT)], env=env, capture_output=True, text=True)
            return (result, trace.read_text().splitlines() if trace.exists() else [],
                    (work / 'crane').is_file(), (work / 'tool.tar.gz').stat().st_mode & 0o777
                    if (work / 'tool.tar.gz').exists() else None, list(config.iterdir()))

    def test_missing_file_fails_without_network(self):
        r, calls, extracted, _, _ = self.run_case('missing')
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(calls, [])
        self.assertFalse(extracted)

    def test_bad_digest_uses_real_sha_and_never_extracts(self):
        r, calls, extracted, _, _ = self.run_case('bad')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('FAILED', r.stdout)
        self.assertEqual(calls, [])
        self.assertFalse(extracted)

    def test_matching_fixture_extracted_only_after_verifier_accepts(self):
        r, calls, extracted, mode, _ = self.run_case('valid', mock_digest=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(extracted)
        self.assertEqual(mode, 0o600)
        self.assertEqual(calls, [])

    def test_unsafe_path_owner_or_mode_refused(self):
        for case in ('symlink', 'permissions', 'owner'):
            with self.subTest(case=case):
                r, calls, extracted, _, _ = self.run_case(case)
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(calls, [])
                self.assertFalse(extracted)

    def test_unconfigured_preserves_online_path_but_network_is_mocked(self):
        r, calls, extracted, _, _ = self.run_case('online')
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(calls, ['curl'])
        self.assertFalse(extracted)

    def test_private_workspace_cleaned_on_prepare_failure(self):
        r, calls, _, _, remaining = self.run_case('missing', private_pull=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('curl', calls)
        self.assertNotIn('docker', calls)
        self.assertEqual(remaining, [])


if __name__ == '__main__':
    unittest.main()
