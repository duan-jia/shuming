import { isPaperStorageName } from "./paper-filenames.ts";
import { readFile, mkdir, writeFile, rename, unlink } from "node:fs/promises";
import { randomUUID } from "node:crypto";
import path from "node:path";
import { paperMetadata, type PaperClassification } from "./paper-library.ts";

export function backendUrl(route: string) {
  return `${(process.env.BACKEND_API_BASE || "http://127.0.0.1:9006").replace(/\/$/, "")}${route}`;
}

function processedDirectory() {
  const root = process.env.PROJECT_ROOT || path.resolve(/* turbopackIgnore: true */ process.cwd(), "..");
  return process.env.PDF_PROCESSED_DIR || path.join(/* turbopackIgnore: true */ root, "data", "processed");
}

function classificationPath(id: number) {
  if (!Number.isSafeInteger(id) || id <= 0) throw new Error("Invalid document ID");
  return path.join(processedDirectory(), "library", `${id}.json`);
}

async function readJson(file: string): Promise<Record<string, unknown>> {
  try {
    const value: unknown = JSON.parse(await readFile(file, "utf8"));
    return value && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : {};
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT" || error instanceof SyntaxError) return {};
    throw error;
  }
}

export async function readPaperMetadata(document: { id: number; source_storage_name?: string | null }) {
  const card = await readMetadataJson(document);
  const title = typeof card.title === "string" ? card.title.replace(/\s+/g, " ").trim() : "";
  return { ...paperMetadata(card), title: title || null };
}

export async function readMetadataJson(document: { id: number; source_storage_name?: string | null }): Promise<Record<string, unknown>> {
  let card: Record<string, unknown> = {};
  const storage = document.source_storage_name || "";
  if (isPaperStorageName(storage)) {
    card = await readJson(path.join(processedDirectory(), storage.replace(/\.pdf$/, ".json")));
  }
  const override = await readJson(classificationPath(document.id));
  return { ...card, ...override, ...paperMetadata({ ...card, ...override }) };
}

export async function writeClassification(id: number, classification: PaperClassification | Record<string, unknown>) {
  const target = classificationPath(id);
  await mkdir(path.dirname(target), { recursive: true });
  const temporary = `${target}.${randomUUID()}.tmp`;
  try {
    await writeFile(temporary, `${JSON.stringify(classification)}\n`, { flag: "wx" });
    await rename(temporary, target);
  } finally { await unlink(temporary).catch(error => { if (error.code !== "ENOENT") throw error; }); }
}
