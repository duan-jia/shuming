"""Fault injection only: no real registry, Docker, token, or sleeping."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_staging_image_archive import fixture

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('transfer_state', ROOT / 'deploy/staging/transfer-state.py')
state = importlib.util.module_from_spec(spec)
spec.loader.exec_module(state)


class TransferStateTests(unittest.TestCase):
    def manifest(self, root, payload):
        digest = 'sha256:' + hashlib.sha256(payload).hexdigest()
        (root / 'manifest.json').write_text(json.dumps({
            'config': {'digest': 'sha256:' + '0' * 64, 'size': 3},
            'layers': [{'digest': digest, 'size': len(payload)}]}))
        return digest

    def test_only_manifest_matching_complete_private_compressed_layers_survive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / 'cache'
            cache.mkdir()
            digest = self.manifest(root, b'valid')
            good = cache / digest
            good.write_bytes(b'valid')
            good.chmod(0o600)
            bad = cache / ('sha256:' + '1' * 64)
            bad.write_bytes(b'partial')
            link = cache / 'link'
            external = root / 'external'
            external.write_text('do-not-touch')
            link.symlink_to(external)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                state.clean_cache(root)
            self.assertEqual(list(cache.iterdir()), [good])
            self.assertEqual(external.read_text(), 'do-not-touch')
            self.assertIn('complete=1 bytes=5 discarded=2', out.getvalue())

    def test_wrong_hash_same_size_and_permissive_mode_are_discarded(self):
        for payload, mode in [(b'wrong', 0o600), (b'valid', 0o644)]:
            with self.subTest(payload=payload, mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                digest = self.manifest(root, b'valid')
                (root / 'cache').mkdir()
                path = root / 'cache' / digest
                path.write_bytes(payload)
                path.chmod(mode)
                with contextlib.redirect_stdout(io.StringIO()):
                    state.clean_cache(root)
                self.assertFalse(path.exists())

    def test_insufficient_cache_and_archive_space_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.manifest(root, b'valid')
            with patch.object(state.shutil, 'disk_usage') as usage:
                usage.return_value.free = 1024
                with self.assertRaisesRegex(ValueError, 'space'):
                    state.clean_cache(root)

    def test_error_categories_never_return_secret_or_url(self):
        samples = [(124, '', 'deadline'), (137, '', 'killed'), (1, 'GET https://private.invalid/?sig=SECRET 401 Bearer SECRET', 'auth'),
                   (1, 'tls: handshake failure', 'tls'), (1, 'no such host', 'dns'),
                   (1, 'unexpected EOF', 'eof'), (1, 'no space left on device', 'disk'),
                   (1, 'Registry layer 0 digest/size mismatch', 'digest'), (1, 'HTTP 503', 'http_transient'),
                   (1, '', 'empty_stderr')]
        for rc, error, expected in samples:
            self.assertEqual(state.classify(rc, error), expected)

    def run_fault(self, fault):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = root / 'private'
            config.mkdir(mode=0o700)
            bins = root / 'bin'
            bins.mkdir()
            archive = root / 'fixture.tar'
            manifest = fixture(archive, empty_corruption=fault == 'verify')
            manifest['config']['size'] = 100
            (root / 'fixture.json').write_text(json.dumps(manifest))
            # This is a process-level fake of native crane, not a real network.
            crane = root / 'crane'
            crane.write_text('''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys, tarfile
root=pathlib.Path(os.environ['FIXTURE_ROOT'])
args=sys.argv[1:]
with (root/'trace').open('a') as out: out.write(args[0]+'\\n')
if args[0]=='manifest':
    if os.environ['FAULT']=='auth':
        print('401 Authorization: Bearer SECRET https://private.invalid/?sig=SECRET',file=sys.stderr);sys.exit(1)
    print((root/'fixture.json').read_text());sys.exit(0)
cache=pathlib.Path(args[args.index('--cache_path')+1])
count=root/'count'
n=int(count.read_text())+1 if count.exists() else 1
count.write_text(str(n))
manifest=json.loads((root/'fixture.json').read_text())
digest=manifest['layers'][0]['digest']
if os.environ['FAULT'] in ('resume','timeout') and n==1:
    with tarfile.open(root/'fixture.tar') as tar: data=tar.extractfile('layer.tar.gz').read()
    (cache/digest).write_bytes(data);(cache/digest).chmod(0o600)
    (cache/'unknown-partial').write_bytes(b'partial')
    pathlib.Path(args[-1]).write_bytes(b'incomplete')
    sys.exit(124)
if os.environ['FAULT']=='resume':
    assert (cache/digest).is_file() and not (cache/'unknown-partial').exists()
if os.environ['FAULT']=='timeout': sys.exit(124)
shutil.copyfile(root/'fixture.tar',args[-1])
''')
            crane.chmod(0o700)
            scripts = {
                'timeout': '#!/bin/bash\nshift 2\nexec "$@"\n',
                'sleep': '#!/bin/bash\nexit 0\n',
                'docker': '''#!/bin/bash
echo "docker-$1" >> "$FIXTURE_ROOT/trace"
if [[ "$FAULT" == load && "$1" == load ]]; then echo 'no space left SECRET' >&2; exit 1; fi
if [[ "$FAULT" == unpack && "$1" == create ]]; then echo 'private SECRET' >&2; exit 1; fi
exit 0
'''}
            for name, data in scripts.items():
                p = bins / name
                p.write_text(data)
                p.chmod(0o700)
            env = {**os.environ, 'DOCKER_CONFIG': str(config), 'FIXTURE_ROOT': str(root),
                   'FAULT': fault, 'PATH': str(bins) + ':' + os.environ['PATH']}
            code = '''set -Eeuo pipefail
source "$1"
prepare_crane() { cp "$FIXTURE_ROOT/crane" "$1/crane"; }
pull_private_image ghcr.io/example/fixture:immutable
'''
            result = subprocess.run(['bash', '-c', code, '_', str(ROOT / 'deploy/staging/pull-images.sh')],
                                    env=env, capture_output=True, text=True)
            self.assertNotIn('SECRET', result.stdout + result.stderr)
            self.assertNotIn('private.invalid', result.stdout + result.stderr)
            self.assertEqual(list(config.iterdir()), [], 'job-private auth workspace descendants must be cleaned')
            trace = (root / 'trace').read_text().splitlines()
            return result, trace

    def test_timed_out_pull_reuses_only_verified_complete_layers_then_imports(self):
        result, trace = self.run_fault('resume')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(trace.count('pull'), 2)
        self.assertIn('stage=pull rc=124', result.stderr)
        self.assertIn('complete=1', result.stdout)
        for stage in ('manifest', 'pull', 'verify', 'load', 'inspect', 'unpack'):
            self.assertIn('stage=' + stage, result.stderr)
        self.assertIn('docker-create', trace)

    def test_two_timeouts_are_bounded_and_never_import(self):
        result, trace = self.run_fault('timeout')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(trace.count('pull'), 2)
        self.assertNotIn('docker-load', trace)
        self.assertIn('category=deadline', result.stderr)

    def test_auth_failure_never_pulls_or_imports_and_is_redacted(self):
        result, trace = self.run_fault('auth')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('pull', trace)
        self.assertNotIn('docker-load', trace)
        self.assertIn('category=auth', result.stderr)

    def test_verify_load_unpack_failures_are_identified_and_not_retried(self):
        for fault in ('verify', 'load', 'unpack'):
            with self.subTest(fault=fault):
                result, trace = self.run_fault(fault)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(trace.count('pull'), 1)
                self.assertIn('stage=' + fault + ' rc=1', result.stderr)
                if fault == 'verify':
                    self.assertNotIn('docker-load', trace)
                    self.assertIn('category=digest', result.stderr)


if __name__ == '__main__':
    unittest.main()
