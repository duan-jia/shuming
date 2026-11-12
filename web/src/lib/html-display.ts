export interface HtmlDisplay {
  type: "html_display";
  title: string;
  html: string;
  summary: string;
}

export function normalizeDisplays(value: unknown): HtmlDisplay[] {
  if (!Array.isArray(value)) return [];
  return value.slice(0, 4).filter((item): item is HtmlDisplay => {
    if (!item || typeof item !== "object") return false;
    const block = item as Record<string, unknown>;
    return block.type === "html_display" &&
      (["title", "html", "summary"] as const).every(key => {
        const text = block[key];
        return typeof text === "string" && text.trim().length > 0 &&
          new TextEncoder().encode(text).length <= (key === "html" ? 12000 : key === "title" ? 120 : 2000);
      });
  });
}

// This policy belongs to the trusted wrapper, never to generated content.
export const DISPLAY_CSP = "default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; img-src 'none'; font-src 'none'; connect-src 'none'; media-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'";
