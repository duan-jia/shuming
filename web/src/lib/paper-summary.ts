// These sections are the paper-card format produced by scripts/paper_card.py.
// Historical PDF extraction artifacts also use .md, but contain page blocks.
export function isPaperSummary(markdown: string): boolean {
  if (/^## .*\| PDF page \d+ \| block \d+/m.test(markdown)) return false;
  const sections = new Map(markdown.split(/^## /m).slice(1).map(section => {
    const newline = section.indexOf("\n");
    return [section.slice(0, newline).trim(), section.slice(newline + 1).trim()];
  }));
  return [
    "Problem and Context",
    "Core Idea and Approach",
    "Key Takeaways",
    "Scope and Limitations",
  ].every(title => Boolean(sections.get(title)));
}
