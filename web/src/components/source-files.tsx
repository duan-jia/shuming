"use client";

import { useState } from "react";
import { downloadDocumentFile, type SourceFile } from "../lib/source-files";

export function SourceFiles({ files, token, label = "来源文件" }: { files: SourceFile[]; token: string; label?: string }) {
  const [expanded, setExpanded] = useState(false);
  const [pending, setPending] = useState<number | null>(null);
  const [error, setError] = useState("");
  if (!files.length) return null;

  async function download(file: SourceFile): Promise<void> {
    setPending(file.doc_id);
    setError("");
    try {
      await downloadDocumentFile(file.doc_id, file.filename, "pdf", token);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "下载失败，请稍后重试。");
    } finally {
      setPending(null);
    }
  }

  return <div className="source-files" aria-label={label === "附件" ? "消息附件" : "检索来源文件"}>
    <div className="source-files-row">
      <span className="source-files-label">{label}</span>
      {(expanded ? files : files.slice(0, 2)).map(file => <button
        key={file.doc_id} type="button" className="source-file-link"
        title={file.filename} aria-label={`下载 ${file.filename}`}
        disabled={pending !== null} onClick={() => void download(file)}
      >
        <span className="source-file-name">{file.filename}</span>
        <span aria-hidden="true">{pending === file.doc_id ? "…" : "↓"}</span>
      </button>)}
      {files.length > 2 && <button type="button" className="source-files-toggle"
        aria-expanded={expanded} onClick={() => setExpanded(value => !value)}>
        {expanded ? "收起" : `展开其余 ${files.length - 2} 个`}
      </button>}
    </div>
    {error && <p className="source-files-error" role="status">{error}</p>}
  </div>;
}
