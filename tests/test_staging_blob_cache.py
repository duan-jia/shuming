"""Cross-run blob reuse with synthetic registry responses, no Docker/network."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_staging_ranged_image import BASIC, IMAGE, Server, ranged, sha, verify


class BlobCacheTests(unittest.TestCase):
    def transfer(self, root, name, server, cache):
        work = root / name
        work.mkdir(mode=0o700)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            ranged.transfer(IMAGE[:-40] + name[0] * 40, work, BASIC,
                            opener=server, cache_directory=cache)
        verify.verify(work / 'image.tar', server.manifest)
        self.assertFalse((work / 'blobs').exists())
        self.assertNotIn('FAKE_SECRET', output.getvalue())
        return output.getvalue()

    def test_changed_revision_downloads_only_changed_config_and_reauthenticates(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / 'cache'
            self.transfer(root, 'a', Server(), cache)
            next_server = Server()
            original = next_server.manifest['config']['digest']
            config = json.loads(next_server.blobs.pop(original))
            config['config'] = {'Labels': {'version': 'new'}}
            raw = json.dumps(config).encode()
            next_server.blobs[sha(raw)] = raw
            next_server.manifest['config'].update(digest=sha(raw), size=len(raw))
            next_server.raw = json.dumps(next_server.manifest).encode()
            output = self.transfer(root, 'b', next_server, cache)
            self.assertIn('Range cache hit:', output)
            self.assertEqual({digest for digest, _, _ in next_server.ranges}, {sha(raw)})
            self.assertTrue(any('/token?' in url for url, _, _ in next_server.requests))
            self.assertEqual(cache.stat().st_mode & 0o777, 0o700)
            self.assertTrue(all(p.stat().st_mode & 0o777 == 0o600 for p in cache.iterdir()))

    def test_cli_uses_cache_env_and_keeps_auth_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            auth = root / 'auth'
            auth.mkdir(mode=0o700)
            config = auth / 'config.json'
            config.write_text(json.dumps({'auths': {'ghcr.io': {'auth': BASIC}}}))
            config.chmod(0o600)
            for index in (1, 2):
                work = root / str(index)
                work.mkdir(mode=0o700)
                server = Server()
                with patch.dict(os.environ, {'DOCKER_CONFIG': str(auth),
                                             'STAGING_BLOB_CACHE': str(root / 'cache')}), \
                     patch('sys.argv', ['ranged-image.py', IMAGE, str(work)]), \
                     patch.object(ranged.urllib.request, 'build_opener', return_value=server), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(ranged.main(), 0)
                verify.verify(work / 'image.tar', server.manifest)
                if index == 2:
                    self.assertFalse(server.ranges)
            self.assertEqual(set(auth.iterdir()), {config})
            for entry in (root / 'cache').iterdir():
                self.assertNotIn(b'FAKE_SECRET', entry.read_bytes())

    def test_low_space_on_cache_filesystem_skips_retention(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'blob'
            source.write_bytes(b'hello')
            with ranged.BlobCache(root / 'cache', ranged.Budget()) as cache, \
                 patch.object(ranged.shutil, 'disk_usage', return_value=type('Disk', (), {'free': 1})()), \
                 contextlib.redirect_stdout(io.StringIO()):
                cache.put({'digest': sha(b'hello'), 'size': 5}, source)
            self.assertEqual({p.name for p in (root / 'cache').iterdir()}, {'.lock'})

    def test_corrupt_cache_is_redownloaded_not_imported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, server = Path(tmp), Server()
            cache = root / 'cache'
            self.transfer(root, 'a', server, cache)
            item = server.manifest['layers'][0]
            path = cache / item['digest'][7:]
            body = path.read_bytes()
            path.write_bytes(bytes([body[0] ^ 1]) + body[1:])
            server = Server()
            output = self.transfer(root, 'b', server, cache)
            self.assertIn('cache discarded:', output)
            self.assertEqual({digest for digest, _, _ in server.ranges}, {item['digest']})

    def test_cached_content_never_bypasses_current_registry_authorization(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / 'cache'
            self.transfer(root, 'a', Server(), cache)
            with self.assertRaisesRegex(ranged.Failure, 'auth_policy'):
                self.transfer(root, 'b', Server('realm'), cache)

    def test_bad_download_never_enters_cache_and_next_run_recovers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / 'cache'
            with self.assertRaisesRegex(ranged.Failure, 'blob_digest'):
                self.transfer(root, 'a', Server('corrupt'), cache)
            self.assertEqual({p.name for p in cache.iterdir()}, {'.lock'})
            self.transfer(root, 'b', Server(), cache)

    def test_size_cap_lru_oversize_and_interrupted_publication_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / 'cache'
            budget = ranged.Budget()
            with ranged.BlobCache(directory, budget, limit=8) as cache:
                for index, body in enumerate((b'aaaa', b'bbbb', b'cccc')):
                    source = root / str(index)
                    source.write_bytes(body)
                    cache.put({'digest': sha(body), 'size': len(body)}, source)
                    os.utime(directory / sha(body)[7:], ns=(index, index))
                self.assertFalse((directory / sha(b'aaaa')[7:]).exists())
                self.assertTrue((directory / sha(b'bbbb')[7:]).exists())
                large = root / 'large'
                large.write_bytes(b'x' * 9)
                cache.put({'digest': sha(b'x' * 9), 'size': 9}, large)
                self.assertFalse((directory / sha(b'x' * 9)[7:]).exists())
            orphan = directory / '.partial-killed'
            orphan.write_bytes(b'x')
            orphan.chmod(0o600)
            with ranged.BlobCache(directory, budget, limit=8):
                self.assertFalse(orphan.exists())
                self.assertLessEqual(sum(p.stat().st_size for p in directory.iterdir()), 8)

    def test_symlinks_permissions_hardlinks_and_concurrent_writer_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            directory = root / 'cache'
            with ranged.BlobCache(directory, ranged.Budget()):
                with self.assertRaisesRegex(ranged.Failure, 'cache_busy'):
                    with ranged.BlobCache(directory, ranged.Budget()):
                        pass
            directory.chmod(0o755)
            with self.assertRaises(ranged.Failure):
                with ranged.BlobCache(directory, ranged.Budget()):
                    pass
            directory.chmod(0o700)
            alias = root / 'alias'
            alias.symlink_to(directory, target_is_directory=True)
            with self.assertRaisesRegex(ranged.Failure, 'cache_policy'):
                with ranged.BlobCache(alias, ranged.Budget()):
                    pass
            external = root / 'keep'
            external.write_bytes(b'unchanged')
            external.chmod(0o600)
            entry = directory / ('a' * 64)
            for link in ('symlink', 'hardlink'):
                if link == 'symlink':
                    entry.symlink_to(external)
                else:
                    os.link(external, entry)
                with self.assertRaises(ranged.Failure):
                    with ranged.BlobCache(directory, ranged.Budget()):
                        pass
                self.assertEqual(external.read_bytes(), b'unchanged')
                entry.unlink()


if __name__ == '__main__':
    unittest.main()
