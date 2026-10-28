import test from 'node:test';
import assert from 'node:assert/strict';
import { acceptAuth, authFetch, refreshAuth, logoutAuth, subscribeAuth } from '../src/lib/auth-client.ts';

const user = { id: 1, username: 'fixture', tenant_id: 1, role: 'user' };
function token(expiry) {
  const now = Math.floor(Date.now() / 1000);
  return 'test.' + Buffer.from(JSON.stringify({ iat: now - 10, exp: now + expiry })).toString('base64url') + '.signature';
}

test('expired access requests share one renewal and send the new token', async () => {
  const original = globalThis.fetch;
  const fresh = token(600); let renewals = 0; const seen = [];
  globalThis.fetch = async (url, init) => {
    if (url === '/api/auth/refresh') { ++renewals; await new Promise(resolve => setTimeout(resolve, 10)); return Response.json({ token: fresh, user }); }
    seen.push(new Headers(init.headers).get('Authorization')); return Response.json({ ok: true });
  };
  try {
    acceptAuth({ token: token(-1), user });
    await Promise.all(Array.from({ length: 5 }, () => authFetch('/api/auth/me', { headers: { Authorization: 'Bearer obsolete' } })));
    assert.equal(renewals, 1); assert.equal(seen.length, 5); assert.ok(seen.every(value => value === `Bearer ${fresh}`));
  } finally { acceptAuth(null); globalThis.fetch = original; }
});

test('401 retries once; a network failure preserves the renewable session', async () => {
  const original = globalThis.fetch; let calls = 0; let last; const stop = subscribeAuth(value => { last = value; });
  globalThis.fetch = async url => url === '/api/auth/refresh' ? Response.json({ token: token(600), user })
    : ++calls === 1 ? Response.json({}, { status: 401 }) : Response.json({ ok: true });
  try {
    acceptAuth({ token: token(600), user });
    assert.equal((await authFetch('/api/users')).status, 200); assert.equal(calls, 2);
    globalThis.fetch = async () => Response.json({}, { status: 503 });
    await assert.rejects(refreshAuth()); assert.ok(last);
    globalThis.fetch = async () => Response.json({}, { status: 401 });
    assert.equal(await refreshAuth(), null); assert.equal(last, null);
  } finally { stop(); acceptAuth(null); globalThis.fetch = original; }
});

test('guest expiry does not renew; logout clears the current identity', async () => {
  const original = globalThis.fetch; let calls = 0;
  globalThis.fetch = async () => { ++calls; return Response.json({}); };
  try {
    acceptAuth({ token: token(-1), user: { ...user, role: 'guest' } });
    await assert.rejects(authFetch('/api/sessions')); assert.equal(calls, 0);
    acceptAuth({ token: token(600), user }); await logoutAuth(); assert.equal(calls, 1);
    await authFetch('/api/sessions'); assert.equal(calls, 2);
  } finally { acceptAuth(null); globalThis.fetch = original; }
});

test('a late refresh response cannot resurrect a signed-out identity', async () => {
  const original = globalThis.fetch; let resolve; let last;
  const stop = subscribeAuth(value => { last = value; });
  globalThis.fetch = () => new Promise(done => { resolve = done; });
  try {
    acceptAuth({ token: token(600), user });
    const pending = refreshAuth();
    await new Promise(done => setImmediate(done));
    acceptAuth(null);
    resolve(Response.json({ token: token(600), user }));
    assert.equal(await pending, null); assert.equal(last, null);
  } finally { stop(); acceptAuth(null); globalThis.fetch = original; }
});

test('anonymous bootstrap does not clear an in-progress login form', async () => {
  const original = globalThis.fetch; const changes = [];
  acceptAuth(null);
  const stop = subscribeAuth(value => changes.push(value));
  globalThis.fetch = async () => Response.json({}, { status: 401 });
  try { assert.equal(await refreshAuth(), null); assert.equal(changes.length, 0); }
  finally { stop(); acceptAuth(null); globalThis.fetch = original; }
});

test('a long PDF upload renews before its multi-step processing begins', async () => {
  const original = globalThis.fetch; const fresh = token(900); let renewals = 0;
  globalThis.fetch = async (url, init) => {
    if (url === '/api/auth/refresh') { ++renewals; return Response.json({ token: fresh, user }); }
    assert.equal(new Headers(init.headers).get('Authorization'), `Bearer ${fresh}`);
    return Response.json({});
  };
  try { acceptAuth({ token: token(120), user }); await authFetch('/api/papers/upload', { method: 'POST' }); assert.equal(renewals, 1); }
  finally { acceptAuth(null); globalThis.fetch = original; }
});
