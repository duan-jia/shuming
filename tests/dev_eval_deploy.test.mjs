import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {ROOT, PROJECT, parseEnv, createEnvironment, dockerEnvironment, composeArgs, assertSafeRoot, preparationState} from '../deploy/dev-eval/manage.mjs';
const compose = JSON.parse(fs.readFileSync(new URL('../deploy/dev-eval/compose.json', import.meta.url)));
const staging = JSON.parse(fs.readFileSync(new URL('../deploy/staging/compose.json', import.meta.url)));

test('only a persistent database is deployed, not backend/frontend/MCP or workflow', () => {
  assert.deepEqual(Object.keys(compose.services), ['mysql']);
  assert.equal(compose.name, PROJECT);
  assert.equal(compose.services.mysql.restart, 'unless-stopped');
  assert.equal(fs.existsSync(new URL('../.github/workflows/dev-eval.yml', import.meta.url)), false);
});
test('CI/production resource names and directories are not reused', () => {
  assert.notEqual(compose.name, staging.name);
  assert.equal(compose.volumes['mysql-data'].name, 'shuming-dev-eval_mysql-data');
  assert.equal(compose.networks['dev-eval'].name, 'shuming-dev-eval_network');
  assert.ok(!JSON.stringify(compose).includes('/srv/fastllm-staging'));
  assert.ok(!JSON.stringify(compose).includes('/opt/fast-llm-reasoner'));
  assert.equal(staging.services.mysql.restart, 'no');
});
test('database port is only published on server loopback and app uses a non-root account', () => {
  assert.deepEqual(compose.services.mysql.ports, ['127.0.0.1:23306:3306']);
  assert.equal(compose.services.mysql.environment.MYSQL_USER, 'shuming');
  assert.equal(compose.services.mysql.environment.MYSQL_DATABASE, 'llm_platform');
  assert.ok(compose.services.mysql.volumes.some(v => v === ROOT+'/init.sql:/docker-entrypoint-initdb.d/001-init.sql:ro'));
});
test('credential generation is independent and parser rejects overrides/injection', () => {
  const text = createEnvironment(), env = parseEnv(text);
  assert.ok(env.MYSQL_PASSWORD.length >= 32);
  assert.notEqual(env.MYSQL_PASSWORD, env.MYSQL_ROOT_PASSWORD);
  assert.notEqual(text, createEnvironment());
  for (const invalid of [text+'COMPOSE_PROJECT_NAME=production\n', text+'MYSQL_PASSWORD='+env.MYSQL_PASSWORD+'\n', text.replace(env.MYSQL_PASSWORD,'$(id)'), text.replace(env.MYSQL_PASSWORD,'short')]) assert.throws(()=>parseEnv(invalid));
});
test('commands pin target and cannot remove volumes or inherit external Docker context', () => {
  assert.throws(()=>composeArgs('down'));
  assert.throws(()=>composeArgs('rm'));
  for(const command of ['up','stop','status','check']) {
    const a=composeArgs(command);
    assert.equal(a[a.indexOf('--project-name')+1],PROJECT);
    assert.equal(a[a.indexOf('--env-file')+1],ROOT+'/.env');
    assert.ok(!a.includes('--volumes'));
  }
  const env=dockerEnvironment({PATH:'/bin',HOME:'/home/operator',DOCKER_HOST:'tcp://production',DOCKER_CONTEXT:'remote',COMPOSE_FILE:'evil',MYSQL_PASSWORD:'evil'});
  assert.equal(env.DOCKER_CONTEXT,'default');
  assert.equal(env.DOCKER_HOST,undefined);
  assert.equal(env.COMPOSE_FILE,undefined);
  assert.equal(env.MYSQL_PASSWORD,undefined);
});
test('preparation preserves existing credentials and refuses partial/missing recovery data', () => {
  assert.equal(preparationState(true,true,true),'preserve');
  assert.equal(preparationState(false,false,false),'create');
  assert.throws(()=>preparationState(true,false,true));
  assert.throws(()=>preparationState(false,true,false));
  assert.throws(()=>preparationState(false,false,true));
});
function mockFs(bad, mode=0o600) {
  return {lstatSync: location=>{
    const file=location.endsWith('/.env')||location.endsWith('/init.sql');
    return {isSymbolicLink:()=>location===bad,isFile:()=>file,isDirectory:()=>!file,mode:file?(location.endsWith('/init.sql')?0o644:mode):0o755};
  }};
}
test('root and file symlinks, including dangling links, are rejected', () => {
  for(const bad of ['/srv',ROOT,ROOT+'/.env',ROOT+'/init.sql']) assert.throws(()=>assertSafeRoot(mockFs(bad)),/Symlink/);
});
test('credentials must be private but public schema remains readable by MySQL container UID', () => {
  assert.throws(()=>assertSafeRoot(mockFs('',0o644)),/permissions/);
  assert.doesNotThrow(()=>assertSafeRoot(mockFs('')));
});
