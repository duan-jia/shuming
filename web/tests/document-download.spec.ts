import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { GET } from "../src/app/api/documents/[id]/download/[format]/route.ts";

const storage = "00000000-0000-0000-0000-000000000001--Paper.pdf";
const summary = "# Paper\n\n## Problem and Context\n\nProblem.\n\n## Core Idea and Approach\n\nMethod.\n\n## Key Takeaways\n\n- Finding.\n\n## Scope and Limitations\n\nScope.\n";
const fullText = "# Paper\n\n## Paper | PDF page 1 | block 1\n\nOriginal full paper.";

test("MD download selects summaries and preserves document authorization", async (t) => {
  const directory = await mkdtemp(path.join(tmpdir(), "summary-download-"));
  const originalFetch = globalThis.fetch;
  const previous = process.env.PDF_PROCESSED_DIR;
  process.env.PDF_PROCESSED_DIR = directory;
  let detail: Record<string, unknown> = {};
  let status = 200;
  globalThis.fetch = async (_url, options) => {
    assert.equal(new Headers(options?.headers).get("authorization"), "Bearer fixture");
    return Response.json(detail, { status });
  };
  const download = (id = "1", authorization = "Bearer fixture") => GET(
    new Request("http://localhost/api/documents/1/download/md", { headers: { authorization } }),
    { params: Promise.resolve({ id, format: "md" }) }
  );
  try {
    await t.test("generated summary wins over historical full-text database content", async () => {
      detail = { filename: "Paper.md", source_filename: "Paper.pdf", source_storage_name: storage, content: fullText };
      await writeFile(path.join(directory, storage.replace(/\.pdf$/, ".md")), summary);
      await mkdir(path.join(directory, "pdf-text"));
      await writeFile(path.join(directory, "pdf-text/full.md"), fullText);
      const response = await download();
      assert.equal(response.status, 200);
      assert.equal(await response.text(), summary);
      assert.match(response.headers.get("content-disposition")!, /Paper.md/);
      assert.equal(response.headers.get("content-length"), String(Buffer.byteLength(summary)));
    });
    await t.test("download name shares the source PDF stem and sanitizes punctuation", async () => {
      detail.source_filename = "REPS: Recycled.pdf";
      const response = await download();
      assert.match(response.headers.get("content-disposition")!, /filename\*=UTF-8''REPS_%20Recycled.md/);
      detail.source_filename = "Paper.pdf";
    });
    await t.test("missing artifact can use the indexed summary", async () => {
      await rm(path.join(directory, storage.replace(/\.pdf$/, ".md")));
      detail.content = summary;
      assert.equal(await (await download()).text(), summary);
    });
    await t.test("missing summaries never fall back to full source text", async () => {
      detail.content = fullText;
      assert.equal((await download()).status, 409);
      await writeFile(path.join(directory, storage.replace(/\.pdf$/, ".md")), fullText);
      assert.equal((await (await download()).json()).code, "SUMMARY_NOT_AVAILABLE");
    });
    await t.test("full-text page blocks cannot masquerade as a summary", async () => {
      detail.content = summary + "\n" + fullText;
      assert.equal((await download()).status, 409);
    });
    await t.test("storage traversal does not read files outside processed", async () => {
      detail.source_storage_name = "../Paper.pdf";
      detail.content = fullText;
      assert.equal((await download()).status, 409);
    });
    await t.test("curated legacy summary headings remain downloadable without rewriting prose", async () => {
      const legacy = "# Paper\n\n## Summary\n\nSummary.\n\n## Key Takeaways\n\nFinding.\n\n## Scope and Positioning\n\nScope.\n";
      detail = { filename: "Paper.md", source_filename: "Paper.pdf", content: legacy };
      assert.equal(await (await download()).text(), legacy);
      detail.content = legacy + "\n" + fullText;
      assert.equal((await download()).status, 409);
    });
    await t.test("legacy Markdown without a PDF remains downloadable", async () => {
      detail = { filename: "notes.md", content: "# Notes" };
      assert.equal(await (await download()).text(), "# Notes");
    });
    await t.test("backend access denial precedes artifact reads", async () => {
      status = 404;
      detail = { error: "Document not found" };
      assert.equal((await download()).status, 404);
      assert.equal((await download("1", "")).status, 401);
      assert.equal((await download("../1")).status, 400);
    });
  } finally {
    globalThis.fetch = originalFetch;
    if (previous === undefined) delete process.env.PDF_PROCESSED_DIR;
    else process.env.PDF_PROCESSED_DIR = previous;
    await rm(directory, { recursive: true, force: true });
  }
});
