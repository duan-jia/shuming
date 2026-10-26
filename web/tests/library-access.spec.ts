import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, writeFile, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { POST } from "../src/app/api/papers/upload/route.ts";
import { DELETE } from "../src/app/api/documents/[id]/route.ts";
import config from "../next.config.ts";

function uploadRequest(visibility: string) {
  const form = new FormData();
  form.set("visibility", visibility);
  form.set("file", new File(["%PDF-test-only"], "probe.pdf", { type: "application/pdf" }));
  return new Request("http://localhost/api/papers/upload", {
    method: "POST", headers: { Authorization: "Bearer fixture-only" }, body: form,
  });
}

test("public upload rejects before file storage or PDF processing and preserves target scope", async () => {
  const original = globalThis.fetch;
  let preflight = false;
  globalThis.fetch = async (_url, options) => {
    if (options?.method !== "POST") return Response.json({ documents: [] });
    assert.equal(JSON.parse(String(options.body)).visibility, "public");
    preflight = true;
    return Response.json({ error: "Only administrators can upload to the public library" }, { status: 403 });
  };
  try {
    const response = await POST(uploadRequest("public"));
    assert.equal(response.status, 403);
    assert.equal(preflight, true);
  } finally { globalThis.fetch = original; }
});

test("group upload keeps group scope during duplicate preflight", async () => {
  const original = globalThis.fetch;
  globalThis.fetch = async (_url, options) => {
    if (options?.method !== "POST") return Response.json({ documents: [] });
    assert.equal(JSON.parse(String(options.body)).visibility, "group");
    return Response.json({ duplicate: true, id: 1, filename: "existing.md", status: "ready" });
  };
  try { assert.equal((await POST(uploadRequest("group"))).status, 409); }
  finally { globalThis.fetch = original; }
});

test("invalid upload scope is rejected", async () => {
  const original = globalThis.fetch;
  globalThis.fetch = async () => Response.json({ documents: [] });
  try { assert.equal((await POST(uploadRequest("invalid"))).status, 400); }
  finally { globalThis.fetch = original; }
});

for (const allowCleanup of [false, true]) {
  test(`document deletion ${allowCleanup ? "removes unreferenced" : "preserves shared"} source files`, async () => {
    const directory = await mkdtemp(path.join(tmpdir(), "library-delete-"));
    const storage = "00000000-0000-0000-0000-000000000001.pdf";
    const pdf = path.join(directory, storage);
    await writeFile(pdf, "%PDF-fixture");
    const oldPdf = process.env.PDF_SOURCE_DIR, oldProcessed = process.env.PDF_PROCESSED_DIR;
    const original = globalThis.fetch;
    process.env.PDF_SOURCE_DIR = directory; process.env.PDF_PROCESSED_DIR = directory;
    globalThis.fetch = async (_url, options) => options?.method === "DELETE"
      ? Response.json({ status: "deleted", source_cleanup_allowed: allowCleanup })
      : Response.json({ source_storage_name: storage });
    try {
      const response = await DELETE(new Request("http://localhost/api/documents/1", {
        method: "DELETE", headers: { Authorization: "Bearer fixture-only" },
      }), { params: Promise.resolve({ id: "1" }) });
      assert.equal(response.status, 200);
      assert.equal((await response.json()).cleanup_warning, null);
      if (allowCleanup) await assert.rejects(readFile(pdf), { code: "ENOENT" });
      else assert.equal(await readFile(pdf, "utf8"), "%PDF-fixture");
    } finally {
      globalThis.fetch = original;
      if (oldPdf === undefined) delete process.env.PDF_SOURCE_DIR; else process.env.PDF_SOURCE_DIR = oldPdf;
      if (oldProcessed === undefined) delete process.env.PDF_PROCESSED_DIR; else process.env.PDF_PROCESSED_DIR = oldProcessed;
      await rm(directory, { recursive: true, force: true });
    }
  });
}

test("action routes precede the dynamic document route", async () => {
  const rewrites = await config.rewrites!();
  assert.ok(!Array.isArray(rewrites));
  const paths = rewrites.beforeFiles?.map(route => route.source);
  assert.ok(paths?.includes("/api/documents/search"));
  assert.ok(paths?.includes("/api/documents/deduplicate"));
});
