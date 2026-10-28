import { cookies } from "next/headers";
import { NextResponse } from "next/server";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";
const COOKIE = "emos_refresh";
type Context = { params: Promise<{ action: string }> };

async function proxy(request: Request, context: Context) {
  const { action } = await context.params;
  const allowed = request.method === "GET" ? ["config", "me"] : ["login", "guest", "refresh", "logout", "password"];
  if (!allowed.includes(action)) return NextResponse.json({ error: "Method not allowed" }, { status: 405 });
  const origin = request.headers.get("origin");
  if (request.method === "POST") {
    try {
      if ((origin && new URL(origin).host !== request.headers.get("host"))
        || (!origin && request.headers.get("sec-fetch-site") === "cross-site"))
        return NextResponse.json({ error: "Cross-origin request rejected" }, { status: 403 });
    } catch { return NextResponse.json({ error: "Invalid origin" }, { status: 403 }); }
  }
  const base = (process.env.BACKEND_API_BASE || "http://127.0.0.1:9006").replace(/\/$/, "");
  const jar = await cookies();
  const refresh = jar.get(COOKIE)?.value || "";
  const secure = process.env.AUTH_COOKIE_SECURE === "true" || new URL(request.url).protocol === "https:"
    || request.headers.get("x-forwarded-proto") === "https";
  try {
    const headers: Record<string, string> = { "Content-Type": "application/json" };
    const authorization = request.headers.get("authorization");
    if (authorization) headers.Authorization = authorization;
    const body = request.method === "GET" ? undefined : ["refresh", "logout"].includes(action)
      ? JSON.stringify({ refresh_token: refresh }) : await request.text();
    const upstream = await fetch(`${base}/api/auth/${action}`, {
      method: request.method, headers, body, cache: "no-store", signal: AbortSignal.timeout(15_000),
    });
    const data = await upstream.json();
    if (upstream.ok && action === "guest" && refresh)
      await fetch(`${base}/api/auth/logout`, { method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ refresh_token: refresh }), signal: AbortSignal.timeout(5000) }).catch(() => {});
    const nextRefresh = typeof data.refresh_token === "string" ? data.refresh_token : "";
    delete data.refresh_token;
    const response = NextResponse.json(data, { status: upstream.status, headers: { "Cache-Control": "no-store" } });
    const options = { httpOnly: true, secure, sameSite: "strict" as const, path: "/api/auth" };
    if (upstream.ok && nextRefresh) {
      response.cookies.set(COOKIE, nextRefresh, { ...options, maxAge: Number(data.refresh_expires_in) });
      if (action === "login" && refresh) {
        // Replacing an existing browser login should not leave an orphan session.
        await fetch(`${base}/api/auth/logout`, { method: "POST", headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ refresh_token: refresh }), signal: AbortSignal.timeout(5000) }).catch(() => {});
      }
    } else if ((upstream.ok && ["logout", "password", "guest"].includes(action))
      || (action === "refresh" && upstream.status === 401)) {
      response.cookies.set(COOKIE, "", { ...options, maxAge: 0 });
    }
    return response;
  } catch { return NextResponse.json({ error: "认证服务暂不可用，请稍后重试。" }, { status: 503 }); }
}

export const GET = proxy;
export const POST = proxy;
