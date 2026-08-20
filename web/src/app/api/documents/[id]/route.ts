import { unlink } from "node:fs/promises";
import path from "node:path";

export const runtime = "nodejs";

const STORAGE_NAME_PATTERN =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}(?:--[^\u0000-\u001f/\\]{1,160})?\.pdf$/u;

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

async function responseData(response: Response) {
  return (await response.json().catch(() => ({}))) as {
    error?: string;
    source_storage_name?: string | null;
    status?: string;
  };
}

export async function DELETE(
  request: Request,
  { params }: { params: Promise<{ id: string }> }
) {
  const authorization = request.headers.get("authorization") || "";
  if (!authorization.startsWith("Bearer ")) {
    return Response.json({ error: "登录状态无效，请重新登录。" }, { status: 401 });
  }

  const { id } = await params;
  if (!/^[1-9]\d*$/.test(id)) {
    return Response.json({ error: "文档编号无效。" }, { status: 400 });
  }

  const headers = { Authorization: authorization };
  let detailResponse: Response;
  try {
    detailResponse = await fetch(backendUrl(`/api/documents/${id}`), {
      headers,
      cache: "no-store",
    });
  } catch {
    return Response.json({ error: "后端文档服务暂时不可用。" }, { status: 502 });
  }
  const detail = await responseData(detailResponse);
  if (!detailResponse.ok) {
    return Response.json(
      { error: detail.error || "无法读取文档信息。" },
      { status: detailResponse.status }
    );
  }

  let deleteResponse: Response;
  try {
    deleteResponse = await fetch(backendUrl(`/api/documents/${id}`), {
      method: "DELETE",
      headers,
      cache: "no-store",
    });
  } catch {
    return Response.json({ error: "后端文档服务暂时不可用。" }, { status: 502 });
  }
  const deleted = await responseData(deleteResponse);
  if (!deleteResponse.ok) {
    return Response.json(
      { error: deleted.error || "删除文档失败。" },
      { status: deleteResponse.status }
    );
  }

  const storageName = detail.source_storage_name || "";
  const cleanupFailures: string[] = [];
  if (STORAGE_NAME_PATTERN.test(storageName)) {
    const stem = path.parse(storageName).name;
    const artifacts = [
      path.join(pdfDirectory(), storageName),
      path.join(processedDirectory(), `${stem}.md`),
      path.join(processedDirectory(), `${stem}.json`),
    ];
    await Promise.all(
      artifacts.map(async (artifact) => {
        try {
          await unlink(artifact);
        } catch (error) {
          if ((error as NodeJS.ErrnoException).code !== "ENOENT") {
            cleanupFailures.push(path.basename(artifact));
            console.error("Failed to remove document artifact", artifact, error);
          }
        }
      })
    );
  } else if (storageName) {
    cleanupFailures.push("invalid-storage-name");
    console.error("Refused to remove document artifact with invalid storage name", storageName);
  }

  return Response.json({
    status: deleted.status || "deleted",
    cleanup_warning: cleanupFailures.length > 0
      ? `文档已删除，但 ${cleanupFailures.length} 个磁盘文件清理失败。`
      : null,
  });
}
