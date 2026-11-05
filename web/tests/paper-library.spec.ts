import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, writeFile, mkdir, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { paperMetadata, parseClassification, normalizeVenue } from "../src/lib/paper-library.ts";
import { GET } from "../src/app/api/library/documents/route.ts";
import { PUT } from "../src/app/api/library/documents/[id]/classification/route.ts";

test("classification reads governed publication fields and supports legacy metadata", () => {
  const paper = paperMetadata({ title: "Fast Networks", authors: ["Alice"], keywords: ["RDMA"], problem_and_context: "Congestion control", venue_and_year: "International Conference on Networks, 2025" });
  assert.equal(normalizeVenue("ACM SIGCOMM (SIGCOMM '26)"), "ACM SIGCOMM");
  assert.equal(normalizeVenue("ACM SIGCOMM"), "ACM SIGCOMM");
  assert.equal(paper.publication_year, 2025);
  assert.equal(paper.publication_type, "conference");
  assert.equal(paper.publication_venue, "International Conference on Networks");
  assert.deepEqual(Object.keys(paper).sort(), ["publication_type", "publication_venue", "publication_year"]);
  assert.deepEqual(paperMetadata({ publication_year: 2026, publication_type: "conference", publication_venue: "NSDI", venue_and_year: "Outdated 2024" }), {
    publication_year: 2026, publication_type: "conference", publication_venue: "USENIX NSDI",
  });
  assert.equal(paperMetadata({ venue_and_year: "[VERIFY: venue and year unavailable 2024]" }).publication_year, null);
  assert.equal(paperMetadata({ venue_and_year: "2024 / 2025" }).publication_year, null);
  assert.equal(paperMetadata({ created_at: "2026-01-01" }).publication_year, null);
  assert.equal(paperMetadata({ venue_and_year: "ExampleConf 2025" }).publication_type, "unknown");
  for (const publication_year of [true, 0, 1799, 2025.5, "2025"]) assert.equal(parseClassification({ publication_year, publication_type: "conference", publication_venue: "NSDI" }), null);
  assert.equal(parseClassification({ publication_year: null, publication_type: "toString", publication_venue: "" }), null);
});

test("library respects backend visibility, validates classification and persists authorized edits", async () => {
  const directory = await mkdtemp(path.join(tmpdir(), "paper-library-"));
  const previous = process.env.PDF_PROCESSED_DIR;
  const originalFetch = globalThis.fetch;
  process.env.PDF_PROCESSED_DIR = directory;
  const storage = "00000000-0000-0000-0000-000000000001--Paper.pdf";
  let allowed = true;
  let status = 200;
  globalThis.fetch = async (url, options) => {
    assert.equal(new Headers(options?.headers).get("authorization"), "Bearer fixture");
    if (status !== 200) return Response.json({ error: "Access denied" }, { status });
    return Response.json(String(url).endsWith("/api/documents")
      ? { documents: [{ id: 1, filename: "Paper.md", source_storage_name: storage }, { id: 2, filename: "Legacy.md", source_storage_name: "../private.pdf" }] }
      : { id: 1, can_delete: allowed });
  };
  const request = () => new Request("http://localhost/api/library/documents", { headers: { Authorization: "Bearer fixture" } });
  const classification = { publication_year: 2025, publication_type: "journal", publication_venue: "Test Journal" };
  const update = (body: unknown = classification) => PUT(new Request("http://localhost/api/library/documents/1/classification", { method: "PUT", headers: { Authorization: "Bearer fixture", "Content-Type": "application/json" }, body: JSON.stringify(body) }), { params: Promise.resolve({ id: "1" }) });
  try {
    await writeFile(path.join(directory, storage.replace(/\.pdf$/, ".json")), JSON.stringify({ title: "REPS:  Recycled\nPackets", venue_and_year: "Conference on Systems 2024" }));
    await mkdir(path.join(directory, "library"));
    await writeFile(path.join(directory, "library/3.json"), JSON.stringify(classification));
    let documents = (await (await GET(request())).json()).documents;
    assert.deepEqual(documents.map((doc: { id: number }) => doc.id), [1, 2]);
    assert.equal(documents[0].publication_year, 2024);
    assert.equal(documents[1].publication_year, null);
    assert.equal(documents[0].source_storage_name, undefined);
    allowed = false;
    assert.equal((await update()).status, 200);
    allowed = true;
    assert.equal((await update({ ...classification, publication_type: "bogus" })).status, 400);
    assert.equal((await update(null)).status, 400);
    assert.equal((await update()).status, 200);
    documents = (await (await GET(request())).json()).documents;
    assert.equal(documents[0].publication_type, "journal");
    assert.equal(documents[0].publication_year, 2025);
    assert.equal(documents[0].title, "REPS: Recycled Packets");
    assert.equal(documents[1].title, null);
    await writeFile(path.join(directory, "library/1.json"), JSON.stringify({ ...classification, title: "REPS: Corrected Title" }));
    documents = (await (await GET(request())).json()).documents;
    assert.equal(documents[0].title, "REPS: Corrected Title");
    assert.equal(documents[0].filename, "Paper.md");
    assert.equal(documents[0].abstract, undefined);
    assert.equal(JSON.parse(await readFile(path.join(directory, "library/1.json"), "utf8")).publication_venue, "Test Journal");
    status = 404;
    assert.equal((await update()).status, 404);
    assert.equal((await GET(request())).status, 404);
    assert.equal((await GET(new Request("http://localhost/api/library/documents"))).status, 401);
  } finally {
    globalThis.fetch = originalFetch;
    if (previous === undefined) delete process.env.PDF_PROCESSED_DIR; else process.env.PDF_PROCESSED_DIR = previous;
    await rm(directory, { recursive: true, force: true });
  }
});
