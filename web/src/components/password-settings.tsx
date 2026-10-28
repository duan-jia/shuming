"use client";

import { useState } from "react";
import { authFetch, acceptAuth } from "../lib/auth-client";

export function PasswordSettings({ onChanged }: { onChanged: () => void }) {
  const [pending, setPending] = useState(false);
  const [error, setError] = useState("");
  return <section className="max-w-lg" aria-label="修改自己的密码">
    <p className="mb-6 text-sm text-neutral-400">修改后，所有已登录设备需要使用新密码重新登录。</p>
    <form className="grid gap-4" onSubmit={async event => {
      event.preventDefault();
      const data = new FormData(event.currentTarget);
      const password = String(data.get("new_password") || "");
      if (Array.from(password).length < 8 || Array.from(password).length > 16) { setError("新密码需要 8 至 16 个字符。"); return; }
      if (password !== data.get("confirm_password")) { setError("两次输入的新密码不一致。"); return; }
      setPending(true); setError("");
      try {
        const response = await authFetch("/api/auth/password", { method: "POST", headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ current_password: data.get("current_password"), new_password: password }) });
        const body = await response.json();
        if (!response.ok) throw new Error(response.status === 403 ? "当前密码不正确。" : response.status === 429 ? "尝试过于频繁，请稍后重试。" : body.error || "修改失败，请重试。");
        acceptAuth(null);
        onChanged();
      } catch (err) { setError(err instanceof Error ? err.message : "修改密码失败。"); }
      finally { setPending(false); }
    }}>
      {[["当前密码", "current_password", "current-password"], ["新密码", "new_password", "new-password"], ["确认新密码", "confirm_password", "new-password"]].map(([label, name, complete]) =>
        <label key={name} className="grid gap-2 text-sm text-neutral-300">{label}
          <input className="rounded-lg border border-line bg-surface px-3 py-2.5 outline-none focus:border-neutral-500" name={name} type="password" autoComplete={complete} maxLength={1024} required disabled={pending} />
        </label>)}
      <p className="text-xs text-neutral-500">新密码需 8 至 16 个字符，不限制字符组合。</p>
      {error && <p role="alert" className="text-sm text-amber-300">{error}</p>}
      <button className="justify-self-start rounded-lg bg-foreground px-4 py-2.5 text-sm font-medium text-background disabled:opacity-50" disabled={pending} type="submit">{pending ? "修改中…" : "修改密码并重新登录"}</button>
    </form>
  </section>;
}
