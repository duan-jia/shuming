// Conservative regression checks for this repository's workflow layout.
// Not a package ACL audit, image secret scan, or security sandbox.
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
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

test('GHCR downloads use pinned client-side transport without daemon changes', () => {
  const helper = readFileSync(path.join(root, 'deploy/staging/pull-images.sh'), 'utf8');
  assert.match(helper, /HTTPS_PROXY=http:\/\/127\.0\.0\.1:7890/);
  assert.match(helper, /v0\.22\.1\/go-containerregistry_Linux_x86_64\.tar\.gz/);
  assert.match(helper, /0ab7a1d6932a213aed964ce97666c3077fe691c8606413674a8b3e0b9ec4cda0/);
  assert.ok(helper.indexOf('sha256sum --check') < helper.indexOf('"$work/crane" pull'));
  assert.match(helper, /--platform linux\/amd64 --format tarball/);
  assert.doesNotMatch(helper, /--cache_path/);
  assert.match(helper, /http2client=0/);
  assert.match(helper, /verify-image\.py/);
  assert.match(helper, /trap 'rm -rf -- "\$work"' EXIT/);
  assert.match(helper, /docker load --input "\$work\/image\.tar"/);
  assert.doesNotMatch(helper + workflow, /systemctl\s+(?:restart|stop)|daemon\.json/);
});

