const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const Module = require('node:module');
const ts = require('../web/node_modules/typescript');
const path = require.resolve('../web/src/lib/source-files.ts');
const compiled = ts.transpileModule(fs.readFileSync(path, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
});
const mod = new Module(path, module);
mod._compile(compiled.outputText, path);
const { normalizeSourceFiles, downloadDocumentFile } = mod.exports;

test('untrusted references: deduplicate IDs, ignore invalid fields, never keep supplied URLs', () => {
  assert.deepEqual(normalizeSourceFiles([
    {doc_id: 1, filename: 'paper.pdf', url: 'https://evil.invalid'},
    {doc_id: 1, filename: 'duplicate.pdf'},
    {doc_id: '2', filename: 'invalid.pdf'}, {doc_id: -1, filename: 'invalid.pdf'},
    {doc_id: 2147483648, filename: 'invalid.pdf'}, {doc_id: 3, filename: ' '}, null,
  ]), [{doc_id: 1, filename: 'paper.pdf'}]);
  for (const value of [undefined, null, {}, 'invalid']) assert.deepEqual(normalizeSourceFiles(value), []);
});
test('bounded files and filenames', () => {
  const refs = normalizeSourceFiles(Array.from({length: 100}, (_, i) => ({doc_id: i+1, filename: 'a'.repeat(1000)})));
  assert.equal(refs.length, 40);
  assert.equal(refs[0].filename.length, 512);
});
test('download reuses authenticated document route, never token in URL', async () => {
  const originalFetch = global.fetch, originalDocument = global.document, originalWindow = global.window;
  const originalCreate = URL.createObjectURL, originalRevoke = URL.revokeObjectURL;
  let clicked = false, removed = false, filename = '', requested;
  global.fetch = async (url, options) => { requested = {url, options}; return new Response('%PDF-1.7'); };
  const anchor = {set href(value) { assert.equal(value, 'blob:test'); }, set download(value) { filename = value; }, click() { clicked = true; }, remove() { removed = true; }};
  global.document = {createElement: () => anchor, body: {appendChild: value => assert.equal(value, anchor)}};
  global.window = {setTimeout: fn => fn()};
  URL.createObjectURL = () => 'blob:test'; URL.revokeObjectURL = value => assert.equal(value, 'blob:test');
  try {
    await downloadDocumentFile(1, 'paper.pdf', 'pdf', 'fixture-token');
    assert.equal(requested.url, '/api/documents/1/download/pdf');
    assert.equal(requested.options.headers.Authorization, 'Bearer fixture-token');
    assert.equal(requested.options.cache, 'no-store');
    assert.ok(clicked && removed); assert.equal(filename, 'paper.pdf');
  } finally { global.fetch = originalFetch; global.document = originalDocument; global.window = originalWindow; URL.createObjectURL = originalCreate; URL.revokeObjectURL = originalRevoke; }
});
test('permissions, missing files and expired login report an error instead of download', async () => {
  const original = global.fetch;
  try {
    for (const status of [401, 403, 404, 500]) {
      global.fetch = async () => Response.json({error:'cannot download'}, {status});
      await assert.rejects(downloadDocumentFile(1, 'paper.pdf', 'pdf', 'fixture-token'));
    }
    global.fetch = () => { throw Error('must not make request'); };
    await assert.rejects(downloadDocumentFile(1, 'paper.pdf', 'pdf', ''), /登录/);
    await assert.rejects(downloadDocumentFile(-1, 'paper.pdf', 'pdf', 'fixture-token'), /登录/);
  } finally { global.fetch = original; }
});
