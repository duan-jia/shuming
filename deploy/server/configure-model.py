#!/usr/bin/env python3
"""Configure the shared remote model without placing credentials in arguments."""

import argparse
import getpass
import json
import os
from pathlib import Path
import tempfile
from urllib.parse import urlsplit


def write_private(path: Path, value: str) -> None:
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f'.{path.name}.')
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write(value)
        if path.exists():
            stat = path.stat()
            os.chown(temporary, stat.st_uid, stat.st_gid)
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--web-env', type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    current = config.get('remote_engine', {})
    endpoint = input(f"API base URL [{current.get('endpoint', 'https://api.deepseek.com/v1')}]: ").strip()
    endpoint = endpoint or current.get('endpoint', 'https://api.deepseek.com/v1')
    model = input(f"Model [{current.get('model', 'deepseek-chat')}]: ").strip()
    model = model or current.get('model', 'deepseek-chat')
    key = getpass.getpass('API key (hidden): ').strip()
    parsed = urlsplit(endpoint)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
        raise SystemExit('Use an HTTPS API base URL without embedded credentials.')
    if not key or any(char in value for value in (endpoint, model, key) for char in '\r\n\x00'):
        raise SystemExit('A key is required; values must be single-line strings.')
    current.update(endpoint=endpoint, model=model, api_key=key)
    config['remote_engine'] = current
    write_private(args.config, json.dumps(config, ensure_ascii=False, indent=2) + '\n')
    write_private(args.web_env, '\n'.join([
        f'REMOTE_ENGINE_ENDPOINT={endpoint}',
        f'REMOTE_ENGINE_MODEL={model}',
        f'REMOTE_ENGINE_API_KEY={key}',
        '',
    ]))
    print('Model configuration saved. Recreate backend and web to apply it.')


if __name__ == '__main__':
    main()
