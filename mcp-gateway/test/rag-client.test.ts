import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import test from 'node:test';
import type { ServicePrincipal } from '../src/config.js';
import { sha256Token } from '../src/config.js';
import { RagApiError, RagClient } from '../src/rag-client.js';

const input = { query: 'retrieval', top_k: 3, mode: 'chunks' as const };

test('service-account auth validates tenant and refreshes once after 401', async () => {
  let loginCount = 0;
  const searchTokens: string[] = [];
  const upstream = createServer((req, res) => {
    if (req.url === '/api/auth/login') {
      loginCount += 1;
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ token: `jwt-${loginCount}`, user: { tenant_id: 7 } }));
      return;
    }
    if (req.url === '/api/documents/search') {
      searchTokens.push(req.headers.authorization ?? '');
      if (searchTokens.length === 1) {
        res.statusCode = 401;
        res.end(JSON.stringify({ error: 'Unauthorized' }));
        return;
      }
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({
        query: input.query,
        rewritten_query: input.query,
        query_rewritten: false,
        query_aliases: [],
        results: []
      }));
      return;
    }
    res.statusCode = 404;
    res.end();
  });
  await new Promise<void>(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const address = upstream.address();
  assert(address && typeof address !== 'string');
  const principal: ServicePrincipal = {
    name: 'nanoclaw',
    tokenSha256: sha256Token('external'),
    tenantId: 7,
    upstreamUsername: 'mcp_nanoclaw',
    upstreamPassword: 'password'
  };

  try {
    const result = await new RagClient(`http://127.0.0.1:${address.port}`, 1000).search(principal, input);
    assert.equal(result.query, input.query);
    assert.equal(loginCount, 2);
    assert.deepEqual(searchTokens, ['Bearer jwt-1', 'Bearer jwt-2']);
  } finally {
    upstream.close();
  }
});

test('service-account auth blocks a mismatched upstream tenant', async () => {
  let searchCalled = false;
  const upstream = createServer((req, res) => {
    if (req.url === '/api/auth/login') {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ token: 'wrong-tenant-jwt', user: { tenant_id: 8 } }));
      return;
    }
    searchCalled = true;
    res.end();
  });
  await new Promise<void>(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const address = upstream.address();
  assert(address && typeof address !== 'string');
  const principal: ServicePrincipal = {
    name: 'nanoclaw',
    tokenSha256: sha256Token('external'),
    tenantId: 7,
    upstreamUsername: 'mcp_nanoclaw',
    upstreamPassword: 'password'
  };

  try {
    await assert.rejects(
      () => new RagClient(`http://127.0.0.1:${address.port}`, 1000).search(principal, input),
      (error: unknown) => error instanceof RagApiError && /tenant/.test(error.message)
    );
    assert.equal(searchCalled, false);
  } finally {
    upstream.close();
  }
});
