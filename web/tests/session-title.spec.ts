import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sessionTitle } from '../src/lib/session-title.ts';

test('first-question titles normalize whitespace and mark truncation only when needed', () => {
  assert.equal(sessionTitle('  如何理解\n  RDMA 的作用？  '), '如何理解 RDMA 的作用？');
  assert.equal(sessionTitle('问'.repeat(24)), '问'.repeat(24));
  assert.equal(sessionTitle('问'.repeat(25)), '问'.repeat(24) + '…');
});

test('title truncation keeps emoji sequences and combining characters intact', () => {
  assert.equal(sessionTitle('问'.repeat(23) + '👩🏽‍💻后续'), '问'.repeat(23) + '👩🏽‍💻…');
  assert.equal(sessionTitle('e\u0301'.repeat(25)), 'e\u0301'.repeat(24) + '…');
});
