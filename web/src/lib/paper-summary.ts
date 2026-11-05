// These sections are the paper-card format produced by scripts/paper_card.py.
// Historical PDF extraction artifacts also use .md, but contain page blocks.
export function isPaperSummary(markdown: string): boolean {
  if (/^## .*\| PDF page \d+ \| block \d+/m.test(markdown)) return false;
  const sections = new Map(markdown.split(/^## /m).slice(1).map(section => {
    const newline = section.indexOf("\n");
    return [section.slice(0, newline).trim(), section.slice(newline + 1).trim()];
  }));
  const current = [
    "Problem and Context",
    "Core Idea and Approach",
    "Key Takeaways",
    "Scope and Limitations",
  ].every(title => Boolean(sections.get(title)));
  // Earlier curated paper summaries use these headings. Preserve their prose
  // instead of rewriting it merely to make an existing MD downloadable.
  const legacy = ["Summary", "Key Takeaways", "Scope and Positioning"]
    .every(title => Boolean(sections.get(title)));
  return current || legacy;
}
