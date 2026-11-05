"use client";
import { useState } from "react";
import { authFetch } from "../lib/auth-client";
import type { PaperClassification } from "../lib/paper-library";
export function PaperClassificationEditor({ document, token, onSaved }: {
  document: Partial<PaperClassification> & { id: number; can_delete: boolean };
  token: string; onSaved: () => Promise<void>;
}) {
  const [editing, setEditing] = useState(false);
  const [json, setJson] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const url = `/api/library/documents/${document.id}/metadata`;
  return <div className="mt-2 text-xs text-neutral-400">
    <p>{document.publication_year || "年份未知"} · {document.publication_venue || "未分类"}</p>
    {!editing && <button type="button" disabled={busy} className="mt-1 text-neutral-300" onClick={async () => {
      setBusy(true); setError("");
      try { const response = await authFetch(url, { headers: { Authorization: `Bearer ${token}` } }); const data = await response.json(); if (!response.ok) throw new Error(data.error); setJson(JSON.stringify(data, null, 2)); setEditing(true); }
      catch (error) { setError(error instanceof Error ? error.message : "读取失败。"); } finally { setBusy(false); }
    }}>编辑元数据 JSON</button>}
    {editing && <form className="mt-2 space-y-2" onSubmit={async event => {
      event.preventDefault(); setError(""); setBusy(true);
      try { JSON.parse(json); const response = await authFetch(url, { method: "PUT", headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" }, body: json }); const data = await response.json(); if (!response.ok) throw new Error(data.error); await onSaved(); setEditing(false); }
      catch (error) { setError(error instanceof Error ? error.message : "保存失败。"); } finally { setBusy(false); }
    }}>
      <textarea aria-label="元数据 JSON" value={json} onChange={event => setJson(event.target.value)} disabled={busy} spellCheck={false} rows={18} className="w-full rounded border border-line bg-background p-2 font-mono text-xs" />
      <div className="flex gap-3"><button type="submit" disabled={busy}>{busy ? "保存中…" : "保存"}</button><button type="button" disabled={busy} onClick={() => setEditing(false)}>取消</button></div>
    </form>}
    {error && <p role="alert" className="text-amber-300">{error}</p>}
  </div>;
}
