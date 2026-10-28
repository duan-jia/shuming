import { authFetch as fetch } from "./auth-client";

export interface SourceFile {
  doc_id: number;
  filename: string;
}

// Treat SSE and stored JSON as untrusted. Never accept a URL from the model.
export function normalizeSourceFiles(value: unknown): SourceFile[] {
  if (!Array.isArray(value)) return [];
  const seen = new Set<number>();
  const files: SourceFile[] = [];
  for (const item of value) {
    if (!item || typeof item !== "object") continue;
    const { doc_id, filename } = item;
    if (!Number.isSafeInteger(doc_id) || doc_id < 1 || doc_id > 2147483647
      || typeof filename !== "string" || !filename.trim() || seen.has(doc_id)) continue;
    seen.add(doc_id);
    files.push({ doc_id, filename: filename.slice(0, 512) });
    if (files.length === 40) break;
  }
  return files;
}

export async function downloadDocumentFile(id: number, filename: string, format: "pdf" | "md", token: string): Promise<void> {
  if (!token || !Number.isSafeInteger(id) || id < 1) throw new Error("登录状态无效，请重新登录。");
  const response = await fetch(`/api/documents/${id}/download/${format}`, {
    headers: { Authorization: `Bearer ${token}` },
    cache: "no-store",
  });
  if (!response.ok) {
    const data = await response.json().catch(() => ({}));
    throw new Error(response.status === 403 || response.status === 404
      ? "文件已删除、不可访问或没有源 PDF。" : data.error || "下载失败，请稍后重试。");
  }
  const url = URL.createObjectURL(await response.blob());
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  // Some browsers consume the URL after the click handler returns.
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}
