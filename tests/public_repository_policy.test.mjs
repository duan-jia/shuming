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
  "(github.event_name == 'pull_request' &&",
  " github.event.pull_request.base.ref == 'master' &&",
  " github.event.pull_request.head.repo.full_name == github.repository) ||",
  "(github.event_name == 'push' && github.ref == 'refs/heads/master') ||",
  "(github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/master')",
].join('\n      ');

function checkStaging(source) {
  assert.match(source, /^  pull_request:\n    branches: \[master\]/m);
  assert.match(source, /^  push:\n    branches: \[master\]/m);
  assert.match(source, /^  workflow_dispatch:/m);
  assert.doesNotMatch(source, /pull_request_target|ENABLE_PRIVATE_DEPLOYMENT/);
  const blocks = source.split(/^jobs:\s*\n/m)[1]?.split(/(?=^  [\w-]+:\s*$)/m).filter((b) => b.trim());
  assert.equal(blocks?.length, 2);
  for (const block of blocks) assert.ok(block.includes('    if: >-\n      ' + gate + '\n'));
  assert.match(source, /group: lab-server-release\n  cancel-in-progress: false/);
}

test('same-repository PR and final master staging are automatic and path-filtered', () => {
  const source = read(directory + '/staging-e2e.yml');
  checkStaging(source);
  assert.equal((source.match(/    paths:/g) || []).length, 2);
  assert.doesNotMatch(source, /      - (?:docs\/|\*\*\/\*\.md)/);
  assert.ok(source.includes('      - web/**'));
  assert.ok(source.includes('      - engine/**'));
});

test('gate rejects forks, wrong targets and non-master push/manual events', () => {
  const allow = Function('github', 'return (' + gate + ')');
  for (const event of ['pull_request', 'push', 'workflow_dispatch', 'pull_request_target', 'release']) {
    for (const ref of ['refs/heads/master', 'refs/heads/feature', 'refs/pull/24/merge']) {
      for (const base of ['master', 'other']) {
        for (const head of ['owner/project', 'stranger/fork']) {
          const github = { event_name: event, ref, repository: 'owner/project', event: {
            pull_request: { base: { ref: base }, head: { repo: { full_name: head } } },
          } };
          const expected = (event === 'pull_request' && base === 'master' && head === github.repository) ||
            (['push', 'workflow_dispatch'].includes(event) && ref === 'refs/heads/master');
          assert.equal(allow(github), expected);
        }
      }
    }
  }
  const source = read(directory + '/staging-e2e.yml');
  for (const needle of [
    "github.event.pull_request.head.repo.full_name == github.repository",
    "github.event.pull_request.base.ref == 'master'",
    "github.event_name == 'push'",
  ]) assert.throws(() => checkStaging(source.replace(needle, 'true')));
  // Push/dispatch contexts do not include pull_request: short-circuiting must be safe.
  for (const event of ['push', 'workflow_dispatch', 'release']) {
    assert.equal(allow({ event_name: event, ref: 'refs/heads/master', event: {} }), event !== 'release');
  }
});

test('production and smoke remain manual on master; Release never auto-deploys', () => {
  for (const file of ['production-release.yml', 'staging-smoke.yml']) {
    const source = read(directory + '/' + file);
    assert.match(source, /^on:\n  workflow_dispatch:/m);
    assert.doesNotMatch(source.split(/^permissions:/m)[0], /^  (pull_request|pull_request_target|push|release|workflow_run|schedule):/m);
    assert.ok(source.includes("github.event_name == 'workflow_dispatch' &&\n      github.ref == 'refs/heads/master'"));
    assert.doesNotMatch(source, /ENABLE_PRIVATE_DEPLOYMENT/);
    assert.match(source, /group: lab-server-release\n  cancel-in-progress: false/);
  }
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
