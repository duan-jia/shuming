import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('release', Path(__file__).resolve().parents[1] / 'deploy/server/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ProductionReleaseTests(unittest.TestCase):
    def test_revision_must_be_immutable(self):
        for value in ('latest', 'master', 'a' * 39, 'a' * 40 + '; echo unsafe'):
            with self.assertRaises(ValueError):
                release.images(value)
        self.assertEqual(len(release.images('a' * 40)), 3)

    def test_release_preserves_mcp_network_and_frontend_volumes(self):
        integration = {'services': {'mcp': {'networks': ['staging', 'lab-knowledge'], 'image': 'old'}},
                       'networks': {'lab-knowledge': {'external': True}}}
        frontend = {'services': {'web': {'image': 'old', 'volumes': ['pdfs:/data/pdfs'],
                                        'env_file': ['model.env']}}}
        selected = release.images('a' * 40)
        updated, web = release.updated_files(integration, frontend, selected)
        self.assertEqual(updated['services']['mcp']['networks'], ['staging', 'lab-knowledge'])
        self.assertEqual(updated['networks'], integration['networks'])
        self.assertEqual(web['services']['web']['env_file'], ['model.env'])
        self.assertEqual(web['services']['web']['volumes'], ['pdfs:/data/pdfs'])
        self.assertEqual(updated['services']['backend']['image'], selected['backend'])
        self.assertEqual(integration['services']['mcp']['image'], 'old')
        self.assertEqual(frontend['services']['web']['image'], 'old')


if __name__ == '__main__':
    unittest.main()
