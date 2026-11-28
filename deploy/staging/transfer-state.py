"""Allowlisted transfer telemetry and verified job-local compressed-layer reuse.

Never print raw tool diagnostics, registry headers, URLs, or credential values.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import shutil

DIGEST = re.compile(r'sha256:[0-9a-f]{64}')


def descriptors(work):
    manifest = json.loads((work / 'manifest.json').read_text())
    layers = manifest['layers']
    config = manifest['config']
    if not isinstance(layers, list) or not isinstance(config, dict):
        raise ValueError('Invalid image descriptors')
    for item in [config, *layers]:
        if not DIGEST.fullmatch(item['digest']) or type(item['size']) is not int or item['size'] < 0:
            raise ValueError('Invalid image descriptor digest/size')
    return layers


def clean_cache(work):
    layers = descriptors(work)
    expected = {item['digest']: item['size'] for item in layers}
    cache = work / 'cache'
    cache.mkdir(mode=0o700, exist_ok=True)
    kept = removed = size = 0
    for path in cache.iterdir():
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            raise ValueError('Unexpected cache directory')
        valid = (stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                 and stat.S_IMODE(info.st_mode) == 0o600
                 and path.name in expected and info.st_size == expected[path.name])
        if valid:
            checksum = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    checksum.update(block)
            valid = path.name == 'sha256:' + checksum.hexdigest()
        if valid:
            kept += 1
            size += info.st_size
        else:
            path.unlink()
            removed += 1
    # Cache plus output tar need roughly two compressed copies. Keep at least
    # 20% of currently free space; Docker unpacking still needs a separate gate.
    config_size = json.loads((work / 'manifest.json').read_text())['config']['size']
    required = 2 * sum(x['size'] for x in layers) + 2 * config_size + 1024 * 1024
    available = shutil.disk_usage(work).free
    if required > available * 4 // 5:
        raise ValueError('Insufficient private transfer space')
    print(f'Transfer capacity: required_bytes={required} free_bytes={available}')
    print(f'Layer cache verified: complete={kept} bytes={size} discarded={removed} '
          f'expected_layers={len(layers)} expected_compressed_bytes={sum(x["size"] for x in layers)}')


def classify(rc, error):
    if rc == 0:
        return 'ok'
    if rc == 124:
        return 'deadline'
    if rc == 137:
        return 'killed'
    ranged = re.search(r'Range transfer failed category=(deadline|redirect_policy|credentials_policy|image_policy|auth_policy|auth|io|tls|metadata|manifest_http|manifest_digest|address_expired|http_transient|range_status|range_protocol|range_interrupted|disk|blob_digest|platform|capacity|cache_policy|cache_busy|internal)\b', error)
    if ranged:
        return ranged.group(1)
    # Only fixed category labels leave the private workspace, never matched text.
    classes = [
        ('auth', r'UNAUTHORIZED|DENIED|unauthorized|authentication required|\b(?:401|403)\b'),
        ('dns', r'no such host|Could not resolve|name resolution'),
        ('tls', r'TLS handshake|SSL_ERROR|certificate|tls:'),
        ('disk', r'no space left|disk quota|ENOSPC'),
        ('digest', r'digest.*mismatch|checksum.*mismatch|filesystem digest|layer.*size mismatch'),
        ('timeout', r'timed out|context deadline|timeout'),
        ('eof', r'unexpected EOF|unexpected end|connection reset'),
        ('http_transient', r'\b(?:408|429|500|502|503|504)\b'),
    ]
    for label, pattern in classes:
        if re.search(pattern, error, re.IGNORECASE):
            return label
    return 'other' if error else 'empty_stderr'


def report(work, stage, rc, seconds):
    path = work / 'transfer.err'
    error = ''
    if path.exists():
        with path.open('rb') as stream:
            stream.seek(max(0, path.stat().st_size - 65536))
            error = stream.read().decode('utf-8', errors='replace')
    archive = work / 'image.tar'
    size = archive.stat().st_size if archive.is_file() else 0
    cache_bytes = sum(p.lstat().st_size for p in (work / 'cache').glob('*')
                      if stat.S_ISREG(p.lstat().st_mode))
    print(f'Transfer stage={stage} rc={rc} seconds={seconds} archive_bytes={size} cache_bytes={cache_bytes} '
          f'category={classify(rc, error)}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['cache', 'report'])
    parser.add_argument('work', type=Path)
    parser.add_argument('--stage', choices=['manifest', 'pull', 'ranges', 'verify', 'load', 'inspect', 'unpack'])
    parser.add_argument('--rc', type=int, default=0)
    parser.add_argument('--seconds', type=int, default=0)
    args = parser.parse_args()
    try:
        if args.operation == 'cache':
            clean_cache(args.work)
        else:
            report(args.work, args.stage, args.rc, args.seconds)
    except (ValueError, KeyError, TypeError, OSError):
        raise SystemExit('Transfer metadata/cache validation failed (details withheld).') from None
