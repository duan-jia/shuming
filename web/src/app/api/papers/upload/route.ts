import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdir, readFile, unlink, writeFile } from "node:fs/promises";
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
  error?: string;
};

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
  sourceSizeBytes: number
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
      visibility: "group",
      source_filename: sourceFilename,
      source_storage_name: sourceStorageName,
      source_size_bytes: sourceSizeBytes,
    }),
    cache: "no-store",
    signal: AbortSignal.timeout(60_000),
  });
  const data = (await response.json().catch(() => ({}))) as {
    id?: number;
    status?: string;
    error?: string;
  };
  if (!response.ok || !data.id) {
    throw new Error(data.error || `RAG 接口返回 HTTP ${response.status}。`);
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

  const sourceDirectory = pdfDirectory();
  const storageName = `${randomUUID()}.pdf`;
  const destination = path.join(sourceDirectory, storageName);
  try {
    await mkdir(sourceDirectory, { recursive: true });
    await writeFile(destination, bytes, { flag: "wx" });
  } catch {
    return Response.json({ error: "保存 PDF 失败。" }, { status: 500 });
  }

  const generatedPaths: string[] = [];
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
    generatedPaths.push(markdownPath, jsonPath);
    const markdown = await readFile(markdownPath, "utf8");
    const markdownFilename = `${path.parse(filename).name}.md`;
    const ragResult = await submitToRag(
      authorization,
      markdownFilename,
      markdown,
      filename,
      storageName,
      file.size
    );
    return Response.json({
      filename,
      markdown_filename: markdownFilename,
      json_filename: `${path.parse(filename).name}.json`,
      model: compression.model,
      document_id: ragResult.id,
      status: ragResult.status || "processing",
    });
  } catch (error) {
    console.error("PDF compression failed", error);
    await Promise.all(
      [destination, ...generatedPaths].map((filePath) =>
        unlink(filePath).catch((cleanupError: NodeJS.ErrnoException) => {
          if (cleanupError.code !== "ENOENT") {
            console.error("Failed to clean up upload artifact", filePath, cleanupError);
          }
        })
      )
    );
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