// Exercise transfer routing, failure bounds and private-workspace cleanup using
// mock processes only: no network, real registry credentials or Docker daemon.
import { mkdtempSync, mkdirSync, writeFileSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
function mockTransfer({ fail = false, offline = false, cached = false, corrupt = false, brokenCache = false, badLoad = false } = {}) {
  const dir = mkdtempSync(path.join(tmpdir(), 'shuming-transfer-test-'));
  const bin = path.join(dir, 'bin'), auth = path.join(dir, 'auth');
  mkdirSync(bin); mkdirSync(auth);
  const fixture = spawnSync('python3', ['-c', `
import sys, pathlib, json, importlib.util
root=pathlib.Path(sys.argv[1]); target=pathlib.Path(sys.argv[2])
spec=importlib.util.spec_from_file_location('archive_fixture',root/'tests/test_staging_image_archive.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
(target/'registry.json').write_text(json.dumps(m.fixture(target/'fixture.tar')))
`, root, dir], { encoding: 'utf8' });
  assert.equal(fixture.status, 0, fixture.stderr);
  if (corrupt) writeFileSync(path.join(dir, 'fixture.tar'), 'incomplete archive');
  const scripts = {
    timeout: '[[ "$1" == 2 ]] && exit 1; shift 2; exec "$@"',
    curl: 'echo curl >> "$TEST_LOG"; while (($#)); do if [[ "$1" == --output ]]; then : > "$2"; break; fi; shift; done',
    sha256sum: 'cat >/dev/null; echo checksum >> "$TEST_LOG"',
    tar: `while (($#)); do if [[ "$1" == --directory ]]; then target="$2"; break; fi; shift; done
cat > "$target/crane" <<'CRANE'
#!/usr/bin/env bash
echo crane >> "$TEST_LOG"
echo "$GODEBUG" >> "$TEST_GODEBUG"
if [[ "$TEST_FAIL" == 1 ]]; then echo 'Get https://registry.example/blob?sig=private-secret' >&2; exit 1; fi
if [[ "$1" == manifest ]]; then cat "$TEST_MANIFEST"; else cp "$TEST_ARCHIVE" "\${!#}"; fi
CRANE
chmod +x "$target/crane"`,
    docker: `echo "docker $1" >> "$TEST_LOG"
case "$1" in
 image) [[ "$TEST_CACHED" == 1 || -f "$TEST_STATE" ]];;
 load) touch "$TEST_STATE";;
 create) [[ "$TEST_BAD_LOAD" != 1 ]] && { [[ "$TEST_BROKEN_CACHE" != 1 || -f "$TEST_STATE" ]]; };;
 rm) exit 0;;
esac`,
    sleep: ':'
  };
  for (const [name, script] of Object.entries(scripts)) {
    writeFileSync(path.join(bin, name), '#!/usr/bin/env bash\n' + script + '\n', { mode: 0o755 });
  }
  const log = path.join(dir, 'calls'); writeFileSync(log, '');
  try {
    const r = spawnSync('bash', ['-c', 'set -e; source "$1"; pull_image ghcr.io/example/test:sha', '_', path.join(root, 'deploy/staging/pull-images.sh')], {
      env: { ...process.env, PATH: bin + ':' + process.env.PATH, DOCKER_CONFIG: auth,
        STAGING_OFFLINE: offline ? '1' : '0', TEST_FAIL: fail ? '1' : '0',
        TEST_CACHED: cached ? '1' : '0', TEST_LOG: log, TEST_STATE: path.join(dir, 'loaded'),
        TEST_ARCHIVE: path.join(dir, 'fixture.tar'), TEST_MANIFEST: path.join(dir, 'registry.json'),
        TEST_BROKEN_CACHE: brokenCache ? '1' : '0', TEST_BAD_LOAD: badLoad ? '1' : '0',
        GODEBUG: 'other=1', TEST_GODEBUG: path.join(dir, 'godebug') }, encoding: 'utf8'
    });
    return { status: r.status, calls: readFileSync(log, 'utf8'), remaining: readdirSync(auth), stderr: r.stderr,
      godebug: existsSync(path.join(dir, 'godebug')) ? readFileSync(path.join(dir, 'godebug'), 'utf8') : '' };
  } finally { rmSync(dir, { recursive: true, force: true }); }
}
test('client-side transfer loads locally and removes temporary image files', () => {
  const r = mockTransfer();
  assert.equal(r.status, 0, r.stderr);
  assert.match(r.calls, /checksum\ncrane\ncrane\ndocker load/);
  assert.ok(r.godebug.split('\n').filter(Boolean).every(value => value === 'other=1,http2client=0'));
  assert.deepEqual(r.remaining, []);
});

test('corrupt archives are never imported and are removed on failure', () => {
  const result = mockTransfer({ corrupt: true });
  assert.notEqual(result.status, 0);
  assert.doesNotMatch(result.calls, /docker load/);
  assert.deepEqual(result.remaining, []);
});

test('an inspectable but unstartable cache is rejected offline and repaired online', () => {
  const offline = mockTransfer({ cached: true, brokenCache: true, offline: true });
  assert.notEqual(offline.status, 0);
  assert.doesNotMatch(offline.calls, /crane|docker load/);
  const online = mockTransfer({ cached: true, brokenCache: true });
  assert.equal(online.status, 0, online.stderr);
  assert.match(online.calls, /docker load/);
});

test('successful Docker load with failing layer unpack never reports success', () => {
  const result = mockTransfer({ badLoad: true });
  assert.notEqual(result.status, 0);
  assert.equal(result.calls.match(/docker load/g)?.length, 2);
  assert.deepEqual(result.remaining, []);
});
test('failed client-side transfer is bounded, never loaded, and cleans up', () => {
  const r = mockTransfer({ fail: true });
  assert.notEqual(r.status, 0);
  assert.equal(r.calls.match(/^crane$/gm)?.length, 2);
  assert.doesNotMatch(r.calls, /docker load/);
  assert.deepEqual(r.remaining, []);
  assert.doesNotMatch(r.stderr, /private-secret|sig=/);
  assert.match(r.stderr, /redacted-url/);
});
test('cached and offline image paths never download tools or image layers', () => {
  const cached = mockTransfer({ cached: true });
  assert.equal(cached.status, 0);
  assert.doesNotMatch(cached.calls, /curl|crane/);
  const offline = mockTransfer({ offline: true });
  assert.notEqual(offline.status, 0);
  assert.doesNotMatch(offline.calls, /curl|crane/);
});


test('new builds and release preview share shuming image names without renaming live resources', () => {
  for (const service of ['backend', 'mcp', 'web']) {
    assert.ok(workflow.includes('ghcr.io/duan-jia/shuming-' + service + ':'));
  }
  assert.doesNotMatch(workflow, /ghcr\.io\/duan-jia\/fast-llm-reasoner-/);
  const release = readFileSync(path.join(root, 'deploy/server/release.py'), 'utf8');
  assert.ok(release.includes('ghcr.io/duan-jia/shuming-{service}:{revision}'));
  assert.ok(release.includes("'web': 'fast-llm-reasoner-web-web-1'"));
  const compose = JSON.parse(readFileSync(path.join(root, 'deploy/server/compose.json'), 'utf8'));
  assert.equal(compose.services.web.image, 'shuming-web:local');
  assert.equal(compose.name, 'fast-llm-reasoner-web');
});
