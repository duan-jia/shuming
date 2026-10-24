// Run against npm start with the default backend (127.0.0.1:9006).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';

test('Next proxy forwards credentials and delivers first SSE event before upstream finishes', { timeout: 15000 }, async () => {
  let finish: (() => void) | undefined;
  const server = createServer((req, res) => {
    assert.equal(req.url, '/api/chat/stream');
    assert.equal(req.headers.authorization, 'Bearer test-only');
    assert.equal(req.headers['x-request-id'], 'stream-test');
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      assert.equal(JSON.parse(body).message, '你好');
      res.writeHead(200, { 'Content-Type': 'text/event-stream' });
      res.write('data: {"type":"answer","content":"第一段"}\n\n');
      finish = () => res.end('data: {"type":"done"}\n\n');
    });
  });
  await new Promise<void>(resolve => server.listen(9006, '127.0.0.1', resolve));
  let reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
  try {
    const url = (process.env.TEST_URL || 'http://localhost:3000') + '/api/chat/stream';
    assert.equal((await fetch(url, { method: 'POST', body: '{}' })).status, 401);
    const response = await fetch(url, {
      method: 'POST', headers: { Authorization: 'Bearer test-only', 'Content-Type': 'application/json', 'X-Request-Id': 'stream-test' },
      body: JSON.stringify({message:'你好'}), signal: AbortSignal.timeout(8000),
    });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('x-accel-buffering'), 'no');
    reader = response.body!.getReader();
    const first = await reader.read();
    assert.match(new TextDecoder().decode(first.value), /第一段/);
    assert.equal(first.done, false);
    finish!();
    let tail = '';
    for (;;) { const chunk = await reader.read(); if(chunk.done) break; tail += new TextDecoder().decode(chunk.value); }
    assert.match(tail, /done/);
  } finally {
    finish?.();
    await reader?.cancel().catch(() => {});
    server.closeAllConnections();
    await new Promise<void>(resolve => server.close(() => resolve()));
  }
});
