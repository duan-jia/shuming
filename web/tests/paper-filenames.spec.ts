import test from "node:test";
import assert from "node:assert/strict";
import { safePaperStem, safePdfFilename, isPaperStorageName } from "../src/lib/paper-filenames.ts";

test("paper names preserve readable titles while remaining portable", () => {
  assert.equal(safePaperStem('REPS: Recycled / Packets?'), 'REPS_ Recycled _ Packets_');
  assert.equal(safePaperStem('中文论文\n  Title. '), '中文论文 Title');
  for (const name of ['CON', 'nul', 'COM1', 'LPT9', 'CON.notes']) {
    assert.equal(safePaperStem(name), `_${name}`);
  }
  for (const name of ['', '...', '  ']) assert.equal(safePaperStem(name), 'document');
  assert.equal(safePdfFilename('C:\\papers\\REPS: Recycled.PDF'), 'REPS_ Recycled.pdf');
  assert.equal(safePdfFilename('../REPS: Recycled.pdf'), 'REPS_ Recycled.pdf');
  assert.equal(safePdfFilename('notes.md'), '');
  assert.equal(safePdfFilename('.pdf'), '');
});

test("UTF-8 limits keep new artifacts compatible with historical storage validation", () => {
  const id = '00000000-0000-0000-0000-000000000001';
  const stem = safePaperStem('中'.repeat(100));
  assert.equal(Buffer.byteLength(stem), 159);
  assert.equal(stem.includes('\uFFFD'), false);
  assert.equal(isPaperStorageName(`${id}--${stem}.pdf`), true);
  assert.equal(isPaperStorageName(`${id}.pdf`), true);
  assert.equal(isPaperStorageName(`${id}--${'中'.repeat(60)}.pdf`), true);
  assert.equal(isPaperStorageName(`${id}--${'中'.repeat(61)}.pdf`), false);
  for (const name of [`../${id}.pdf`, `${id}--a/b.pdf`, `${id}--a\\b.pdf`, `${id}--a\n.pdf`]) {
    assert.equal(isPaperStorageName(name), false);
  }
});
