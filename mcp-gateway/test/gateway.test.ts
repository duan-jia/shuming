import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import test from 'node:test';
import { Client, StreamableHTTPClientTransport } from '@modelcontextprotocol/client';
import { createGateway } from '../src/app.js';
import { sha256Token, type GatewayConfig } from '../src/config.js';

test('native routes preserve RPC and protocol, authenticate and refresh JWT', async () => {
  let logins = 0;
  const paths: string[] = [];
  const upstream = createServer((req, res) => {
    let text = '';
    req.on('data', chunk => { text += chunk; });
    req.on('end', () => {
      res.setHeader('content-type', 'application/json');
      if (req.url === '/api/auth/login') {
        res.end(JSON.stringify({ token: `jwt-${++logins}`, user: { tenant_id: 7 } }));
        return;
      }
      assert.equal(req.headers['mcp-protocol-version'], '2025-11-25');
      assert.equal(req.headers.origin, 'http://127.0.0.1');
      paths.push(req.url!);
      if (req.headers.authorization === 'Bearer jwt-1') {
        res.statusCode = 401;
        res.end('{}');
        return;
      }
      assert.equal(req.headers.authorization, 'Bearer jwt-2');
      const rpc = JSON.parse(text);
      if (!('id' in rpc)) {
        res.statusCode = 202;
        res.end();
        return;
      }
      res.end(JSON.stringify({ jsonrpc: '2.0', id: rpc.id, result: { tools: [
        { name: req.url === '/mcp/web' ? 'search_web' : 'fetch_knowledge' }
      ] } }));
    });
  });
  await new Promise<void>(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const address = upstream.address();
  assert(address && typeof address !== 'string');
  const gateway = createGateway({ host: '127.0.0.1', port: 0, allowedHosts: ['127.0.0.1'],
    ragApiBaseUrl: `http://127.0.0.1:${address.port}`, ragTimeoutMs: 1000,
    principals: [{ name: 'waku', tokenSha256: sha256Token('secret'), tenantId: 7,
      upstreamUsername: 'service', upstreamPassword: 'password' }] });
  const server = gateway.listen();
  await new Promise<void>(resolve => server.once('listening', resolve));
  const gw = server.address();
  assert(gw && typeof gw !== 'string');
  try {
    for (const path of ['/mcp/knowledge', '/mcp/web']) {
      const url: string = `http://127.0.0.1:${gw.port}${path}`;
      assert.equal((await fetch(url, { method: 'POST' })).status, 401);
      const headers = { authorization: 'Bearer secret', 'content-type': 'application/json',
        'mcp-protocol-version': '2025-11-25', origin: 'http://127.0.0.1' };
      const response = await fetch(url, { method: 'POST', headers,
        body: JSON.stringify({ jsonrpc: '2.0', id: 42, method: 'tools/list' }) });
      assert.equal(response.status, 200);
      const rpc = await response.json();
      assert.equal(rpc.id, 42);
      assert.equal(rpc.result.tools[0].name, path === '/mcp/web' ? 'search_web' : 'fetch_knowledge');
      assert.equal((await fetch(url, { method: 'POST', headers,
        body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) })).status, 202);
      assert.equal((await fetch(url, { headers })).status, 405);
    }
    assert.equal(logins, 2);
    assert.equal(paths.length, 5);
  } finally {
    server.close();
    upstream.close();
    await gateway.close();
  }
});

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
