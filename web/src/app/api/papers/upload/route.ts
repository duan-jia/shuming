import { execFile } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { mkdir, readFile, rename, unlink, writeFile } from "node:fs/promises";
import path from "node:path";
import { promisify } from "node:util";

export const runtime = "nodejs";

const MAX_PDF_BYTES = 30 * 1024 * 1024;
const PDF_SIGNATURE = "%PDF-";
const execFileAsync = promisify(execFile);

function repositoryRoot() {
  return process.env.PROJECT_ROOT
    ? path.resolve(/* turbopackIgnore: true */ process.env.PROJECT_ROOT)
    : path.resolve(/* turbopackIgnore: true */ process.cwd(), "..");
}

function pdfDirectory() {
  return process.env.PDF_SOURCE_DIR
    ? path.resolve(/* turbopackIgnore: true */ process.env.PDF_SOURCE_DIR)
    : path.join(/* turbopackIgnore: true */ repositoryRoot(), "data", "pdfs");
}

function processedDirectory() {
  return process.env.PDF_PROCESSED_DIR
    ? path.resolve(/* turbopackIgnore: true */ process.env.PDF_PROCESSED_DIR)
    : path.join(/* turbopackIgnore: true */ repositoryRoot(), "data", "processed");
}

function safePdfFilename(filename: string) {
  const base = path.basename(filename).normalize("NFKC");
  if (!base.toLowerCase().endsWith(".pdf")) return "";

  const sanitized = base
    .replace(/[^\p{L}\p{N}._()\- ]/gu, "_")
    .replace(/\s+/g, " ")
    .trim();

  return sanitized && sanitized.toLowerCase() !== ".pdf" ? sanitized : "";
}

function truncateUtf8(value: string, maxBytes: number) {
  let result = "";
  let bytes = 0;
  for (const character of value) {
    const size = Buffer.byteLength(character, "utf8");
    if (bytes + size > maxBytes) break;
    result += character;
    bytes += size;
  }
  return result;
}

function safeDocumentTitle(title: string | undefined, fallbackFilename: string) {
  const fallback = path.parse(fallbackFilename).name;
  const sanitized = (title || fallback)
    .normalize("NFKC")
    .replace(/[<>:"/\\|?*\u0000-\u001f]/g, "_")
    .replace(/\s+/g, " ")
    .replace(/\.(?:pdf|md)$/i, "")
    .replace(/[. ]+$/g, "")
    .trim();
  return truncateUtf8(sanitized || fallback || "document", 160);
}

async function tokenIsValid(authorization: string) {
  const backendBase = process.env.BACKEND_API_BASE || "http://127.0.0.1:9006";
  try {
    const response = await fetch(`${backendBase.replace(/\/$/, "")}/api/documents`, {
      headers: { Authorization: authorization },
      cache: "no-store",
    });
    return response.ok;
  } catch {
    return false;
  }
}

type CompressionResult = {
  status?: string;
  markdown_path?: string;
  json_path?: string;
  model?: string;
  source_sha256?: string;
  title?: string;
  needs_review?: boolean;
  review_reason?: string;
  error?: string;
};

type BackendDocumentResult = {
  id?: number;
  filename?: string;
  status?: string;
  error?: string;
};

class BackendRequestError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly result: BackendDocumentResult
  ) {
    super(message);
  }
}

async function compressPdf(destination: string): Promise<CompressionResult> {
  const outputDirectory = processedDirectory();
  const python = process.env.PDF_PROCESSOR_PYTHON || "python3";
  const script = path.join(
    /* turbopackIgnore: true */ repositoryRoot(),
    "scripts",
    "pdf_to_markdown.py"
  );

  await mkdir(outputDirectory, { recursive: true });
  try {
    const { stdout } = await execFileAsync(
      python,
      [script, "--file", destination, "--output", outputDirectory],
      { timeout: 240_000, maxBuffer: 2 * 1024 * 1024 }
    );
    const result = JSON.parse(stdout.trim()) as CompressionResult;
    if (result.status !== "ready" || !result.markdown_path || !result.json_path) {
      throw new Error(result.error || "大模型没有生成可用的文档摘要。");
    }
    return result;
  } catch (error) {
    const executionError = error as Error & { stderr?: string };
    if (executionError.stderr) {
      try {
        const detail = JSON.parse(executionError.stderr.trim()) as CompressionResult;
        throw new Error(detail.error || executionError.message);
      } catch (parseError) {
        if (parseError instanceof SyntaxError) {
          throw new Error(executionError.stderr.trim() || executionError.message);
        }
        throw parseError;
      }
    }
    throw error;
  }
}

