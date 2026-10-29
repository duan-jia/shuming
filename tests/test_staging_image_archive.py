import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verify_image', ROOT / 'deploy/staging/verify-image.py')
verify_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify_image)


def fixture(path, *, empty_corruption=False, config_corruption=False, wrong_diff=False):
    layer = b'\0' * 1024
    compressed = gzip.compress(layer, mtime=0)
    config = json.dumps({'rootfs': {'diff_ids': ['sha256:' + hashlib.sha256(layer).hexdigest()]}}).encode()
    manifest = {'config': {'digest': 'sha256:' + hashlib.sha256(config).hexdigest()}, 'layers': [{
        'digest': 'sha256:' + hashlib.sha256(compressed).hexdigest(),
        'size': len(compressed), 'mediaType': 'application/vnd.oci.image.layer.v1.tar+gzip'}]}
    if empty_corruption or wrong_diff:
        compressed = b''
    if wrong_diff:
        # A loadable legacy descriptor can still disagree with rootfs.diff_ids.
        manifest['layers'][0].update(digest='sha256:' + hashlib.sha256(b'').hexdigest(),
                                     size=0, mediaType='application/vnd.oci.image.layer.v1.tar')
    if config_corruption:
        config += b' '
    with tarfile.open(path, 'w') as archive:
        for name, data in [('config.json', config), ('layer.tar.gz', compressed),
                           ('manifest.json', json.dumps([{'Config': 'config.json', 'Layers': ['layer.tar.gz']}]).encode())]:
            item = tarfile.TarInfo(name)
            item.size = len(data)
            archive.addfile(item, io.BytesIO(data))
    return manifest


class ImageArchiveTests(unittest.TestCase):
    def test_intact_compressed_empty_layer_matches_registry_and_filesystem(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'image.tar'
            verify_image.verify(path, fixture(path))

    def test_zero_byte_layer_is_rejected_before_import(self):
        for options in ({'empty_corruption': True}, {'wrong_diff': True}, {'config_corruption': True}):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'image.tar'
                manifest = fixture(path, **options)
                with self.assertRaisesRegex(ValueError, 'mismatch'):
                    verify_image.verify(path, manifest)

    def test_archive_links_are_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'link.tar'
            with tarfile.open(path, 'w') as archive:
                item = tarfile.TarInfo('config.json')
                item.type = tarfile.SYMTYPE
                item.linkname = '/etc/passwd'
                archive.addfile(item)
            with tarfile.open(path) as archive:
                with self.assertRaisesRegex(ValueError, 'non-file'):
                    verify_image.member(archive, 'config.json')


if __name__ == '__main__':
    unittest.main()
