#!/usr/bin/env python3
"""Upload Markdown files listed in a RAG import manifest."""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row, ensure_ascii=False) + "\n")


def request_json(api_base: str, path: str, token: str, method: str = "GET",
                 payload: dict[str, Any] | None = None, timeout: int = 60) -> dict[str, Any]:
    data = None
    headers = {"Authorization": f"Bearer {token}"}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(
        f"{api_base.rstrip('/')}{path}",
        data=data,
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        detail = body[:1000] if body else exc.reason
        raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc


def login(api_base: str, username: str, password: str) -> str:
    payload = json.dumps({"username": username, "password": password}).encode("utf-8")
    req = urllib.request.Request(
        f"{api_base.rstrip('/')}/api/auth/login",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))["token"]
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        detail = body[:1000] if body else exc.reason
        raise RuntimeError(f"login failed: HTTP {exc.code}: {detail}") from exc


def upload_row(api_base: str, token: str, row: dict[str, Any]) -> dict[str, Any]:
    markdown_path = Path(str(row["markdown_path"]))
    content = markdown_path.read_text(encoding="utf-8")
    payload = {
        "filename": row["filename"],
        "content": content,
        "visibility": row.get("visibility", "group"),
    }
    result = request_json(api_base, "/api/documents", token, method="POST", payload=payload)
    return {
        "source_path": row.get("source_path", ""),
        "markdown_path": str(markdown_path),
        "filename": row["filename"],
        "doc_type": row.get("doc_type", ""),
        "owner": row.get("owner", ""),
        "topic": row.get("topic", ""),
        "visibility": row.get("visibility", "group"),
        "tenant_name": row.get("tenant_name", ""),
        "status": result.get("status", "unknown"),
        "doc_id": result.get("id"),
        "response": result,
    }


def wait_for_ready(api_base: str, token: str, doc_ids: set[int], timeout_seconds: int,
                   poll_seconds: float) -> dict[int, str]:
    deadline = time.time() + timeout_seconds
    statuses: dict[int, str] = {}
    while time.time() < deadline:
        data = request_json(api_base, "/api/documents", token)
        for doc in data.get("documents", []):
            doc_id = doc.get("id")
            if isinstance(doc_id, int) and doc_id in doc_ids:
                statuses[doc_id] = doc.get("status", "unknown")
        if doc_ids and all(statuses.get(doc_id) in {"ready", "failed", "deleted"} for doc_id in doc_ids):
            return statuses
        time.sleep(poll_seconds)
    return statuses


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Upload RAG Markdown files from a manifest.")
    parser.add_argument("--manifest", default="data/governance/tenants/RDMA/rag_import_manifest.jsonl")
    parser.add_argument("--result", default="data/governance/tenants/RDMA/rag_upload_results.jsonl")
    parser.add_argument("--api-base", default="http://localhost:9006")
    parser.add_argument("--token", default="", help="JWT token; otherwise use username/password")
    parser.add_argument("--username", default="")
    parser.add_argument("--password", default="")
    parser.add_argument("--limit", type=int, default=0, help="Upload at most N rows")
    parser.add_argument("--dry-run", action="store_true", help="Validate manifest and print planned uploads")
    parser.add_argument("--wait", action="store_true", help="Poll /api/documents until uploaded docs finish processing")
    parser.add_argument("--wait-timeout", type=int, default=180)
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    rows = read_jsonl(Path(args.manifest))
    if args.limit > 0:
        rows = rows[: args.limit]
    if not rows:
        print("No manifest rows to upload", file=sys.stderr)
        return 2

    missing = [row for row in rows if not Path(str(row.get("markdown_path", ""))).exists()]
    if missing:
        print(f"{len(missing)} markdown files are missing", file=sys.stderr)
        for row in missing[:10]:
            print(f"missing: {row.get('markdown_path', '')}", file=sys.stderr)
        return 2

    if args.dry_run:
        for row in rows:
            scope = row.get("visibility", "group")
            tenant = row.get("tenant_name", "")
            if scope == "group" and tenant:
                scope = f"group:{tenant}"
            print(f"upload [{scope}] {row['filename']} <- {row['markdown_path']}")
        return 0

    token = args.token
    if not token:
        if not args.username or not args.password:
            print("--token or --username/--password required", file=sys.stderr)
            return 2
        token = login(args.api_base, args.username, args.password)

    results: list[dict[str, Any]] = []
    for row in rows:
        try:
            result = upload_row(args.api_base, token, row)
        except Exception as exc:
            result = {
                "source_path": row.get("source_path", ""),
                "markdown_path": row.get("markdown_path", ""),
                "filename": row.get("filename", ""),
                "status": "failed",
                "error": str(exc),
            }
        results.append(result)
        print(f"{result.get('status', 'unknown'):10s} {result.get('filename', '')}")

    if args.wait:
        doc_ids = {int(row["doc_id"]) for row in results if isinstance(row.get("doc_id"), int)}
        statuses = wait_for_ready(args.api_base, token, doc_ids, args.wait_timeout, args.poll_seconds)
        for row in results:
            doc_id = row.get("doc_id")
            if isinstance(doc_id, int):
                row["final_status"] = statuses.get(doc_id, "unknown")

    write_jsonl(Path(args.result), results)
    failed = sum(1 for row in results if row.get("status") == "failed")
    print(f"\nUploaded {len(results) - failed}/{len(results)} documents")
    print(f"Results: {args.result}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
