import { backendUrl, readPaperMetadata } from "../../../../lib/paper-library-store.ts";

export const runtime = "nodejs";

export async function GET(request: Request) {
  const authorization = request.headers.get("authorization") || "";
  if (!authorization.startsWith("Bearer ")) return Response.json({ error: "请先登录。" }, { status: 401 });
  try {
    const response = await fetch(backendUrl("/api/documents"), { headers: { Authorization: authorization }, cache: "no-store", signal: AbortSignal.timeout(30_000) });
    const data = await response.json();
    if (!response.ok) return Response.json(data, { status: response.status });
    // Read artifacts only for documents returned by the backend's visibility checks.
    const documents = [];
    for (const document of data.documents || []) {
      const { source_storage_name: storage, ...summary } = document;
      documents.push({ ...summary, ...await readPaperMetadata({ id: document.id, source_storage_name: storage }) });
    }
    return Response.json({ documents }, { headers: { "Cache-Control": "no-store" } });
  } catch {
    return Response.json({ error: "无法加载论文元数据，请稍后重试。" }, { status: 502 });
  }
}
