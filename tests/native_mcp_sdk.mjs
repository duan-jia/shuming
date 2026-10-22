// Use the official MCP client SDK against the real C++ HTTP server.
// SDK module directory is explicit so this test does not add a root Node project.
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const sdkRoot = process.env.MCP_SDK_ROOT;
if (!sdkRoot) throw new Error('MCP_SDK_ROOT must point to @modelcontextprotocol/client');
const { Client, StreamableHTTPClientTransport } = await import(pathToFileURL(resolve(sdkRoot, 'dist/index.mjs')));
const endpoint = process.env.NATIVE_MCP_URL;
const token = process.env.NATIVE_MCP_TOKEN;
if (!endpoint || !token) throw new Error('NATIVE_MCP_URL and NATIVE_MCP_TOKEN are required');
for (const [path, names] of [['/mcp', ['search_knowledge', 'search_web']], ['/mcp/knowledge', ['search_knowledge']], ['/mcp/web', ['search_web']]]) {
  const client = new Client({ name: 'native-mcp-sdk-test', version: '1.0.0' });
  const transport = new StreamableHTTPClientTransport(new URL(path, endpoint), {
    requestInit: { headers: { authorization: `Bearer ${token}` } }
  });
  try {
    await client.connect(transport);
    const tools = await client.listTools();
    assert.deepEqual(tools.tools.map(tool => tool.name), names);
    for (const name of names) {
      const result = await client.callTool({ name, arguments: { query: 'SDK integration evidence' } });
      assert.equal(result.isError, false);
      assert.equal(result.structuredContent.results.length, 1);
      assert.deepEqual(JSON.parse(result.content[0].text), result.structuredContent);
    }
    await client.ping();
  } finally {
    await client.close();
  }
}
console.log('Official MCP SDK: all three native endpoints passed');
