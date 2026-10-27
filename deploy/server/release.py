"""Manually release immutable images using the live integration overrides."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path('/srv/lab-knowledge')
ENV = Path('/opt/fast-llm-reasoner/staging/.env')
SERVICES = {
    'backend': 'fast-llm-reasoner-staging-backend-1',
    'mcp': 'fast-llm-reasoner-staging-mcp-1',
    'web': 'fast-llm-reasoner-web-web-1',
}


def images(revision):
    if not re.fullmatch(r'[a-f0-9]{40}', revision):
        raise ValueError('Image revision must be a full lowercase commit SHA')
    return {service: f'ghcr.io/duan-jia/shuming-{service}:{revision}'
            for service in SERVICES}


def override(selected):
    return {'services': {service: {'image': image} for service, image in selected.items()}}


def updated_files(integration, frontend, selected):
    # Clone JSON structures so callers keep an untouched rollback snapshot.
    integration = json.loads(json.dumps(integration))
    frontend = json.loads(json.dumps(frontend))
    for service in ('backend', 'mcp'):
        integration.setdefault('services', {}).setdefault(service, {})['image'] = selected[service]
    frontend['services']['web']['image'] = selected['web']
    return integration, frontend


def run(*args, capture=False):
    return subprocess.run(['docker', *args], check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def save(path, value):
    fd, temporary = tempfile.mkstemp(prefix='.release-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2)
            stream.write('\n')
        if path.exists():
            stat = path.stat()
            os.chown(temporary, stat.st_uid, stat.st_gid)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def release(revision):
    selected = images(revision)
    integration_path = ROOT / 'backend.integration.json'
    frontend_path = ROOT / 'src/deploy/server/compose.json'
    integration = json.loads(integration_path.read_text())
    frontend = json.loads(frontend_path.read_text())
    old = {service: run('inspect', '--format', '{{.Image}}', container, capture=True).strip()
           for service, container in SERVICES.items()}
    os.environ.update(REPO_ROOT=str(ROOT / 'src'), SERVER_ROOT=str(ROOT))
    for image in selected.values():
        run('pull', image)
    backups = ROOT / 'backups'
    backups.mkdir(mode=0o700, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    backup = backups / f'release-{stamp}.json'
    save(backup, {'revision': revision, 'previous_images': old,
                  'backend_integration': integration, 'frontend_compose': frontend})
    candidate = backups / f'candidate-{stamp}.json'
    save(candidate, override(selected))

    # Each project override names only its own services.
    def start_separately(selected_images):
        save(candidate, override({s: selected_images[s] for s in ('backend', 'mcp')}))
        run('compose', '--env-file', str(ENV), '-f', str(ROOT / 'backend.compose.yml'),
            '-f', str(integration_path), '-f', str(candidate),
            'up', '-d', '--no-build', '--no-deps', '--wait', '--wait-timeout', '240',
            'backend', 'mcp')
        save(candidate, override({'web': selected_images['web']}))
        run('compose', '-f', str(frontend_path), '-f', str(candidate),
            'up', '-d', '--no-build', '--no-deps', '--wait', '--wait-timeout', '120', 'web')

    try:
        start_separately(selected)
        new_integration, new_frontend = updated_files(integration, frontend, selected)
        save(integration_path, new_integration)
        save(frontend_path, new_frontend)
    except Exception:
        save(integration_path, integration)
        save(frontend_path, frontend)
        start_separately(old)
        raise
    finally:
        candidate.unlink(missing_ok=True)
    print(f'Release completed. Rollback metadata: {backup}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    try:
        selected = images(args.revision)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    if args.apply:
        release(args.revision)
    else:
        print(json.dumps(override(selected), indent=2))
