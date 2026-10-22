import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'deploy/server' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ServerDeploymentTest(unittest.TestCase):
    def test_mcp_integration_preserves_existing_servers_and_is_repeatable(self):
        module = load('integrate', 'integrate-waku.py')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            waku = root / 'waku'
            server = root / 'server'
            server.mkdir()
            home = waku / 'tenants/admin/home'
            home.mkdir(parents=True)
            config = home / 'mcp.json'
            existing = {'name': 'existing', 'url': 'https://existing.example/mcp'}
            config.write_text(json.dumps({'servers': [existing]}))
            env = root / 'backend.env'
            env.write_text('MCP_BEARER_TOKEN=fixture-token\n')
            argv = ['integrate', '--waku-root', str(waku), '--server-root', str(server),
                    '--backend-env', str(env)]
            with patch('sys.argv', argv):
                module.main()
                module.main()
            entries = json.loads(config.read_text())['servers']
            self.assertEqual(entries[0], existing)
            self.assertEqual(len(entries), 2)
            self.assertNotIn('fixture-token', config.read_text())
            self.assertEqual((waku / 'lab-mcp.env').stat().st_mode & 0o777, 0o600)
            self.assertEqual(len(list((server / 'backups').iterdir())), 2)

    def test_model_configuration_updates_backend_and_pdf_processor(self):
        module = load('model', 'configure-model.py')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'config.json'
            web_env = root / 'web-model.env'
            config.write_text(json.dumps({'mysql': {'database': 'existing'},
                                          'remote_engine': {'endpoint': 'https://api.deepseek.com/v1',
                                                            'model': 'deepseek-chat'}}))
            with patch('sys.argv', ['model', '--config', str(config), '--web-env', str(web_env)]), \
                    patch('builtins.input', side_effect=['', '']), \
                    patch.object(module.getpass, 'getpass', return_value='fixture-key'):
                module.main()
            saved = json.loads(config.read_text())
            self.assertEqual(saved['mysql']['database'], 'existing')
            self.assertEqual(saved['remote_engine']['api_key'], 'fixture-key')
            self.assertIn('REMOTE_ENGINE_API_KEY=fixture-key', web_env.read_text())
            self.assertEqual(web_env.stat().st_mode & 0o777, 0o600)


if __name__ == '__main__':
    unittest.main()
