// Explicit streaming proxy: never buffer the upstream response with text()/json().
export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function POST(request: Request): Promise<Response> {
  const authorization = request.headers.get("authorization");
  if (!authorization?.startsWith("Bearer ")) {
    return Response.json({ error: "Unauthorized" }, { status: 401 });
  }
  const base = (process.env.BACKEND_API_BASE || "http://127.0.0.1:9006").replace(/\/$/, "");
  try {
    const upstream = await fetch(`${base}/api/chat/stream`, {
      method: "POST",
      headers: {
        Authorization: authorization,
        "Content-Type": "application/json",
        Accept: "text/event-stream",
        "X-Request-Id": request.headers.get("x-request-id") || "",
      },
      body: await request.text(),
      signal: request.signal,
      cache: "no-store",
    });
    return new Response(upstream.body, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("content-type") || "text/event-stream; charset=utf-8",
        "Cache-Control": "no-cache, no-transform",
        "X-Accel-Buffering": "no",
      },
    });
  } catch {
    return Response.json({ error: "无法连接对话服务，请稍后重试。" }, { status: 502 });
  }
}
