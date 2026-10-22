#!/usr/bin/env python3
"""Generate a private mihomo config from a Clash subscription URL."""
import json
import os
import shutil
from pathlib import Path
import sys
from urllib.parse import parse_qs, urlsplit


def subscription_url(value):
    value = value.strip()
    parsed = urlsplit(value)
    if parsed.scheme == 'clash' and parsed.netloc == 'install-config':
        value = parse_qs(parsed.query).get('url', [''])[0]
        parsed = urlsplit(value)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or '\n' in value or '\r' in value:
        raise ValueError('Use an HTTPS Clash/Mihomo subscription or clash://install-config link.')
    return value


def configure(root):
    root = Path(root)
    local_nodes = root / 'subscription.yaml'
    target = root / 'runtime'
    target.mkdir(mode=0o700, exist_ok=True)
    if local_nodes.is_file() and local_nodes.stat().st_size:
        # Local files bypass subscription endpoints that reject server IPs.
        # Mihomo validates the YAML; no YAML dependency is needed on the host.
        pending = target / 'subscription.pending'
        fd = os.open(pending, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'wb') as stream, local_nodes.open('rb') as source:
            shutil.copyfileobj(source, stream)
        os.replace(pending, target / 'subscription.yaml')
        provider = {'type': 'file', 'path': './subscription.yaml'}
    else:
        value = (root / 'subscription.url').read_text()
        if not value.strip():
            raise ValueError('Add subscription.yaml or fill subscription.url before starting.')
        provider = {'type': 'http', 'url': subscription_url(value),
                    'path': './providers/subscription.yaml', 'interval': 3600}
    provider['health-check'] = {'enable': True, 'url': 'https://www.gstatic.com/generate_204', 'interval': 600}
    config = {
        'mixed-port': 7890, 'allow-lan': True, 'bind-address': '*',
        'mode': 'rule', 'log-level': 'warning', 'ipv6': False,
        'external-controller': '127.0.0.1:9090',
        'proxy-providers': {'subscription': provider},
        'proxy-groups': [{'name': 'DOWNLOAD', 'type': 'url-test',
                          'use': ['subscription'], 'url': 'https://www.gstatic.com/generate_204',
                          'interval': 300, 'tolerance': 50}],
        'rules': ['MATCH,DOWNLOAD'],
    }
    temp = target / 'config.pending'
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(config, stream)
    os.replace(temp, target / 'config.json')


if __name__ == '__main__':
    try:
        configure(Path(__file__).resolve().parent)
    except (OSError, ValueError):
        print('Configuration failed: provide subscription.yaml or a valid HTTPS Clash/Mihomo URL in subscription.url.', file=sys.stderr)
        sys.exit(1)
    print('Private mihomo configuration generated.')
