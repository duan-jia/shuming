export const SESSION_TITLE_LIMIT = 24;

export function sessionTitle(question: string): string {
  const text = question.trim().replace(/\s+/gu, " ");
  const characters = Array.from(new Intl.Segmenter(undefined, { granularity: "grapheme" }).segment(text),
    ({ segment }) => segment);
  return characters.slice(0, SESSION_TITLE_LIMIT).join("")
    + (characters.length > SESSION_TITLE_LIMIT ? "…" : "");
}
