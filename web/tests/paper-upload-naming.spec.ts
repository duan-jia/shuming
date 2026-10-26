import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, writeFile, readdir, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { POST } from "../src/app/api/papers/upload/route.ts";

for (const outcome of ["success", "submission-timeout", "backend-rejected"]) {
  test(`paper upload handles title-based artifacts after ${outcome}`, async () => {
    const root = await mkdtemp(path.join(tmpdir(), "paper-title-"));
    const keys = ["PROJECT_ROOT", "PDF_SOURCE_DIR", "PDF_PROCESSED_DIR", "PDF_PROCESSOR_PYTHON"];
    const saved = keys.map(key => process.env[key]);
    const originalFetch = globalThis.fetch;
    await mkdir(path.join(root, "scripts"));
    await writeFile(path.join(root, "scripts/pdf_to_markdown.py"), `
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--file');p.add_argument('--output');a=p.parse_args()
s=Path(a.file);o=Path(a.output);m=o/(s.stem+'.md');j=o/(s.stem+'.json')
m.write_text('# A Paper: 中文 / Methods');j.write_text('{}')
print(json.dumps(dict(status='ready',markdown_path=str(m),json_path=str(j),source_sha256=hashlib.sha256(s.read_bytes()).hexdigest(),title='A Paper: 中文 / Methods')))
  `);
    process.env.PROJECT_ROOT = root;
    process.env.PDF_SOURCE_DIR = path.join(root, "pdf");
    process.env.PDF_PROCESSED_DIR = path.join(root, "processed");
    process.env.PDF_PROCESSOR_PYTHON = "python3";
    let submitted: Record<string, unknown> | undefined;
    globalThis.fetch = async (url, options) => {
      if (options?.method !== "POST") return Response.json({ documents: [] });
      if (String(url).endsWith("/deduplicate")) return Response.json({ duplicate: false });
      submitted = JSON.parse(String(options.body));
      if (outcome === "submission-timeout") throw new TypeError("simulated connection reset after commit");
      if (outcome === "backend-rejected") return Response.json({ id: 7, status: "ready" }, { status: 409 });
      return Response.json({ id: 42, status: "processing" });
    };
    try {
      const form = new FormData();
      form.set("file", new File(["%PDF-fixture"], "2504.17307v2.pdf", { type: "application/pdf" }));
      const response = await POST(new Request("http://localhost/api/papers/upload", {
        method: "POST", headers: { Authorization: "Bearer test" }, body: form,
      }));
      assert.equal(response.status, outcome === "success" ? 200 : outcome === "backend-rejected" ? 409 : 502);
      if (outcome === "success") assert.equal((await response.json()).filename, "A Paper_ 中文 _ Methods.pdf");
      assert.equal(submitted?.filename, "A Paper_ 中文 _ Methods.md");
      assert.equal(submitted?.source_filename, "A Paper_ 中文 _ Methods.pdf");
      assert.match(String(submitted?.source_storage_name), /^[a-f0-9-]{36}--A Paper_ 中文 _ Methods\.pdf$/);
      const files = await readdir(path.join(root, "pdf"));
      if (outcome === "backend-rejected") {
        assert.deepEqual(files, []);
        assert.deepEqual(await readdir(path.join(root, "processed")), []);
        return;
      }
      assert.deepEqual(files, [submitted?.source_storage_name]);
      const metadata = JSON.parse(await readFile(path.join(root, "processed", String(files[0]).replace(/\.pdf$/, ".json")), "utf8"));
      assert.equal(metadata.source_pdf, submitted?.source_filename);
      assert.equal(metadata.source_storage_name, submitted?.source_storage_name);
    } finally {
      globalThis.fetch = originalFetch;
      keys.forEach((key, index) => {
        if (saved[index] === undefined) delete process.env[key]; else process.env[key] = saved[index];
      });
      await rm(root, { recursive: true, force: true });
    }
  });
}
