import { backendUrl, readMetadataJson, writeClassification } from "../../../../../../lib/paper-library-store.ts";
import { paperMetadata, validYear } from "../../../../../../lib/paper-library.ts";
export const runtime = "nodejs";
async function handle(request: Request, { params }: { params: Promise<{ id: string }> }) {
  const authorization = request.headers.get("authorization") || "";
  if (!authorization.startsWith("Bearer ")) return Response.json({ error: "请先登录。" }, { status: 401 });
  const { id } = await params;
  if (!/^[1-9]\d*$/.test(id) || !Number.isSafeInteger(Number(id))) return Response.json({ error: "文档编号无效。" }, { status: 400 });
  try {
    const response = await fetch(backendUrl(`/api/documents/${id}`), { headers: { Authorization: authorization }, cache: "no-store", signal: AbortSignal.timeout(30000) });
    const document = await response.json();
    if (!response.ok) return Response.json(document, { status: response.status });
    const current = await readMetadataJson({ ...document, id: Number(id) });
    if (request.method === "GET") return Response.json(current, { headers: { "Cache-Control": "no-store" } });
    let value;
    try { value = JSON.parse(await request.text()); } catch { return Response.json({ error: "JSON 格式无效。" }, { status: 400 }); }
    if (!value || Array.isArray(value) || typeof value !== "object" || JSON.stringify(value).length > 200000 || (value.publication_year !== null && !validYear(value.publication_year)) || typeof value.publication_venue !== "string" || value.publication_venue.length > 200) return Response.json({ error: "元数据须为 JSON 对象，年份及会议/期刊名称须有效。" }, { status: 400 });
    // Source identity and processing provenance belong to ingestion, not editable bibliographic metadata.
    for (const key of ["source_pdf", "source_storage_name", "source_sha256", "schema_version", "status", "model", "generated_at", "page_count", "extracted_chars", "needs_review", "review_reason"]) {
      if (key in current) value[key] = current[key]; else delete value[key];
    }
    const result = { ...value, ...paperMetadata(value) };
    await writeClassification(Number(id), result);
    return Response.json(result);
  } catch { return Response.json({ error: "元数据服务暂时不可用。" }, { status: 502 }); }
}
export const GET = handle;
export const PUT = handle;
