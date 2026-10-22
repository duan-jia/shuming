"""Refuse deployment outside the dedicated CI staging stack."""
import json
import os
from pathlib import Path
import re

ROOT = Path('/srv/fastllm-staging')
PROJECT = 'fast-llm-reasoner-ci-staging'
KEYS = {
    'STAGING_ROOT': r'/srv/fastllm-staging',
    'HOST_UID': r'[0-9]+', 'HOST_GID': r'[0-9]+',
    'MYSQL_ROOT_PASSWORD': r'[a-f0-9]{48}',
    'JWT_SECRET': r'[a-f0-9]{64}', 'ADMIN_PASSWORD': r'[a-f0-9]{48}',
    'SERVICE_ACCOUNT_PASSWORD': r'[a-f0-9]{48}',
    'MCP_BEARER_TOKEN': r'[a-f0-9]{64}',
    'MCP_BEARER_TOKEN_SHA256': r'[a-f0-9]{64}',
}


def validate_compose(config):
    if config.get('name') != PROJECT:
        raise ValueError('Compose must select the dedicated CI staging project')
    if set(config.get('services', {})) != {'mysql', 'backend', 'mcp'}:
        raise ValueError('Compose must contain only staging services')
    if config.get('networks') != {'staging': {'driver': 'bridge'}}:
        raise ValueError('Compose must contain only its private staging network')
    expected_ports = {'mysql': [], 'backend': ['127.0.0.1:19006:9006'],
                      'mcp': ['127.0.0.1:13100:3100']}
    expected_volumes = {
        'mysql': ['mysql-data:/var/lib/mysql', '${REPO_ROOT}/sql/init.sql:/docker-entrypoint-initdb.d/001-init.sql:ro'],
        'backend': ['${STAGING_ROOT}/config.json:/run/fastllm/config.json:ro', '${STAGING_ROOT}/rag-data:/data/rag'],
        'mcp': [],
    }
    if config.get('volumes') != {'mysql-data': None}:
        raise ValueError('Compose must use a project-owned database volume')
    for service, ports in expected_ports.items():
        spec = config['services'][service]
        if spec.get('ports', []) != ports or spec.get('networks') != ['staging']:
            raise ValueError('Compose ports and networks must belong to CI staging')
        if spec.get('volumes', []) != expected_volumes[service]:
            raise ValueError('Compose mounts must belong to CI staging')


def validate(environ, root=ROOT):
    selected = Path(environ.get('STAGING_ROOT', str(ROOT)))
    if selected != root or selected.resolve() != root:
        raise ValueError('STAGING_ROOT must be the dedicated CI staging directory')
    for key in ('COMPOSE_PROJECT_NAME', 'COMPOSE_FILE', 'COMPOSE_PROFILES'):
        if environ.get(key):
            raise ValueError(f'{key} must be unset for staging')
    for name in ('.env', 'config.json', 'rag-data', 'models'):
        if (root / name).is_symlink():
            raise ValueError(f'Staging {name} must not be a symlink')
    env_file = root / '.env'
    if env_file.exists():
        values = {}
        for line in env_file.read_text().splitlines():
            key, sep, value = line.partition('=')
            if not sep or key not in KEYS or key in values:
                raise ValueError('Staging environment has an unsupported entry')
            pattern = re.escape(str(root)) if key == 'STAGING_ROOT' else KEYS[key]
            if not re.fullmatch(pattern, value):
                raise ValueError(f'Staging environment has an invalid {key}')
            values[key] = value
        if values.keys() != KEYS.keys():
            raise ValueError('Staging environment is incomplete')
    return selected


if __name__ == '__main__':
    try:
        validate(os.environ)
        validate_compose(json.loads(Path(__file__).with_name("compose.json").read_text()))
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
