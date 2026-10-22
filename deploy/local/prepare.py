#!/usr/bin/env python3
"""Prepare isolated local credentials and CPU runtime without touching production data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / '.tmp/local-deploy'
MODEL = ROOT / 'models/bge-small-zh-v1.5-q4_k_m.gguf'
MODEL_SHA = '0c17cc6ed7ec697db6768c2db6dd22c4e816a12c68ed14ff4d764927338532f8'
MODEL_URL = 'https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf/resolve/5bf683e6a1bd454bbb60ba051088c50731d63fcb/bge-small-zh-v1.5-q4_k_m.gguf?download=true'


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builder-image', default='shuming-local-builder')
    parser.add_argument('--config-only', action='store_true', help='Only initialize isolated credentials and data directories')
    parser.add_argument('--skip-build', action='store_true', help='Use previously prepared .tmp/local-runtime artifacts')
    args = parser.parse_args()
    os.umask(0o077)
    LOCAL.mkdir(parents=True, exist_ok=True)
    for name in ('rag', 'pdfs', 'processed'):
        (LOCAL / 'data' / name).mkdir(parents=True, exist_ok=True)
    env_path = Path(__file__).with_name('.env')
    config_path = LOCAL / 'config.json'
    if not env_path.exists() and not config_path.exists():
        root_password, mysql_password, admin_password = (secrets.token_urlsafe(24) for _ in range(3))
        config = json.loads((ROOT / 'config.example.json').read_text())
        config['mysql'].update(host='mysql', user='shuming', password=mysql_password)
        config['server']['port'] = 9006
        config['jwt']['secret'] = secrets.token_urlsafe(48)
        config['admin']['default_password'] = admin_password
        config['inference']['n_gpu_layers'] = 0
        config['rag']['storage_dir'] = '/app/data/rag'
        config['rag']['query_rewrite']['enabled'] = False
        config_path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + '\n')
        env_path.write_text(f'MYSQL_ROOT_PASSWORD={root_password}\nMYSQL_PASSWORD={mysql_password}\nHOST_UID={os.getuid()}\nHOST_GID={os.getgid()}\nWEB_PORT=3001\nBACKEND_PORT=9006\n# Shared DeepSeek key for chat, search and PDF processing.\nDEEPSEEK_API_KEY=\n')
        print('Created local credentials. Admin password is in .tmp/local-deploy/config.json.')
    elif not env_path.exists() or not config_path.exists():
        raise SystemExit('Local .env and config.json must both exist. Restore the missing file before continuing.')
    if args.config_only:
        return
    MODEL.parent.mkdir(exist_ok=True)
    if not MODEL.exists():
        partial = MODEL.with_suffix('.download')
        print('Downloading pinned BGE embedding model...', flush=True)
        urllib.request.urlretrieve(MODEL_URL, partial)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != MODEL_SHA:
            raise SystemExit('Embedding model checksum mismatch')
        partial.replace(MODEL)
    if hashlib.sha256(MODEL.read_bytes()).hexdigest() != MODEL_SHA:
        raise SystemExit('Embedding model checksum mismatch')
    if not args.skip_build:
        image_exists = subprocess.run(['docker', 'image', 'inspect', args.builder_image], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not image_exists:
            run('docker', 'build', '--build-arg', 'HTTP_PROXY', '--build-arg', 'HTTPS_PROXY', '-t', args.builder_image, '-f', 'deploy/local/build.Dockerfile', '.')
        build_script = r'''
set -eu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF
cmake --build build --target fast-llm-reasoner fast-llm-cli --parallel 2
mkdir -p .tmp/local-runtime/bin .tmp/local-runtime/lib .tmp/local-runtime/certs
cp build/bin/fast-llm-reasoner .tmp/local-runtime/bin/
ldd build/bin/fast-llm-reasoner | awk '{ for (i=1; i<=NF; i++) if ($i ~ /^\//) { print $i; break } }' | sort -u | xargs -r -I '{}' cp -L '{}' .tmp/local-runtime/lib/
cp /etc/ssl/certs/ca-certificates.crt .tmp/local-runtime/certs/
chown -R "$LOCAL_UID:$LOCAL_GID" .tmp/local-runtime
'''
        run('docker', 'run', '--rm', '-e', 'HTTP_PROXY', '-e', 'HTTPS_PROXY', '-v', f'{ROOT}:/src', '-w', '/src', '-e', f'LOCAL_UID={os.getuid()}', '-e', f'LOCAL_GID={os.getgid()}', args.builder_image, 'bash', '-c', build_script)
    print('Ready: docker compose -f deploy/local/compose.yml up -d --build')


if __name__ == '__main__':
    main()
