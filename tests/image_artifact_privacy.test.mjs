// Conservative regression checks for this repository's workflow layout.
// Not a package ACL audit, image secret scan, or security sandbox.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import test from 'node:test';
const root = fileURLToPath(new URL('../', import.meta.url));
const workflow = readFileSync(path.join(root, '.github/workflows/staging-e2e.yml'), 'utf8');

test('Actions source bundle never includes exported image layers', () => {
  const packaging = workflow.split('      - name: Package source-only staging bundle')[1]?.split('      - name: Upload staging bundle')[0];
  assert.ok(packaging);
  assert.ok(packaging.includes('git archive --format=tar HEAD'));
  assert.ok(packaging.includes('sha256sum source.tar.gz REVISION > SHA256SUMS'));
  assert.doesNotMatch(workflow, /docker\s+(?:save|load)|images\.tar(?:\.gz)?/);
  assert.match(workflow, /names = \{'SHA256SUMS', 'REVISION', 'source\.tar\.gz'\}/);
  assert.ok(workflow.includes('if set(archive.namelist()) != names:'));
});

test('private-image fetch uses scoped ephemeral authentication and cleanup', () => {
  const [header, build, deploy] = workflow.split(/\n  (?:build-images|deploy-and-test):\n/);
  assert.doesNotMatch(header, /packages: write/);
  assert.match(build, /packages: write/);
  assert.match(deploy, /packages: read/);
  assert.doesNotMatch(deploy, /packages: write/);
  assert.match(deploy, /DOCKER_CONFIG: \$\{\{ runner.temp \}\}\/shuming-ghcr-/);
  assert.match(deploy, /--password-stdin/);
  assert.match(deploy, /trap cleanup_registry_auth EXIT/);
  assert.match(deploy, /unset GHCR_TOKEN/);
  assert.match(deploy, /name: Remove temporary registry credentials\n        if: always\(\)/);
  assert.doesNotMatch(deploy, /cat .*config\.json|set -x|docker login.*--password /);
});

test('prefetch is gated by the existing isolation guard and deploy remains offline', () => {
  const fetch = workflow.split('      - name: Fetch private images with temporary registry credentials')[1]?.split('      - name: Deploy stack and run E2E')[0];
  assert.ok(fetch);
  assert.ok(fetch.indexOf('python3 deploy/staging/guard.py') < fetch.indexOf('docker login'));
  for (const image of ['$BACKEND_IMAGE', '$MCP_IMAGE', 'mirror.ccs.tencentyun.com/library/mysql:8.4']) {
    assert.ok(fetch.includes(`pull_image ${image.startsWith('$') ? '"' + image + '"' : image}`));
  }
  assert.ok(fetch.includes('export STAGING_OFFLINE=0'));
  assert.match(workflow, /STAGING_OFFLINE: '1'/);
});

test('PRs from forks still cannot start the private staging jobs', () => {
  const gate = "github.event.pull_request.head.repo.full_name == github.repository";
  assert.equal(workflow.split(gate).length - 1, 2);
  assert.doesNotMatch(workflow, /pull_request_target/);
});
