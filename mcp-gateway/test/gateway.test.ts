import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import test from 'node:test';
import { Client, StreamableHTTPClientTransport } from '@modelcontextprotocol/client';
import { createGateway } from '../src/app.js';
import { sha256Token, type GatewayConfig } from '../src/config.js';

test('search_knowledge forwards the tenant-bound upstream token', async () => {
  let receivedAuthorization = '';
  let receivedBody = '';
  const upstream = createServer((req, res) => {
    receivedAuthorization = req.headers.authorization ?? '';
    req.setEncoding('utf8');
    req.on('data', chunk => { receivedBody += chunk; });
    req.on('end', () => {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({
        query: 'vector search',
        rewritten_query: 'vector search',
        query_rewritten: false,
        query_aliases: [],
        results: [{ doc_id: 9, filename: 'paper.md', chunk_index: 1, score: 0.9, content: 'evidence' }]
      }));
    });
  });
  await new Promise<void>(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const upstreamAddress = upstream.address();
  assert(upstreamAddress && typeof upstreamAddress !== 'string');

  const config: GatewayConfig = {
    host: '127.0.0.1',
    port: 0,
    allowedHosts: ['127.0.0.1'],
    ragApiBaseUrl: `http://127.0.0.1:${upstreamAddress.port}`,
    ragTimeoutMs: 1000,
    principals: [{
      name: 'nanoclaw',
      tokenSha256: sha256Token('agent-secret'),
      tenantId: 7,
      upstreamBearerToken: 'tenant-7-jwt'
    }]
  };
  const gateway = createGateway(config);
  const gatewayServer = gateway.listen();
  await new Promise<void>(resolve => gatewayServer.once('listening', resolve));
  const gatewayAddress = gatewayServer.address();
  assert(gatewayAddress && typeof gatewayAddress !== 'string');

  const client = new Client({ name: 'test-client', version: '1.0.0' });
  const transport = new StreamableHTTPClientTransport(
    new URL(`http://127.0.0.1:${gatewayAddress.port}/mcp`),
    { requestInit: { headers: { authorization: 'Bearer agent-secret' } } }
  );
  try {
    await client.connect(transport);
    const tools = await client.listTools();
    assert.deepEqual(tools.tools.map(tool => tool.name), ['search_knowledge']);
    const result = await client.callTool({
      name: 'search_knowledge',
      arguments: { query: 'vector search', top_k: 3, mode: 'chunks' }
    });
    assert.equal(result.isError, undefined);
    assert.equal(receivedAuthorization, 'Bearer tenant-7-jwt');
    assert.deepEqual(JSON.parse(receivedBody), { query: 'vector search', top_k: 3, mode: 'chunks' });

    const injectedTenant = await client.callTool({
      name: 'search_knowledge',
      arguments: { query: 'vector search', tenant_id: 999 }
    });
    assert.equal(injectedTenant.isError, true);
  } finally {
    await client.close();
    gatewayServer.close();
    upstream.close();
    await gateway.close();
  }
});

test('rejects unauthenticated MCP requests', async () => {
  const config: GatewayConfig = {
    host: '127.0.0.1',
    port: 0,
    allowedHosts: ['127.0.0.1'],
    ragApiBaseUrl: 'http://127.0.0.1:9',
    ragTimeoutMs: 100,
    principals: [{
      name: 'nanoclaw',
      tokenSha256: sha256Token('agent-secret'),
      tenantId: 7,
      upstreamBearerToken: 'tenant-7-jwt'
    }]
  };
  const gateway = createGateway(config);
  const gatewayServer = gateway.listen();
  await new Promise<void>(resolve => gatewayServer.once('listening', resolve));
  const address = gatewayServer.address();
  assert(address && typeof address !== 'string');
  try {
    const response = await fetch(`http://127.0.0.1:${address.port}/mcp`, { method: 'POST' });
    assert.equal(response.status, 401);
    assert.match(response.headers.get('www-authenticate') ?? '', /^Bearer/);
  } finally {
    gatewayServer.close();
    await gateway.close();
  }
});
