"use client";

import { useCallback, useEffect, useState } from "react";
import { authFetch as fetch } from "../lib/auth-client";

interface QuotaUser {
  id: number;
  tenant_id: number;
  username: string;
  role: string;
  must_change_password: boolean;
  daily_token_quota: number | null;
  effective_daily_token_quota: number;
  today_token_limit: number;
  today_tokens_used: number;
  today_remaining_tokens: number;
}

const fieldClass = "min-w-0 rounded-lg border border-line bg-background px-3 py-2 text-sm text-foreground";

function CreateUserForm({ tenants, token, onCreated }: { tenants: {id: number; name: string}[]; token: string; onCreated: () => Promise<void> }) {
  const [open, setOpen] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState("");
  return <div className="mb-6 rounded-xl border border-line bg-surface p-4">
    <button type="button" className="text-sm font-medium" aria-expanded={open} onClick={() => { setOpen(!open); setError(""); }}>{open ? "收起创建表单" : "+ 创建用户"}</button>
    {open && <form className="mt-4 grid gap-4 sm:grid-cols-2" onSubmit={async event => {
      event.preventDefault(); const form = event.currentTarget; const data = new FormData(form);
      const password = String(data.get("password") || "");
      const raw = String(data.get("quota") || "").trim(); const quota = raw ? Number(raw) : null;
      if (Array.from(password).length < 8 || Array.from(password).length > 16) { setError("初始密码需要 8 至 16 个字符。"); return; }
      if (quota !== null && (!/^\d+$/.test(raw) || !Number.isSafeInteger(quota) || quota < 1)) { setError("请输入有效的每日额度。"); return; }
      setPending(true); setError("");
      try {
        const response = await fetch("/api/users", { method: "POST", headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" }, body: JSON.stringify({
          username: String(data.get("username") || "").trim(), password, tenant_id: Number(data.get("tenant")),
          role: data.get("role"), daily_token_quota: quota, must_change_password: false,
        }) });
        const body = await response.json();
        if (!response.ok) throw new Error(response.status === 409 ? "该组已有同名用户。" : body.error || "创建失败。");
        form.reset(); setOpen(false); await onCreated();
      } catch (err) { setError(err instanceof Error ? err.message : "创建失败。"); }
      finally { setPending(false); }
    }}>
      <label className="grid gap-2 text-xs text-neutral-400">用户名<input className={fieldClass} name="username" autoComplete="off" required maxLength={64} disabled={pending} /></label>
      <label className="grid gap-2 text-xs text-neutral-400">初始密码<input className={fieldClass} name="password" type="password" autoComplete="new-password" required disabled={pending} /></label>
      <label className="grid gap-2 text-xs text-neutral-400">所属组<select className={fieldClass} name="tenant" required disabled={pending || !tenants.length}>{tenants.map(tenant => <option key={tenant.id} value={tenant.id}>{tenant.name === "default" ? "RMDA group" : tenant.name}</option>)}</select></label>
      <label className="grid gap-2 text-xs text-neutral-400">角色<select className={fieldClass} name="role" disabled={pending}><option value="user">普通用户</option><option value="admin">管理员</option></select></label>
      <label className="grid gap-2 text-xs text-neutral-400">每日专属额度（留空继承）<input className={fieldClass} name="quota" inputMode="numeric" disabled={pending} /></label>
      <p className="self-center text-xs text-neutral-500">密码为 8 至 16 个字符，不限制字符组合。</p>
      {error && <p role="alert" className="text-sm text-amber-300 sm:col-span-2">{error}</p>}
      <button type="submit" disabled={pending || !tenants.length} className="justify-self-start rounded-lg bg-foreground px-4 py-2 text-sm text-background disabled:opacity-50">{pending ? "创建中…" : "确认创建"}</button>
    </form>}
  </div>;
}

