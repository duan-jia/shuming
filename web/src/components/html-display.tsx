"use client";

import { useMemo, useState, useSyncExternalStore } from "react";
import DOMPurify from "dompurify";
import { DISPLAY_CSP, type HtmlDisplay } from "../lib/html-display";

function displayDocument(html: string): string {
  if (!DOMPurify.isSupported) throw new Error("HTML filtering unavailable");
  const clean = DOMPurify.sanitize(html, {
    // Deliberately exclude navigation, active content, SVG/MathML, media and
    // embedded documents. No URL-bearing attributes (including href/src).
    ALLOWED_TAGS: ["p", "br", "hr", "div", "span", "section", "article", "header",
      "h1", "h2", "h3", "h4", "strong", "em", "b", "i", "s", "small", "sub", "sup",
      "ul", "ol", "li", "dl", "dt", "dd", "blockquote", "pre", "code",
      "table", "caption", "thead", "tbody", "tfoot", "tr", "th", "td"],
    ALLOWED_ATTR: ["style", "colspan", "rowspan", "scope", "aria-label"],
    ALLOW_DATA_ATTR: false,
    ALLOW_ARIA_ATTR: false,
  });
  if (!clean.trim()) throw new Error("Empty display after filtering");
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="${DISPLAY_CSP}"><meta name="referrer" content="no-referrer"><meta name="viewport" content="width=device-width,initial-scale=1"><style>html{color-scheme:light}body{margin:0;padding:16px;font:14px/1.6 system-ui,"Noto Sans CJK SC","Microsoft YaHei","PingFang SC",sans-serif;color:#172033;background:#fff;overflow-wrap:anywhere}*{box-sizing:border-box}table{border-collapse:collapse;max-width:100%}th,td{border:1px solid #d9dfe8;padding:8px;text-align:left}pre{white-space:pre-wrap}h1,h2,h3{line-height:1.3}</style></head><body>${clean}</body></html>`;
}

const subscribe = (): (() => void) => () => {};
const clientSnapshot = (): boolean => true;
const serverSnapshot = (): boolean => false;

export function HtmlDisplayCard({ block }: { block: HtmlDisplay }) {
  // DOMPurify needs a browser DOM. Keep SSR/hydration text-only.
  const hydrated = useSyncExternalStore(subscribe, clientSnapshot, serverSnapshot);
  const [loadFailed, setLoadFailed] = useState(false);
  const document = useMemo(() => {
    if (!hydrated) return "";
    try { return displayDocument(block.html); } catch { return ""; }
  }, [hydrated, block.html]);
  const failed = loadFailed || (hydrated && !document);

  return <section className="html-display" aria-label={block.title}>
    <header><strong>{block.title}</strong><span>静态富内容 · 隔离展示</span></header>
    <p className="html-display-summary">{block.summary}</p>
    {failed ? <p role="status">富内容无法显示，请参考以上文字摘要。</p> : document ?
      <iframe title={block.title} srcDoc={document} sandbox="" referrerPolicy="no-referrer"
        allow="camera 'none'; microphone 'none'; geolocation 'none'; clipboard-read 'none'; clipboard-write 'none'"
        onError={() => setLoadFailed(true)} /> : <p role="status">正在准备富内容…</p>}
    <details><summary>展示限制</summary><p>仅支持静态内容，不运行脚本，不加载外部资源。长内容可在框内滚动。</p></details>
  </section>;
}
