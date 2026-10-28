export interface AuthUser {
  id: number;
  username: string;
  role: "admin" | "user" | "guest";
  tenant_id: number;
  must_change_password?: boolean;
}
export interface AuthState { token: string; user: AuthUser }
let current: AuthState | null = null;
let renewal: Promise<AuthState | null> | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;
let generation = 0;
let channel: BroadcastChannel | undefined;
const listeners = new Set<(value: AuthState | null) => void>();

function times(token: string) {
  try {
    const claims = JSON.parse(atob(token.split(".")[1].replace(/-/g, "+").replace(/_/g, "/")));
    const lifetime = Number(claims.exp) - Number(claims.iat);
    return { expiry: Number(claims.exp) * 1000, margin: Math.min(30, Math.max(1, lifetime / 5)) * 1000 };
  } catch { return { expiry: 0, margin: 0 }; }
}

export function acceptAuth(value: AuthState | null, broadcast = true) {
  const previous = current;
  if (!value) {
    ++generation;
    if (current && current.user.role !== "guest" && broadcast) channel?.postMessage({ event: "logout", user_id: current.user.id });
  }
  if (current && value && current.user.id !== value.user.id)
    for (const listener of listeners) listener(null);
  current = value;
  clearTimeout(timer);
  // An anonymous bootstrap response must not clear a password the user has
  // already started typing into the login form.
  if (!previous && !value) return;
  if (value) {
    const { expiry, margin } = times(value.token);
    if (value.user.role !== "guest") timer = setTimeout(() => {
      void refreshAuth().catch(() => {
        // A temporary network outage must not discard a renewable login.
        timer = setTimeout(() => { void refreshAuth().catch(() => {}); }, 10_000);
      });
    }, Math.max(500, expiry - margin - Date.now()));
  }
  for (const listener of listeners) listener(value);
}

function resumeAuth() {
  if (!current || current.user.role === "guest" || document.visibilityState === "hidden") return;
  const { expiry, margin } = times(current.token);
  if (expiry - Date.now() <= margin) void refreshAuth().catch(() => {});
}

export function subscribeAuth(listener: (value: AuthState | null) => void) {
  listeners.add(listener);
  if (listeners.size === 1 && typeof window !== "undefined") {
    if (typeof BroadcastChannel !== "undefined") {
      channel = new BroadcastChannel("emos-auth");
      channel.onmessage = message => {
        if (message.data?.event === "logout" && current?.user.role !== "guest" && current?.user.id === message.data.user_id)
          acceptAuth(null, false);
      };
    }
    window.addEventListener("focus", resumeAuth);
    document.addEventListener("visibilitychange", resumeAuth);
  }
  return () => {
    listeners.delete(listener);
    if (!listeners.size && typeof window !== "undefined") {
      channel?.close(); channel = undefined;
      window.removeEventListener("focus", resumeAuth);
      document.removeEventListener("visibilitychange", resumeAuth);
    }
  };
}

export function refreshAuth(): Promise<AuthState | null> {
  if (renewal) return renewal;
  const started = generation;
  const run = async () => {
    if (started !== generation) return current;
    const response = await globalThis.fetch("/api/auth/refresh", { method: "POST", cache: "no-store" });
    if (started !== generation) return current;
    if (response.status === 401) { acceptAuth(null); return null; }
    if (!response.ok) throw new Error("登录续期暂不可用，请稍后重试。");
    const value: AuthState = await response.json();
    if (started !== generation) return current;
    acceptAuth(value);
    return value;
  };
  // Serialize tabs as well as requests within this page; each rotation reads the
  // newest HttpOnly cookie. No password/token is persisted in browser storage.
  renewal = (async () => {
    if (typeof navigator !== "undefined" && navigator.locks)
      return await navigator.locks.request("emos-auth-refresh", run);
    return await run();
  })().finally(() => { renewal = null; });
  return renewal;
}

export async function logoutAuth() {
  if (renewal) await renewal.catch(() => {});
  const run = async () => {
    const headers = current ? { Authorization: `Bearer ${current.token}` } : undefined;
    const response = await globalThis.fetch("/api/auth/logout", { method: "POST", headers });
    if (!response.ok) throw new Error("退出登录失败，请稍后重试。");
    acceptAuth(null);
  };
  if (typeof navigator !== "undefined" && navigator.locks) await navigator.locks.request("emos-auth-refresh", run);
  else await run();
}

export async function authFetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
  const url = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
  const anonymous = /\/api\/auth\/(login|guest|config|refresh|logout)(?:\?|$)/.test(url);
  if (anonymous) {
    if (/\/api\/auth\/(login|guest)(?:\?|$)/.test(url) && renewal) await renewal.catch(() => {});
    if (/\/api\/auth\/(login|guest)(?:\?|$)/.test(url) && typeof navigator !== "undefined" && navigator.locks)
      return await navigator.locks.request("emos-auth-refresh", () => globalThis.fetch(input, init));
    return globalThis.fetch(input, init);
  }
  if (typeof window !== "undefined" && new URL(url, window.location.origin).origin !== window.location.origin)
    return globalThis.fetch(input, init);
  const auth = current;
  if (auth) {
    const { expiry, margin } = times(auth.token);
    // PDF upload includes a bounded 240s processing step and 60s registration.
    // Renew before beginning it so its original Authorization header remains
    // valid through the downstream registration call.
    const reserve = /\/api\/papers\/upload(?:\?|$)/.test(url) ? Math.max(margin, 330_000) : margin;
    if (expiry - Date.now() <= reserve) {
      if (auth.user.role === "guest") {
        if (expiry <= Date.now()) { acceptAuth(null); throw new Error("游客体验已结束，请重新登录。"); }
      } else if (!await refreshAuth()) throw new Error("登录已过期，请重新登录。");
    }
  }
  const send = () => {
    const headers = new Headers(init?.headers || (input instanceof Request ? input.headers : undefined));
    if (current) headers.set("Authorization", `Bearer ${current.token}`);
    return globalThis.fetch(input, { ...init, headers });
  };
  let response = await send();
  if (response.status === 401 && current?.user.role !== "guest" && current) {
    if (await refreshAuth()) response = await send();
  } else if (response.status === 401 && current?.user.role === "guest") acceptAuth(null);
  if (auth && (!current || current.user.id !== auth.user.id)) throw new Error("登录身份已变更，请重新操作。");
  return response;
}
