import assert from 'node:assert/strict';
import test from 'node:test';
import { findPrincipal, loadConfig, sha256Token } from '../src/config.js';

test('loads principals and authenticates by token hash', () => {
  const config = loadConfig({
    SERVICE_PRINCIPALS_JSON: JSON.stringify([{
      name: 'nanoclaw',
      tokenSha256: sha256Token('external-secret'),
      tenantId: 7,
      upstreamBearerToken: 'upstream-jwt'
    }])
  });
  assert.equal(findPrincipal('external-secret', config.principals)?.tenantId, 7);
  assert.equal(findPrincipal('wrong', config.principals), undefined);
});

test('accepts service-account credentials and rejects ambiguous upstream auth', () => {
  const serviceAccount = {
    name: 'nanoclaw',
    tokenSha256: sha256Token('external-secret'),
    tenantId: 7,
    upstreamUsername: 'mcp_nanoclaw',
    upstreamPassword: 'secret'
  };
  const config = loadConfig({ SERVICE_PRINCIPALS_JSON: JSON.stringify([serviceAccount]) });
  assert.equal(config.principals[0]?.upstreamUsername, 'mcp_nanoclaw');

  assert.throws(() => loadConfig({
    SERVICE_PRINCIPALS_JSON: JSON.stringify([{
      ...serviceAccount,
      upstreamBearerToken: 'also-set'
    }])
  }));
});

test('requires allowed hosts on non-loopback binds', () => {
  assert.throws(() => loadConfig({
    MCP_HOST: '0.0.0.0',
    SERVICE_PRINCIPALS_JSON: JSON.stringify([{
      name: 'nanoclaw',
      tokenSha256: sha256Token('secret'),
      tenantId: 1,
      upstreamBearerToken: 'jwt'
    }])
  }), /MCP_ALLOWED_HOSTS/);
});
