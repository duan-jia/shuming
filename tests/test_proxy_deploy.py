import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

SPEC = importlib.util.spec_from_file_location('proxy_configure', Path(__file__).resolve().parents[1] / 'deploy/proxy/configure.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ProxyConfigTests(unittest.TestCase):
    def test_windows_import_and_private_configuration(self):
        url = 'https://example.com/sub?token=private&format=clash'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'subscription.url').write_text('clash://install-config?url=' + quote(url, safe=''))
            MODULE.configure(root)
            target = root / 'runtime/config.json'
            config = json.loads(target.read_text())
            self.assertEqual(config['proxy-providers']['subscription']['url'], url)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            self.assertEqual(config['rules'], ['MATCH,DOWNLOAD'])

    def test_local_yaml_bypasses_blocked_subscription_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nodes = 'proxies:\n  - name: test\n    type: ss\n'
            (root / 'subscription.yaml').write_text(nodes)
            (root / 'subscription.url').write_text('invalid or blocked URL')
            MODULE.configure(root)
            config = json.loads((root / 'runtime/config.json').read_text())
            provider = config['proxy-providers']['subscription']
            self.assertEqual(provider['type'], 'file')
            self.assertNotIn('url', provider)
            copied = root / 'runtime/subscription.yaml'
            self.assertEqual(copied.read_text(), nodes)
            self.assertEqual(copied.stat().st_mode & 0o777, 0o600)
            self.assertEqual(config['external-controller'], '127.0.0.1:9090')

    def test_empty_or_invalid_input_does_not_overwrite_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'runtime').mkdir()
            target = root / 'runtime/config.json'
            target.write_text('existing')
            for value in ('', 'vmess://secret', 'http://example.com', 'https://example.com/\nsecret'):
                (root / 'subscription.url').write_text(value)
                with self.assertRaises(ValueError):
                    MODULE.configure(root)
                self.assertEqual(target.read_text(), 'existing')


if __name__ == '__main__':
    unittest.main()
