#!/usr/bin/env node
/** Database-only helper; no backend deployment, workflow changes or SSH calls. */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';
import { spawnSync } from 'node:child_process';
export const ROOT = '/srv/shuming-dev-eval';
export const PROJECT = 'shuming-dev-eval';
export const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const KEYS = ['MYSQL_ROOT_PASSWORD', 'MYSQL_PASSWORD'];
export function parseEnv(text) {
  const result = {};
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('#')) continue;
    const match = /^([A-Z_]+)=([A-Za-z0-9_-]{24,})$/.exec(line);
    if (!match || !KEYS.includes(match[1]) || Object.hasOwn(result, match[1])) throw Error('Invalid or unexpected credential entry');
    result[match[1]] = match[2];
  }
  if (KEYS.some(key => !result[key])) throw Error('Incomplete credentials');
  return result;
}
export function createEnvironment() {
  return KEYS.map(key => `${key}=${randomBytes(36).toString('base64url')}`).join('\n') + '\n';
}
export function dockerEnvironment(source = process.env) {
  // In particular, ignore DOCKER_HOST, DOCKER_CONTEXT and COMPOSE_* overrides.
  return {PATH: source.PATH, HOME: source.HOME, DOCKER_CONTEXT: 'default'};
}
export function composeArgs(command) {
  const operations = {
    check: ['config', '--quiet'],
    up: ['up', '-d', '--pull', 'missing', '--wait', '--wait-timeout', '180'],
    stop: ['stop', '--timeout', '20'],
    status: ['ps'],
  };
  if (!Object.hasOwn(operations, command)) throw Error('Unsupported operation');
  return ['compose', '--project-name', PROJECT, '--env-file', `${ROOT}/.env`, '-f', `${REPO}/deploy/dev-eval/compose.json`, ...operations[command]];
}
export function assertSafeRoot(io = fs) {
  for (const location of ['/srv', ROOT, `${ROOT}/.env`, `${ROOT}/init.sql`]) {
    let stat;
    try { stat = io.lstatSync(location); } catch (error) { if (error.code === 'ENOENT') continue; throw error; }
    if (stat.isSymbolicLink()) throw Error(`Symlink forbidden: ${location}`);
    if (location === `${ROOT}/.env` || location === `${ROOT}/init.sql`) {
      const forbidden = location.endsWith('/.env') ? 0o077 : 0o022;
      if (!stat.isFile() || (stat.mode & forbidden)) throw Error(`Unsafe file permissions: ${location}`);
    } else if (!stat.isDirectory() || (stat.mode & (location === ROOT ? 0o077 : 0o022))) {
      throw Error(`Unsafe directory permissions (deployment root must be private): ${location}`);
    }
  }
}
export function preparationState(envExists, schemaExists, volumeExists) {
  if (envExists !== schemaExists) throw Error('Partial preparation: restore missing credentials/schema; never rotate an existing database password');
  if (envExists) return 'preserve';
  if (volumeExists) throw Error('Existing database volume without credentials: restore its original .env');
  return 'create';
}
function load() {
  assertSafeRoot();
  if (!fs.existsSync(`${ROOT}/init.sql`)) throw Error('Missing schema snapshot');
  return parseEnv(fs.readFileSync(`${ROOT}/.env`, 'utf8'));
}
function prepare() {
  assertSafeRoot();
  const envFile = `${ROOT}/.env`, schemaFile = `${ROOT}/init.sql`;
  const a = fs.existsSync(envFile), b = fs.existsSync(schemaFile);
  if (a || b) {
    preparationState(a, b, false); load();
    console.log('Existing credentials and schema preserved. No database operation performed.'); return;
  }
  const volumes = spawnSync('docker', ['volume', 'ls', '--filter', 'name=^shuming-dev-eval_mysql-data$', '--format', '{{.Name}}'], {encoding: 'utf8', env: dockerEnvironment(), timeout: 15000});
  if (volumes.error || volumes.status !== 0) throw Error('Cannot inspect local Docker; no credentials created');
  preparationState(false, false, !!volumes.stdout.trim());
  fs.mkdirSync(ROOT, {recursive: true, mode: 0o700});
  const schema = fs.readFileSync(`${REPO}/sql/init.sql`);
  // Exclusive writes fail closed on races. Partial preparation requires recovery,
  // rather than changing the password of an existing persistent volume.
  fs.writeFileSync(schemaFile, schema, {mode: 0o644, flag: 'wx'});
  fs.writeFileSync(envFile, createEnvironment(), {mode: 0o600, flag: 'wx'});
  console.log('Prepared independent database credentials/schema; secrets were not printed.');
}
function main() {
  const [command, ...extra] = process.argv.slice(2);
  if (extra.length) throw Error('Unexpected arguments');
  if (command === 'prepare') return prepare();
  const args = composeArgs(command); load();
  const result = spawnSync('docker', args, {stdio: 'inherit', env: dockerEnvironment(), timeout: 240000});
  if (result.error || result.status !== 0) throw Error('Docker operation failed; no automatic deletion or rollback performed');
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