async function submitToRag(
  authorization: string,
  filename: string,
  markdown: string,
  sourceFilename: string,
  sourceStorageName: string,
  sourceSizeBytes: number,
  sourceSha256: string,
  needsReview: boolean,
  reviewReason: string,
  visibility: "public" | "group"
) {
  const backendBase = process.env.BACKEND_API_BASE || "http://127.0.0.1:9006";
  const response = await fetch(`${backendBase.replace(/\/$/, "")}/api/documents`, {
    method: "POST",
    headers: {
      Authorization: authorization,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      filename,
      content: markdown,
      visibility,
      source_filename: sourceFilename,
      source_storage_name: sourceStorageName,
      source_size_bytes: sourceSizeBytes,
      source_sha256: sourceSha256,
      needs_review: needsReview,
      review_reason: reviewReason,
    }),
    cache: "no-store",
    signal: AbortSignal.timeout(60_000),
  });
  const data = (await response.json().catch(() => ({}))) as BackendDocumentResult;
  if (!response.ok || !data.id) {
    throw new BackendRequestError(
      data.error || `RAG 接口返回 HTTP ${response.status}。`,
      response.status,
      data
    );
  }
  return data;
}

async function findDuplicate(authorization: string, sourceSha256: string, visibility: "public" | "group") {
  const backendBase = process.env.BACKEND_API_BASE || "http://127.0.0.1:9006";
  const response = await fetch(
    `${backendBase.replace(/\/$/, "")}/api/documents/deduplicate`,
    {
      method: "POST",
      headers: {
        Authorization: authorization,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ source_sha256: sourceSha256, visibility }),
      cache: "no-store",
      signal: AbortSignal.timeout(30_000),
    }
  );
  const data = (await response.json().catch(() => ({}))) as BackendDocumentResult & {
    duplicate?: boolean;
  };
  if (!response.ok) {
    throw new BackendRequestError(
      data.error || `文档查重接口返回 HTTP ${response.status}。`,
      response.status,
      data
    );
  }
  return data;
}

