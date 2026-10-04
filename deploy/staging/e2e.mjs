import assert from 'node:assert/strict';
import { Client, StreamableHTTPClientTransport } from '@modelcontextprotocol/client';

const token = process.env.MCP_E2E_TOKEN;
if (!token) throw new Error('MCP_E2E_TOKEN is required');

const endpoint = process.env.MCP_E2E_URL ?? 'http://127.0.0.1:3100/mcp';
const expectedMarker = 'ORCHID-NEBULA-7429';
const client = new Client({ name: 'staging-e2e', version: '1.0.0' });
const transport = new StreamableHTTPClientTransport(new URL(endpoint), {
  requestInit: { headers: { authorization: `Bearer ${token}` } }
});

try {
  await client.connect(transport);

  const tools = await client.listTools();
  assert.deepEqual(tools.tools.map(tool => tool.name), ['search_knowledge']);

  const result = await client.callTool({
    name: 'search_knowledge',
    arguments: {
      query: 'What is the staging verification codename?',
      top_k: 3,
      mode: 'chunks'
    }
  });
  assert.notEqual(result.isError, true, JSON.stringify(result));
  assert.match(JSON.stringify(result), new RegExp(expectedMarker));

  const injectedTenant = await client.callTool({
    name: 'search_knowledge',
    arguments: {
      query: 'staging verification codename',
      tenant_id: 999
    }
  });
  assert.equal(injectedTenant.isError, true, 'tenant_id injection must be rejected');

  console.log('Staging MCP E2E passed: auth, tenant binding, retrieval, and schema rejection are healthy.');
} finally {
  await client.close();
}
