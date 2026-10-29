"""Verify a crane tarball against its registry manifest before Docker imports it."""
import argparse
import gzip
import hashlib
import json
import re
import tarfile


def digest(stream):
    checksum = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        checksum.update(block)
    return 'sha256:' + checksum.hexdigest()


def member(archive, name):
    if name.startswith('/') or '..' in name.split('/'):
        raise ValueError('Unsafe image archive reference')
    item = archive.getmember(name)
    if not item.isfile():
        raise ValueError('Image archive references a non-file member')
    return item


def small_json(archive, name, limit=4 * 1024 * 1024):
    item = member(archive, name)
    if item.size > limit:
        raise ValueError('Oversized image metadata')
    with archive.extractfile(item) as stream:
        raw = stream.read()
    return raw, json.loads(raw)


def verify(path, registry_manifest):
    with tarfile.open(path) as archive:
        _, entries = small_json(archive, 'manifest.json')
        if len(entries) != 1:
            raise ValueError('Expected exactly one platform image')
        entry = entries[0]
        raw, config = small_json(archive, entry['Config'])
        if 'sha256:' + hashlib.sha256(raw).hexdigest() != registry_manifest['config']['digest']:
            raise ValueError('Registry configuration digest mismatch')
        layers = entry['Layers']
        expected = config['rootfs']['diff_ids']
        descriptors = registry_manifest['layers']
        if not len(layers) == len(expected) == len(descriptors):
            raise ValueError('Image layer count mismatch')
        for index, (name, diff_id, descriptor) in enumerate(zip(layers, expected, descriptors)):
            item = member(archive, name)
            with archive.extractfile(item) as stream:
                if digest(stream) != descriptor['digest'] or item.size != descriptor['size']:
                    raise ValueError(f'Registry layer {index} digest/size mismatch')
            media_type = descriptor['mediaType']
            with archive.extractfile(item) as stream:
                if media_type.endswith('gzip'):
                    with gzip.GzipFile(fileobj=stream) as uncompressed:
                        actual = digest(uncompressed)
                elif media_type.endswith('.tar'):
                    actual = digest(stream)
                else:
                    raise ValueError('Unsupported image layer compression')
            if not re.fullmatch(r'sha256:[0-9a-f]{64}', diff_id) or actual != diff_id:
                raise ValueError(f'Image layer {index} filesystem digest mismatch')
    print(f'Image archive verified: {len(layers)} layers and registry configuration')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive')
    parser.add_argument('manifest')
    args = parser.parse_args()
    try:
        with open(args.manifest) as stream:
            verify(args.archive, json.load(stream))
    except (ValueError, KeyError, IndexError, TypeError, OSError, EOFError, tarfile.TarError) as error:
        raise SystemExit(f'Image archive verification failed: {error}') from None
