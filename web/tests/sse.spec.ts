import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createSseParser, type SseEvent } from '../src/lib/sse.ts';

test('SSE handles arbitrary byte boundaries, CRLF, comments and multiple data lines', () => {
  const events: SseEvent[] = [];
  const parse = createSseParser(event => events.push(event));
  const decoder = new TextDecoder();
  const bytes = new TextEncoder().encode(': heartbeat\r\nevent:message\r\ndata:{"content":"你好🌱"}\r\n\r\nevent: error\ndata: first\ndata: second\n\ndata:[DONE]\n\n');
  for (const byte of bytes) parse(decoder.decode(new Uint8Array([byte]), { stream: true }));
  parse(decoder.decode());
  assert.deepEqual(events, [
    {event: 'message', data: '{"content":"你好🌱"}'},
    {event: 'error', data: 'first\nsecond'},
    {event: 'message', data: '[DONE]'},
  ]);
});

test('incomplete events are not dispatched; event type resets at blank line', () => {
  const events: SseEvent[] = [];
  const parse = createSseParser(event => events.push(event));
  parse('event:error\n\ndata:ok\n');
  assert.equal(events.length, 0);
  parse('\n');
  assert.deepEqual(events, [{event: 'message', data: 'ok'}]);
});
