export const publicationTypes = { conference: "会议", journal: "期刊", preprint: "预印本", unknown: "未分类" };
export type PublicationType = keyof typeof publicationTypes;
export type PaperClassification = {
  publication_year: number | null;
  publication_type: PublicationType;
  publication_venue: string;
};

function text(value: unknown): string { return typeof value === "string" ? value.trim() : ""; }

export function normalizeVenue(value: string): string {
  if (/sigcomm/i.test(value)) return "ACM SIGCOMM";
  if (/eurosys/i.test(value)) return "ACM EuroSys";
  if (/\bnsdi\b|networked systems design and implementation/i.test(value)) return "USENIX NSDI";
  if (/\bmicro\b|symposium on microarchitecture/i.test(value)) return "IEEE/ACM MICRO";
  if (/transactions on networking/i.test(value)) return "IEEE/ACM Transactions on Networking";
  if (/arxiv|preprint/i.test(value)) return "arXiv";
  return value.replace(/\([^)]*\)/g, "").replace(/\b(?:19|20)\d{2}\b/g, "").split(/[,;]/)[0].replace(/\s+/g, " ").trim();
}

export function validYear(value: unknown): value is number {
  return Number.isInteger(value) && Number(value) >= 1800 && Number(value) <= new Date().getFullYear() + 1;
}

// Read governed classification fields; legacy normalization supports older cards only.
export function paperMetadata(card: Record<string, unknown>): PaperClassification {
  const legacy = text(card.venue_and_year);
  const reliableVenue = /\[(?:VERIFY|CITE|REVIEW):/i.test(legacy) ? "" : legacy;
  const years = [...new Set(reliableVenue.match(/\b(?:18|19|20|21)\d{2}\b/g) || [])];
  const year = validYear(card.publication_year) ? card.publication_year : years.length === 1 && validYear(Number(years[0])) ? Number(years[0]) : null;
  const venue = text(card.publication_venue) || reliableVenue.replace(/\b(?:18|19|20|21)\d{2}\b/g, "").replace(/^[\s,;()–-]+|[\s,;()–-]+$/g, "").trim();
  let type: PublicationType = "unknown";
  if (Object.hasOwn(publicationTypes, text(card.publication_type))) type = card.publication_type as PublicationType;
  // Only explicit venue words are inferred for older cards; acronyms stay unclassified.
  else if (/\b(journal|transactions)\b/i.test(venue)) type = "journal";
  else if (/\b(conference|symposium|workshop|proceedings)\b/i.test(venue)) type = "conference";
  else if (/\b(arxiv|preprint)\b/i.test(venue)) type = "preprint";
  return {
    publication_year: year, publication_type: type, publication_venue: normalizeVenue(venue),
  };
}

export function parseClassification(value: Record<string, unknown>): PaperClassification | null {
  if (value.publication_year !== null && !validYear(value.publication_year)) return null;
  if (typeof value.publication_type !== "string" || !Object.hasOwn(publicationTypes, value.publication_type)) return null;
  if (typeof value.publication_venue !== "string" || value.publication_venue.length > 200 || /[\u0000-\u001f]/.test(value.publication_venue)) return null;
  return { publication_year: value.publication_year as number | null, publication_type: value.publication_type as PublicationType, publication_venue: normalizeVenue(value.publication_venue) };
}