export async function POST(request: Request) {
  const authorization = request.headers.get("authorization") || "";
  if (!authorization.startsWith("Bearer ") || !(await tokenIsValid(authorization))) {
    return Response.json({ error: "登录状态无效，请重新登录。" }, { status: 401 });
  }

  let formData: FormData;
  try {
    formData = await request.formData();
  } catch {
    return Response.json({ error: "无法读取上传内容。" }, { status: 400 });
  }

  const visibility = formData.get("visibility") || "group";
  if (visibility !== "public" && visibility !== "group") {
    return Response.json({ error: "文档库范围无效。" }, { status: 400 });
  }

  const file = formData.get("file");
  if (!(file instanceof File)) {
    return Response.json({ error: "请选择一个 PDF 文件。" }, { status: 400 });
  }

  const filename = safePdfFilename(file.name);
  if (!filename) {
    return Response.json({ error: "仅支持文件名有效的 .pdf 文件。" }, { status: 400 });
  }
  if (file.size === 0 || file.size > MAX_PDF_BYTES) {
    return Response.json({ error: "PDF 文件必须小于 30 MB 且不能为空。" }, { status: 413 });
  }
  if (file.type && file.type !== "application/pdf" && file.type !== "application/octet-stream") {
    return Response.json({ error: "文件类型不是 PDF。" }, { status: 415 });
  }

  const bytes = Buffer.from(await file.arrayBuffer());
  if (bytes.subarray(0, PDF_SIGNATURE.length).toString("ascii") !== PDF_SIGNATURE) {
    return Response.json({ error: "文件内容不是有效的 PDF。" }, { status: 415 });
  }

  const sourceSha256 = createHash("sha256").update(bytes).digest("hex");
  try {
    const duplicate = await findDuplicate(authorization, sourceSha256, visibility);
    if (duplicate.duplicate) {
      return Response.json(
        {
          error: `该 PDF 已存在于文档库中（文档 #${duplicate.id}，状态：${duplicate.status}）。`,
          document_id: duplicate.id,
          filename: duplicate.filename,
          status: duplicate.status,
        },
        { status: 409 }
      );
    }
  } catch (error) {
    return Response.json(
      {
        error: error instanceof Error ? `PDF 查重失败：${error.message}` : "PDF 查重失败。",
      },
      { status: error instanceof BackendRequestError ? error.status : 502 }
    );
  }

  const sourceDirectory = pdfDirectory();
  const uploadId = randomUUID();
  const storageName = `${uploadId}.pdf`;
  const destination = path.join(sourceDirectory, storageName);
  try {
    await mkdir(sourceDirectory, { recursive: true });
    await writeFile(destination, bytes, { flag: "wx" });
  } catch {
    return Response.json({ error: "保存 PDF 失败。" }, { status: 500 });
  }

  const generatedPaths = new Set<string>();
  let submissionStarted = false;
  try {
    const compression = await compressPdf(destination);
    const markdownPath = path.resolve(/* turbopackIgnore: true */ compression.markdown_path || "");
    const jsonPath = path.resolve(/* turbopackIgnore: true */ compression.json_path || "");
    const artifactStem = path.parse(storageName).name;
    const expectedMarkdownPath = path.resolve(
      /* turbopackIgnore: true */ processedDirectory(),
      `${artifactStem}.md`
    );
    const expectedJsonPath = path.resolve(
      /* turbopackIgnore: true */ processedDirectory(),
      `${artifactStem}.json`
    );
    if (markdownPath !== expectedMarkdownPath || jsonPath !== expectedJsonPath) {
      throw new Error("文档处理器返回了无效的输出路径。");
    }
    generatedPaths.add(markdownPath);
    generatedPaths.add(jsonPath);
    if (compression.source_sha256 !== sourceSha256) {
      throw new Error("文档处理前后的 SHA-256 不一致。");
    }

    const title = safeDocumentTitle(compression.title, filename);
    const titledStem = `${uploadId}--${title}`;
    const sourceStorageName = `${titledStem}.pdf`;
    const titledPdfPath = path.join(sourceDirectory, sourceStorageName);
    const titledMarkdownPath = path.join(processedDirectory(), `${titledStem}.md`);
    const titledJsonPath = path.join(processedDirectory(), `${titledStem}.json`);
    generatedPaths.add(titledPdfPath);
    generatedPaths.add(titledMarkdownPath);
    generatedPaths.add(titledJsonPath);
    await rename(destination, titledPdfPath);
    await rename(markdownPath, titledMarkdownPath);
    await rename(jsonPath, titledJsonPath);

    const markdown = await readFile(titledMarkdownPath, "utf8");
    const sourceFilename = `${title}.pdf`;
    const markdownFilename = `${title}.md`;
    const metadata = JSON.parse(await readFile(titledJsonPath, "utf8")) as Record<string, unknown>;
    metadata.source_pdf = sourceFilename;
    metadata.source_storage_name = sourceStorageName;
    await writeFile(titledJsonPath, `${JSON.stringify(metadata, null, 2)}\n`, "utf8");
    submissionStarted = true;
    const ragResult = await submitToRag(
      authorization,
      markdownFilename,
      markdown,
      sourceFilename,
      sourceStorageName,
      file.size,
      sourceSha256,
      compression.needs_review === true,
      compression.review_reason || "",
      visibility
    );
    return Response.json({
      filename: sourceFilename,
      markdown_filename: markdownFilename,
      json_filename: `${title}.json`,
      model: compression.model,
      document_id: ragResult.id,
      status: ragResult.status || "processing",
    });
  } catch (error) {
    console.error("PDF compression failed", error);
    // A timeout can occur after the backend committed the document. Keep its
    // source files unless rejection is definitive; a retry will deduplicate.
    const safeToCleanup = !submissionStarted || (
      error instanceof BackendRequestError && error.status >= 400 && error.status < 500
    );
    if (safeToCleanup) await Promise.all(
      [destination, ...generatedPaths].map((filePath) =>
        unlink(filePath).catch((cleanupError: NodeJS.ErrnoException) => {
          if (cleanupError.code !== "ENOENT") {
            console.error("Failed to clean up upload artifact", filePath, cleanupError);
          }
        })
      )
    );
    if (error instanceof BackendRequestError && error.status === 409) {
      return Response.json(
        {
          error: `该 PDF 已存在于文档库中（文档 #${error.result.id}，状态：${error.result.status}）。`,
          document_id: error.result.id,
          filename: error.result.filename,
          status: error.result.status,
        },
        { status: 409 }
      );
    }
    return Response.json(
      {
        error: error instanceof Error
          ? `PDF 处理或加入 RAG 失败：${error.message}`
          : "PDF 处理或加入 RAG 失败。",
      },
      { status: 502 }
    );
  }
}
