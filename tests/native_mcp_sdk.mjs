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
for (const [path, names] of [['/mcp', ['search_knowledge', 'fetch_knowledge', 'search_web', 'fetch_web']], ['/mcp/knowledge', ['search_knowledge', 'fetch_knowledge']], ['/mcp/web', ['search_web', 'fetch_web']]]) {
  const client = new Client({ name: 'native-mcp-sdk-test', version: '1.0.0' });
  const transport = new StreamableHTTPClientTransport(new URL(path, endpoint), {
    requestInit: { headers: { authorization: `Bearer ${token}` } }
  });
  try {
    await client.connect(transport);
    const tools = await client.listTools();
    assert.deepEqual(tools.tools.map(tool => tool.name), names);
    for (const name of names) {
      const arguments_ = name === 'fetch_knowledge' ? { doc_id: 7, representation: 'source' } : name === 'fetch_web' ? { url: 'https://example.org/sdk' } : { query: 'SDK integration evidence' };
      const result = await client.callTool({ name, arguments: arguments_ });
      assert.equal(result.isError, false);
      if (name.startsWith('search_')) assert.equal(result.structuredContent.results.length, 1);
      if (name === 'fetch_knowledge') assert.equal(result.content[1].type, 'resource_link');
      if (name === 'fetch_web') assert.equal(result.structuredContent.title, 'Fetched page');
      assert.deepEqual(JSON.parse(result.content[0].text), result.structuredContent);
    }
    await client.ping();
  } finally {
    await client.close();
  }
}
console.log('Official MCP SDK: all three native endpoints passed');
