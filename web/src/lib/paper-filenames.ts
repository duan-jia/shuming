// One stem for PDF, Markdown and JSON; bibliographic titles stay untouched.
export const PAPER_STEM_MAX_BYTES = 160;

export function safePaperStem(value: string): string {
  const normalized = value.normalize("NFKC")
    .replace(/\s+/g, " ")
    .replace(/[<>:"/\\|?*\u0000-\u001f\u007f]/g, "_")
    .replace(/\.(?:pdf|md|json)$/i, "")
    .trim().replace(/[. ]+$/g, "");
  let stem = "";
  let bytes = 0;
  for (const character of normalized) {
    const size = new TextEncoder().encode(character).byteLength;
    if (bytes + size > PAPER_STEM_MAX_BYTES) break;
    stem += character;
    bytes += size;
  }
  stem = stem.replace(/[. ]+$/g, "") || "document";
  // Windows treats these names as devices, even with an extension.
  if (/^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(stem)) {
    return safePaperStem(`_${stem}`);
  }
  return stem;
}

export function safePdfFilename(value: string): string {
  const basename = value.replace(/\\/g, "/").split("/").pop() || "";
  if (!/\.pdf$/i.test(basename) || !basename.slice(0, -4).trim()) return "";
  return `${safePaperStem(basename)}.pdf`;
}

// Historical backend uploads allowed a 180-byte suffix. New names use 160.
// Retain legacy names verbatim for lookup; never sanitize a storage path.
export function isPaperStorageName(value: string): boolean {
  const match = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}(?:--([^\u0000-\u001f/\\]+))?\.pdf$/u.exec(value);
  return Boolean(match && (!match[1] || new TextEncoder().encode(match[1]).byteLength <= 180));
}
