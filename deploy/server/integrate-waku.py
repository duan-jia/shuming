#!/usr/bin/env python3
"""Prepare Waku's MCP configuration and Compose network overrides on the host."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--waku-root', type=Path, default=Path('/srv/waku-simple'))
    parser.add_argument('--server-root', type=Path, default=Path('/srv/lab-knowledge'))
    parser.add_argument('--backend-env', type=Path, default=Path('/opt/fast-llm-reasoner/staging/.env'))
    args = parser.parse_args()
    values = dict(line.split('=', 1) for line in args.backend_env.read_text().splitlines()
                  if '=' in line and not line.startswith('#'))
    token = values['MCP_BEARER_TOKEN']
    if not token or any(char in token for char in '\r\n\x00'):
        raise SystemExit('Invalid MCP token in deployment configuration.')
    config_path = args.waku_root / 'tenants/admin/home/mcp.json'
    config = json.loads(config_path.read_text())
    servers = config.setdefault('servers', [])
    entry = {'name': 'lab_knowledge', 'url': 'http://mcp:3100/mcp',
             'auth_env': 'LAB_KNOWLEDGE_MCP_TOKEN'}
    existing = next((server for server in servers if server.get('name') == entry['name']), None)
    if existing is not None and existing != entry:
        raise SystemExit('lab_knowledge already has a different configuration; inspect it first.')
    backups = args.server_root / 'backups' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    backups.mkdir(parents=True, mode=0o700)
    for path in (config_path, args.waku_root / 'lab.compose.json', args.waku_root / 'lab-mcp.env'):
        if path.exists():
            shutil.copy2(path, backups / path.name)
            os.chmod(backups / path.name, 0o600)
    if existing is None:
        servers.append(entry)
        config_path.write_text(json.dumps(config, indent=2) + '\n')
    token_path = args.waku_root / 'lab-mcp.env'
    token_path.write_text(f'LAB_KNOWLEDGE_MCP_TOKEN={token}\n')
    os.chmod(token_path, 0o600)
    waku_override = {
        'services': {
            'caddy': {'networks': {'lab-web': None}},
            'waku-admin': {'env_file': [str(token_path)], 'networks': {'lab-knowledge': None}},
        },
        'networks': {
            'lab-web': {'external': True, 'name': 'lab-web'},
            'lab-knowledge': {'external': True, 'name': 'lab-knowledge'},
        },
    }
    backend_override = {
        'services': {'mcp': {'networks': {'lab-knowledge': None}}},
        'networks': {'lab-knowledge': {'name': 'lab-knowledge'}},
    }
    (args.waku_root / 'lab.compose.json').write_text(json.dumps(waku_override, indent=2) + '\n')
    (args.server_root / 'backend.integration.json').write_text(json.dumps(backend_override, indent=2) + '\n')
    print('MCP configuration prepared; existing configuration backed up. No containers restarted.')


if __name__ == '__main__':
    main()
