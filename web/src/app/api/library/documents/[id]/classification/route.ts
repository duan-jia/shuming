import { backendUrl, writeClassification, readMetadataJson } from "../../../../../../lib/paper-library-store.ts";
import { parseClassification } from "../../../../../../lib/paper-library.ts";

export const runtime = "nodejs";

export async function PUT(request: Request, { params }: { params: Promise<{ id: string }> }) {
  const authorization = request.headers.get("authorization") || "";
  if (!authorization.startsWith("Bearer ")) return Response.json({ error: "请先登录。" }, { status: 401 });
  const { id } = await params;
  if (!/^[1-9]\d*$/.test(id) || !Number.isSafeInteger(Number(id))) return Response.json({ error: "无效文档编号。" }, { status: 400 });
  try {
    const response = await fetch(backendUrl(`/api/documents/${id}`), { headers: { Authorization: authorization }, cache: "no-store", signal: AbortSignal.timeout(30_000) });
    const document = await response.json();
    if (!response.ok) return Response.json(document, { status: response.status });
    let classification;
    try {
      const body = await request.json();
      classification = body && typeof body === "object" && !Array.isArray(body) ? parseClassification(body) : null;
    } catch { classification = null; }
    if (!classification) return Response.json({ error: "请填写有效的年份、文献类型及会议或期刊名称（最多 200 字）。" }, { status: 400 });
    await writeClassification(Number(id), { ...await readMetadataJson(document), ...classification });
    return Response.json(classification);
  } catch {
    return Response.json({ error: "保存分类失败，请稍后重试。" }, { status: 502 });
  }
}