function ResetPassword({ user, token, onReset }: { user: QuotaUser; token: string; onReset: () => Promise<void> }) {
  const [open, setOpen] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState("");
  return <div className="mt-4 border-t border-line pt-3">
    <button type="button" className="text-xs text-neutral-400" aria-expanded={open} onClick={() => setOpen(!open)}>重置密码</button>
    {open && <form className="mt-3 grid gap-3" onSubmit={async event => {
      event.preventDefault(); const form = event.currentTarget; const password = String(new FormData(form).get("password") || "");
      if (Array.from(password).length < 8 || Array.from(password).length > 16) { setError("密码需要 8 至 16 个字符。"); return; }
      setPending(true); setError("");
      try {
        const response = await fetch(`/api/users/${user.id}/password`, { method: "POST", headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" }, body: JSON.stringify({ password }) });
        const body = await response.json(); if (!response.ok) throw new Error(body.error || "重置失败。");
        form.reset(); setOpen(false); await onReset();
      } catch (err) { setError(err instanceof Error ? err.message : "重置失败。"); }
      finally { setPending(false); }
    }}>
      <label className="grid gap-2 text-xs text-neutral-400">{user.username} 的新密码<input className={fieldClass} name="password" type="password" autoComplete="new-password" required disabled={pending} /></label>
      <p className="text-xs text-neutral-500">会使该用户所有旧登录凭证失效，之后可使用新密码登录。</p>
      {error && <p role="alert" className="text-sm text-amber-300">{error}</p>}
      <button type="submit" disabled={pending} className="justify-self-start rounded-lg border border-line px-3 py-2 text-xs disabled:opacity-50">{pending ? "重置中…" : "确认重置"}</button>
    </form>}
  </div>;
}

function QuotaForm({ user, save }: { user: QuotaUser; save: (id: number, quota: number | null) => Promise<void> }) {
  const [inherit, setInherit] = useState(user.daily_token_quota === null);
  const [value, setValue] = useState(String(user.daily_token_quota ?? user.effective_daily_token_quota));
  const [pending, setPending] = useState(false);
  const [error, setError] = useState("");

  return (
    <form className="mt-4 flex flex-wrap items-end gap-3" onSubmit={async (event) => {
      event.preventDefault();
      const quota = inherit ? null : Number(value);
      if (quota !== null && (!/^\d+$/.test(value) || !Number.isSafeInteger(quota) || quota < 1)) {
        setError("请输入 1 至 9,007,199,254,740,991 之间的整数。"); return;
      }
      setPending(true); setError("");
      try { await save(user.id, quota); }
      catch (err) { setError(err instanceof Error ? err.message : "保存失败，请重试。"); }
      finally { setPending(false); }
    }}>
      <label className="flex flex-col gap-2 text-xs text-neutral-400">
        额度来源
        <select aria-label={`${user.username} 的额度来源`} className="rounded-lg border border-line bg-background px-3 py-2 text-sm text-foreground"
          disabled={pending} value={inherit ? "tenant" : "user"} onChange={event => setInherit(event.target.value === "tenant")}>
          <option value="tenant">继承租户额度</option><option value="user">用户专属额度</option>
        </select>
      </label>
      {!inherit && <label className="flex min-w-0 flex-col gap-2 text-xs text-neutral-400">
        每日 token 上限
        <input aria-label={`${user.username} 的每日 token 上限`} className="w-full rounded-lg border border-line bg-background px-3 py-2 text-sm text-foreground"
          type="text" inputMode="numeric" required disabled={pending} value={value} onChange={event => setValue(event.target.value)} />
      </label>}
      <button type="submit" disabled={pending} className="rounded-lg bg-foreground px-4 py-2 text-sm font-medium text-background disabled:opacity-50">
        {pending ? "保存中…" : "保存额度"}
      </button>
      {error && <p role="alert" className="w-full text-sm text-amber-300">{error}</p>}
    </form>
  );
}

export function UserManagement({ token, currentUserId }: { token: string; currentUserId: number }) {
  const [tenants, setTenants] = useState<{id: number; name: string}[]>([]);
  const [showGuests, setShowGuests] = useState(false);
  const [users, setUsers] = useState<QuotaUser[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const load = useCallback(async (signal?: AbortSignal) => {
    const response = await fetch("/api/users", { headers: { Authorization: `Bearer ${token}` }, signal });
    if (!response.ok) throw new Error(response.status === 403 ? "仅管理员可以管理额度。" : "无法加载用户列表，请重试。");
    const data = await response.json();
    return data.users as QuotaUser[];
  }, [token]);

  useEffect(() => {
    const controller = new AbortController();
    load(controller.signal).then(users => {
      if (!controller.signal.aborted) setUsers(users);
    }).catch(err => {
      if (!controller.signal.aborted) setError(err.message);
    }).finally(() => { if (!controller.signal.aborted) setLoading(false); });
    fetch("/api/tenants", { headers: { Authorization: `Bearer ${token}` }, signal: controller.signal }).then(async response => {
      if (!response.ok) throw new Error("无法加载所属组。");
      return response.json();
    }).then(data => { if (!controller.signal.aborted) setTenants(data.tenants); }).catch(err => {
      if (!controller.signal.aborted) setError(err.message);
    });
    return () => controller.abort();
  }, [load, token]);

  async function save(id: number, quota: number | null) {
    setNotice("");
    const response = await fetch(`/api/users/${id}/quota`, {
      method: "POST", headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: JSON.stringify({ daily_token_quota: quota }),
    });
    if (!response.ok) throw new Error(response.status === 400 ? "额度无效，请检查输入。" : "保存失败，请刷新后重试。");
    setNotice("额度已保存，今天立即生效，已用量保留。以后每天沿用此设置。");
    try { setUsers(await load()); setError(""); }
    catch { setError("额度已保存，但列表刷新失败，请点击刷新。"); }
  }

  async function changed(message: string) {
    setNotice(message);
    try { setUsers(await load()); setError(""); } catch { setError("操作已完成，请刷新用户列表。"); }
  }

  return <section aria-label="用户管理">
    <CreateUserForm tenants={tenants} token={token} onCreated={() => changed("用户已创建，可直接使用设置的密码登录。")} />
    <label className="mb-4 flex items-center gap-2 text-xs text-neutral-400"><input type="checkbox" checked={showGuests} onChange={event => setShowGuests(event.target.checked)} />显示游客账号</label>
    <div className="mb-4 flex items-center justify-between gap-3">
      <p className="text-sm text-neutral-400">专属额度每天沿用；选择继承后使用租户默认额度。修改会立即更新今天的上限，保留已用量。</p>
      <button type="button" disabled={loading} className="shrink-0 text-sm text-neutral-400 disabled:opacity-50" onClick={async () => {
        setLoading(true); setError("");
        try { setUsers(await load()); } catch (err) { setError(err instanceof Error ? err.message : "刷新失败。"); }
        finally { setLoading(false); }
      }}>刷新</button>
    </div>
    {error && <p role="alert" className="mb-4 text-sm text-amber-300">{error}</p>}
    {notice && <p role="status" className="mb-4 text-sm text-emerald-300">{notice}</p>}
    {loading && <p role="status" className="py-8 text-sm text-neutral-400">正在加载用户额度…</p>}
    <div className="grid gap-4 xl:grid-cols-2">
      {users.filter(user => showGuests || user.role !== "guest").map(user => <article key={user.id} className="min-w-0 rounded-xl border border-line bg-surface p-4">
        <h2 className="break-all font-medium">{user.username} <span className="text-xs text-neutral-500">{user.role} · 租户 {user.tenant_id}</span></h2>
        <dl className="mt-3 grid grid-cols-2 gap-3 text-xs sm:grid-cols-3">
          {[["默认每日额度", user.effective_daily_token_quota], ["今日上限", user.today_token_limit], ["今日已用", user.today_tokens_used], ["今日剩余", user.today_remaining_tokens]].map(([label, value]) =>
            <div key={label}><dt className="text-neutral-500">{label}</dt><dd className="mt-1 break-all text-neutral-300">{Number(value).toLocaleString("zh-CN")}</dd></div>)}
        </dl>
        <QuotaForm key={`${user.id}:${user.daily_token_quota}:${user.effective_daily_token_quota}`} user={user} save={save} />
        {user.role !== "guest" && user.id !== currentUserId && <ResetPassword user={user} token={token} onReset={() => changed("密码已重置，该用户的旧登录凭证已失效。")} />}
      </article>)}
    </div>
  </section>;
}
