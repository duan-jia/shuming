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


if __name__ == '__main__':
    unittest.main()
