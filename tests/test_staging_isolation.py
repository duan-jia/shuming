import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stage_guard', REPO / 'deploy/staging/guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class StagingIsolationTests(unittest.TestCase):
    def test_compose_cannot_overlap_production_resources(self):
        original = json.loads((REPO / 'deploy/staging/compose.json').read_text())
        guard.validate_compose(original)
        for change in ('project', 'port', 'network', 'volume', 'mount'):
            config = json.loads(json.dumps(original))
            if change == 'project':
                config['name'] = 'fast-llm-reasoner-staging'
            elif change == 'port':
                config['services']['backend']['ports'] = ['127.0.0.1:9006:9006']
            elif change == 'network':
                config['networks']['lab-knowledge'] = {'external': True}
            elif change == 'volume':
                config['volumes']['mysql-data'] = {'external': True, 'name': 'fast-llm-reasoner-staging_mysql-data'}
            else:
                config['services']['backend']['volumes'][1] = '/opt/fast-llm-reasoner/staging/rag-data:/data/rag'
            with self.subTest(change=change), self.assertRaises(ValueError):
                guard.validate_compose(config)

    def test_production_root_is_refused_before_docker_runs(self):
        result = subprocess.run(
            ['bash', str(REPO / 'deploy/staging/deploy.sh')],
            env={**os.environ, 'STAGING_ROOT': '/opt/fast-llm-reasoner/staging'},
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('dedicated CI staging directory', result.stderr)

    def test_compose_overrides_cannot_select_production(self):
        for key in ('COMPOSE_PROJECT_NAME', 'COMPOSE_FILE', 'COMPOSE_PROFILES'):
            with self.assertRaisesRegex(ValueError, 'must be unset'):
                guard.validate({key: 'production'})

    def test_environment_cannot_override_root_or_execute_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for value in ('/opt/fast-llm-reasoner/staging', '$(touch /tmp/unsafe)'):
                (root / '.env').write_text(f'STAGING_ROOT={value}\n')
                with self.assertRaisesRegex(ValueError, 'invalid STAGING_ROOT'):
                    guard.validate({'STAGING_ROOT': str(root)}, root)

    def test_symlink_data_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'rag-data').symlink_to('/opt/fast-llm-reasoner/staging/rag-data')
            with self.assertRaisesRegex(ValueError, 'must not be a symlink'):
                guard.validate({'STAGING_ROOT': str(root)}, root)

    def test_generated_environment_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            values = dict(STAGING_ROOT=str(root), HOST_UID='1000', HOST_GID='1000')
            for key in guard.KEYS.keys() - values.keys():
                values[key] = 'a' * (64 if 'TOKEN' in key or key == 'JWT_SECRET' else 48)
            (root / '.env').write_text(''.join(f'{key}={value}\n' for key, value in values.items()))
            self.assertEqual(guard.validate({'STAGING_ROOT': str(root)}, root), root)

    def test_service_password_supports_current_and_legacy_lengths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            values = dict(STAGING_ROOT=str(root), HOST_UID='1000', HOST_GID='1000')
            for key in guard.KEYS.keys() - values.keys():
                values[key] = 'a' * (64 if 'TOKEN' in key or key == 'JWT_SECRET' else 48)
            for length in (16, 48, 17):
                values['SERVICE_ACCOUNT_PASSWORD'] = 'b' * length
                (root / '.env').write_text(''.join(f'{key}={value}\n' for key, value in values.items()))
                if length == 17:
                    with self.assertRaisesRegex(ValueError, 'invalid SERVICE_ACCOUNT_PASSWORD'):
                        guard.validate({'STAGING_ROOT': str(root)}, root)
                else:
                    self.assertEqual(guard.validate({'STAGING_ROOT': str(root)}, root), root)


class ImageTransferTests(unittest.TestCase):
    def run_pull(self, *, cached=False, offline=False, succeeds=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = root / 'trace'
            scripts = {
                'docker': '#!/bin/bash\n[[ "$1" == image ]] && exit ' + ('0' if cached else '1') + '\n[[ "$1" == create || "$1" == rm ]] && exit 0\nexit 1\n',
                'timeout': '#!/bin/bash\nprintf "%s\\n" "$*" >> "$TRACE"\nexit ' + ('0' if succeeds else '124') + '\n',
                'sleep': '#!/bin/bash\nexit 0\n',
            }
            for name, script in scripts.items():
                path = root / name
                path.write_text(script)
                path.chmod(0o755)
            result = subprocess.run(
                ['bash', '-c', 'source "$1"; pull_image fixture:immutable', '_',
                 str(REPO / 'deploy/staging/pull-images.sh')],
                env={**os.environ, 'PATH': str(root) + ':' + os.environ['PATH'],
                     'TRACE': str(trace), 'STAGING_OFFLINE': '1' if offline else '0'},
                capture_output=True, text=True,
            )
            return result, trace.read_text().splitlines() if trace.exists() else []

    def test_cached_images_do_not_contact_registry(self):
        result, calls = self.run_pull(cached=True, offline=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_offline_missing_image_fails_without_network(self):
        result, calls = self.run_pull(offline=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing image', result.stderr)
        self.assertEqual(calls, [])

    def test_stalled_pulls_are_bounded_and_retried_three_times(self):
        result, calls = self.run_pull()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(c.startswith('--kill-after=10s 120s docker pull ') for c in calls))

    def test_successful_pull_stops_retrying(self):
        result, calls = self.run_pull(succeeds=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(calls), 1)


if __name__ == '__main__':
    unittest.main()
