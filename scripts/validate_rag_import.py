#!/usr/bin/env python3
"""Validate document-card RAG import and public/tenant visibility."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from collections import Counter
from pathlib import Path
from typing import Any


def request_json(
    api_base: str,
    path: str,
    token: str = "",
    method: str = "GET",
    payload: dict[str, Any] | None = None,
    timeout: int = 30,
) -> dict[str, Any]:
    data = None
    headers: dict[str, str] = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
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
        raise RuntimeError(f"{method} {path} failed: HTTP {exc.code}: {detail}") from exc


def login(api_base: str, username: str, password: str) -> str:
    data = request_json(
        api_base,
        "/api/auth/login",
        method="POST",
        payload={"username": username, "password": password},
    )
    token = data.get("token")
    if not isinstance(token, str) or not token:
        raise RuntimeError("login response did not include token")
    return token


def ensure_other_user(api_base: str, admin_token: str, tenant: str, username: str, password: str) -> str:
    tenants = request_json(api_base, "/api/tenants", admin_token).get("tenants", [])
    tenant_id = next((t.get("id") for t in tenants if t.get("name") == tenant), None)
    if tenant_id is None:
        created = request_json(
            api_base,
            "/api/tenants",
            admin_token,
            method="POST",
            payload={"name": tenant, "token_limit_daily": 100000},
        )
        tenant_id = created.get("id")
    if not isinstance(tenant_id, int):
        raise RuntimeError(f"could not resolve tenant id for {tenant}")

    users = request_json(api_base, "/api/users", admin_token).get("users", [])
    exists = any(u.get("tenant_id") == tenant_id and u.get("username") == username for u in users)
    if not exists:
        request_json(
            api_base,
            "/api/users",
            admin_token,
            method="POST",
            payload={
                "tenant_id": tenant_id,
                "username": username,
                "password": password,
                "role": "user",
            },
        )
    return login(api_base, username, password)


def search_response(api_base: str, token: str, query: str, top_k: int) -> dict[str, Any]:
    return request_json(
        api_base,
        "/api/documents/search",
        token,
        method="POST",
        payload={"query": query, "top_k": top_k, "mode": "chunks"},
    )


def search(api_base: str, token: str, query: str, top_k: int) -> list[dict[str, Any]]:
    data = search_response(api_base, token, query, top_k)
    return list(data.get("results", []))


def check(condition: bool, message: str, failures: list[str]) -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {message}")
    if not condition:
        failures.append(message)


def validate_documents(api_base: str, token: str, args: argparse.Namespace, failures: list[str]) -> None:
    docs = request_json(api_base, "/api/documents", token).get("documents", [])
    visibility = Counter(doc.get("visibility") for doc in docs)
    ready = sum(1 for doc in docs if doc.get("status") == "ready")

    check(len(docs) == args.expected_docs, f"documents total == {args.expected_docs} (got {len(docs)})", failures)
    check(ready == args.expected_ready, f"ready documents == {args.expected_ready} (got {ready})", failures)
    check(visibility.get("public", 0) == args.expected_public,
          f"public documents == {args.expected_public} (got {visibility.get('public', 0)})", failures)
    check(visibility.get("group", 0) == args.expected_group,
          f"group documents == {args.expected_group} (got {visibility.get('group', 0)})", failures)


def validate_rag_queries(api_base: str, token: str, query_path: Path, top_k: int,
                         failures: list[str]) -> None:
    queries = json.loads(query_path.read_text(encoding="utf-8"))
    passed = 0
    top3_passed = 0
    for item in queries:
        data = search_response(api_base, token, item["query"], top_k)
        results = list(data.get("results", []))
        filenames = [str(row.get("filename", "")) for row in results]
        ok = any(expected in filenames for expected in item["expected_filenames"])
        top3_ok = any(expected in filenames[:3] for expected in item["expected_filenames"])
        if ok:
            passed += 1
        if top3_ok:
            top3_passed += 1
        check(ok, f"RDMA query hit: {item['query']}", failures)
        if not ok:
            if data.get("query_rewritten"):
                print(f"       rewritten: {data.get('rewritten_query', '')}")
                aliases = data.get("query_aliases", [])
                if aliases:
                    print(f"       aliases: {aliases}")
            print(f"       top: {filenames[:top_k]}")
    check(passed == len(queries), f"RDMA queries {passed}/{len(queries)}", failures)
    print(f"RDMA top-3 hits: {top3_passed}/{len(queries)}")


def validate_public_query(api_base: str, token: str, args: argparse.Namespace,
                          failures: list[str]) -> None:
    results = search(api_base, token, args.public_query, args.top_k)
    filenames = [str(row.get("filename", "")) for row in results]
    ok = any(args.public_expected in filename for filename in filenames)
    check(ok, f"public query hits {args.public_expected}", failures)
    if not ok:
        print(f"       top: {filenames[:args.top_k]}")


def validate_visibility(api_base: str, admin_token: str, args: argparse.Namespace,
                        failures: list[str]) -> None:
    other_token = ensure_other_user(
        api_base,
        admin_token,
        args.other_tenant,
        args.other_username,
        args.other_password,
    )
    docs = request_json(api_base, "/api/documents", other_token).get("documents", [])
    filenames = [str(doc.get("filename", "")) for doc in docs]
    check(len(docs) == args.expected_public,
          f"other tenant sees only public docs (got {len(docs)})", failures)
    check(all(not filename.startswith(args.tenant_prefix) for filename in filenames),
          "other tenant document list excludes RDMA docs", failures)

    results = search(api_base, other_token, args.tenant_probe_query, args.top_k)
    result_names = [str(row.get("filename", "")) for row in results]
    check(all(not filename.startswith(args.tenant_prefix) for filename in result_names),
          "other tenant search excludes RDMA docs", failures)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate RAG import and tenant visibility.")
    parser.add_argument("--api-base", default="http://localhost:9006")
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", default="changeme123")
    parser.add_argument("--rdma-queries", default="tests/rdma_rag_queries.json")
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--expected-docs", type=int, default=22)
    parser.add_argument("--expected-ready", type=int, default=22)
    parser.add_argument("--expected-public", type=int, default=1)
    parser.add_argument("--expected-group", type=int, default=21)
    parser.add_argument("--public-query", default="Attention is all you need Transformer self-attention")
    parser.add_argument("--public-expected", default="public/cards/")
    parser.add_argument("--tenant-prefix", default="tenants/RDMA/")
    parser.add_argument("--tenant-probe-query", default="段佳兵 RDMA 负载均衡")
    parser.add_argument("--other-tenant", default="OTHER")
    parser.add_argument("--other-username", default="rag_visibility_check")
    parser.add_argument("--other-password", default="pass123456")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    failures: list[str] = []

    try:
        admin_token = login(args.api_base, args.username, args.password)
        validate_documents(args.api_base, admin_token, args, failures)
        validate_rag_queries(args.api_base, admin_token, Path(args.rdma_queries), args.top_k, failures)
        validate_public_query(args.api_base, admin_token, args, failures)
        validate_visibility(args.api_base, admin_token, args, failures)
    except Exception as exc:
        print(f"[FAIL] validation error: {exc}", file=sys.stderr)
        return 1

    if failures:
        print(f"\nValidation failed: {len(failures)} checks failed", file=sys.stderr)
        return 1

    print("\nValidation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
