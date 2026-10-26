// Conservative lint for this repository's workflow layout, not a security sandbox.
// YAML syntax is separately validated by GitHub; runner removal is an operator gate.
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import test from 'node:test';

const root = fileURLToPath(new URL('../', import.meta.url));
const read = (file) => readFileSync(path.join(root, file), 'utf8');
const directory = '.github/workflows';
const files = readdirSync(path.join(root, directory)).filter((f) => /\.ya?ml$/.test(f));
const gate = [
  "github.event_name == 'workflow_dispatch' &&",
  "github.ref == 'refs/heads/master' &&",
  "vars.ENABLE_PRIVATE_DEPLOYMENT == 'true'",
].join('\n      ');

function checkServerWorkflow(source) {
  assert.match(source, /^on:\n  workflow_dispatch:/m);
  assert.doesNotMatch(source, /^  (pull_request|pull_request_target|push|workflow_run|schedule):/m);
  const blocks = source.split(/^jobs:\s*\n/m)[1]?.split(/(?=^  [\w-]+:\s*$)/m).filter((s) => s.trim());
  assert.ok(blocks?.length, 'expected jobs in standard layout');
  for (const block of blocks) {
    // Gate every job, including hosted image pushes, not just server jobs.
    assert.ok(block.includes(`    if: >-\n      ${gate}\n`), 'manual/master/opt-in gate required');
  }
}

test('all server workflows are manual, master-only and explicitly opt-in', () => {
  for (const file of files) {
    const source = read(`${directory}/${file}`);
    if (/self-hosted/.test(source)) checkServerWorkflow(source);
  }
  for (const file of ['staging-e2e.yml', 'staging-smoke.yml', 'production-release.yml']) {
    checkServerWorkflow(read(`${directory}/${file}`));
  }
});

test('policy rejects accidental trigger and gate regressions', () => {
  const source = read(`${directory}/staging-e2e.yml`);
  for (const mutated of [
    source.replace('  workflow_dispatch:', '  pull_request:\n  workflow_dispatch:'),
    source.replace("github.ref == 'refs/heads/master'", 'true'),
    source.replace("vars.ENABLE_PRIVATE_DEPLOYMENT == 'true'", 'true'),
    source.replace("github.event_name == 'workflow_dispatch'", 'true'),
  ]) assert.throws(() => checkServerWorkflow(mutated));
});

test('PR CI stays hosted, read-only and without deployment secrets', () => {
  const source = read(`${directory}/ci.yml`);
  assert.match(source, /^  pull_request:/m);
  assert.doesNotMatch(source, /self-hosted|pull_request_target|secrets\.|packages:\s*write|contents:\s*write/);
  assert.match(source, /^permissions:\n  contents: read\n/m);
  const runners = source.match(/^    runs-on: .+$/gm);
  assert.ok(runners?.length >= 4);
  for (const runner of runners) assert.equal(runner.trim(), 'runs-on: ubuntu-24.04');
  assert.ok(source.includes('node --test tests/public_repository_policy.test.mjs'));
});

test('GHCR write permission belongs only to the gated staging build job', () => {
  const source = read(`${directory}/staging-e2e.yml`);
  const [header, build, deploy] = source.split(/\n  (?:build-images|deploy-and-test):\n/);
  assert.doesNotMatch(header, /packages: write/);
  assert.match(build, /permissions:\n      contents: read\n      packages: write/);
  assert.doesNotMatch(deploy, /packages: write/);
});

test('unconfirmed third-party PDF is replaced by a publication link', () => {
  assert.equal(existsSync(path.join(root, 'web/public/papers/FlowDyn-Dynamic-Flowlet-Gap-Detection.pdf')), false);
  assert.ok(read('web/public/papers/README.md').includes('https://arxiv.org/abs/1910.03324'));
  assert.ok(read('docs/open-source-readiness.md').includes('历史'));
});

test('contribution, security and release gates remain discoverable', () => {
  for (const file of ['CONTRIBUTING.md', 'SECURITY.md']) {
    assert.ok(read('README.md').includes(`](${file})`));
    assert.ok(read(file).includes('docs/open-source-readiness.md') || read(file).includes('SECURITY.md'));
  }
  assert.ok(read('docs/open-source-readiness.md').includes('LICENSE'));
});
