import { readFile } from "node:fs/promises";
import path from "node:path";
import { isPaperSummary } from "../../../../../../lib/paper-summary.ts";

export const runtime = "nodejs";

const STORAGE_NAME_PATTERN =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}(?:--[^\u0000-\u001f/\\]{1,160})?\.pdf$/u;

type DocumentDetail = {
  error?: string;
  filename?: string;
  source_filename?: string | null;
  source_storage_name?: string | null;
  content?: string;
};

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

function backendUrl(route: string) {
  const base = process.env.BACKEND_API_BASE || "http://127.0.0.1:9006";
  return `${base.replace(/\/$/, "")}${route}`;
}

function contentDisposition(filename: string) {
  const fallback = filename.replace(/[^A-Za-z0-9._-]/g, "_") || "document";
  return `attachment; filename="${fallback}"; filename*=UTF-8''${encodeURIComponent(filename)}`;
}

export async function GET(
  request: Request,
  { params }: { params: Promise<{ id: string; format: string }> }
) {
  const authorization = request.headers.get("authorization") || "";
  if (!authorization.startsWith("Bearer ")) {
    return Response.json({ error: "登录状态无效，请重新登录。" }, { status: 401 });
  }

  const { id, format } = await params;
  if (!/^[1-9]\d*$/.test(id) || (format !== "pdf" && format !== "md")) {
    return Response.json({ error: "下载参数无效。" }, { status: 400 });
  }

  let backendResponse: Response;
  try {
    backendResponse = await fetch(backendUrl(`/api/documents/${id}`), {
      headers: { Authorization: authorization },
      cache: "no-store",
    });
  } catch {
    return Response.json({ error: "后端文档服务暂时不可用。" }, { status: 502 });
  }
  const detail = (await backendResponse.json().catch(() => ({}))) as DocumentDetail;
  if (!backendResponse.ok) {
    return Response.json(
      { error: detail.error || "无法读取文档信息。" },
      { status: backendResponse.status }
    );
  }

  if (format === "md") {
    const filename = detail.filename || `document-${id}.md`;
    let markdown = detail.content || "";
    if (detail.source_filename || detail.source_storage_name) {
      // Older corpus imports put extracted PDF text in documents.content.
      // The generated paper card is a separate artifact; never download the
      // source-text cache as a substitute for a missing summary.
      const storageName = detail.source_storage_name || "";
      if (STORAGE_NAME_PATTERN.test(storageName)) {
        try {
          const generated = await readFile(
            path.join(processedDirectory(), `${path.parse(storageName).name}.md`),
            "utf8"
          );
          if (isPaperSummary(generated)) markdown = generated;
        } catch (error) {
          if ((error as NodeJS.ErrnoException).code !== "ENOENT") {
            console.error("Failed to read paper summary", storageName, error);
            return Response.json({ error: "读取论文摘要失败。" }, { status: 500 });
          }
        }
      }
      if (!isPaperSummary(markdown)) {
        return Response.json({
          error: "该论文尚无可用的总结 MD，请先生成论文摘要。",
          code: "SUMMARY_NOT_AVAILABLE",
        }, { status: 409 });
      }
    }
    const content = new TextEncoder().encode(markdown);
    return new Response(content, {
      headers: {
        "Content-Type": "text/markdown; charset=utf-8",
        "Content-Disposition": contentDisposition(filename),
        "Content-Length": String(content.byteLength),
        "Cache-Control": "private, no-store",
      },
    });
  }

  const storageName = detail.source_storage_name || "";
  if (!STORAGE_NAME_PATTERN.test(storageName) || !detail.source_filename) {
    return Response.json({ error: "该文档没有可下载的源 PDF。" }, { status: 404 });
  }

  try {
    const bytes = await readFile(path.join(pdfDirectory(), storageName));
    return new Response(new Uint8Array(bytes), {
      headers: {
        "Content-Type": "application/pdf",
        "Content-Disposition": contentDisposition(detail.source_filename),
        "Content-Length": String(bytes.byteLength),
        "Cache-Control": "private, no-store",
      },
    });
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") {
      return Response.json({ error: "源 PDF 文件不存在。" }, { status: 404 });
    }
    console.error("Failed to read source PDF", storageName, error);
    return Response.json({ error: "读取源 PDF 失败。" }, { status: 500 });
  }
}
