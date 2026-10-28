"use client";

import { useEffect, useRef, useState } from "react";

function SettingsIcon({ type }: { type: "settings" | "password" | "users" | "logout" }) {
  return <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
    {type === "settings" ? <><path d="m9 3-.6 2.3-2 .9-2.2-.7-1.7 3 1.6 1.7v2.4l-1.6 1.7 1.7 3 2.2-.7 2 .9L9 20h3.5l.6-2.5 2-.9 2.2.7 1.7-3-1.6-1.7v-2.4L19 8.5l-1.7-3-2.2.7-2-.9L12.5 3Z" /><circle cx="10.8" cy="11.5" r="3" /></>
      : type === "password" ? <><rect x="5" y="10" width="14" height="11" rx="2" /><path d="M8 10V7a4 4 0 0 1 8 0v3M12 14v3" /></>
      : type === "users" ? <><circle cx="9" cy="7" r="3" /><path d="M3 21v-2a6 6 0 0 1 12 0v2M16 4a3 3 0 0 1 0 6M21 21v-2a6 6 0 0 0-4-5.7" /></>
      : <><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M13 8l4 4-4 4M8 12h13" /></>}
  </svg>;
}

export function SettingsMenu({ role, onNavigate, onLogout }: {
  role: "admin" | "user" | "guest";
  onNavigate: (view: "password" | "quotas") => void;
  onLogout: () => Promise<void>;
}) {
  const details = useRef<HTMLDetailsElement>(null);
  const [pending, setPending] = useState(false);
  useEffect(() => {
    function outside(event: PointerEvent) {
      if (details.current && !details.current.contains(event.target as Node)) details.current.open = false;
    }
    function escape(event: KeyboardEvent) {
      if (event.key === "Escape" && details.current?.open) {
        details.current.open = false;
        details.current.querySelector("summary")?.focus();
      }
    }
    document.addEventListener("pointerdown", outside);
    document.addEventListener("keydown", escape);
    return () => { document.removeEventListener("pointerdown", outside); document.removeEventListener("keydown", escape); };
  }, []);
  function navigate(view: "password" | "quotas") {
    if (details.current) details.current.open = false;
    onNavigate(view);
  }
  return <details ref={details} className="account-settings">
    <summary aria-label="设置" aria-disabled={pending} onClick={event => { if (pending) event.preventDefault(); }}>
      <SettingsIcon type="settings" /><span>{pending ? "正在退出…" : "设置"}</span>
      <svg className="settings-chevron" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="m6 15 6-6 6 6" /></svg>
    </summary>
    <nav className="account-settings-menu" aria-label="账户设置">
      {role !== "guest" && <button type="button" disabled={pending} onClick={() => navigate("password")}><SettingsIcon type="password" /><span>修改密码</span></button>}
      {role === "admin" && <button type="button" disabled={pending} onClick={() => navigate("quotas")}><SettingsIcon type="users" /><span>用户管理</span></button>}
      {role !== "guest" && <div className="settings-divider" />}
      <button type="button" className="settings-logout" disabled={pending} onClick={async () => {
        if (details.current) details.current.open = false;
        setPending(true);
        try { await onLogout(); } finally { setPending(false); }
      }}><SettingsIcon type="logout" /><span>退出登录</span></button>
    </nav>
  </details>;
}
