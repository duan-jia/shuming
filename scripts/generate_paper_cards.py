#!/usr/bin/env python3
"""Generate lightweight paper cards from research PDFs for the RAG system."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import traceback
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


DEFAULT_ENDPOINT = "https://api.deepseek.com/v1/chat/completions"


@dataclass
class PaperCard:
    schema_version: str
    title: str
    short_title: str
    year: int | None
    topic: str
    keywords: list[str]
    summary: str
    main_contribution: str
    useful_for: list[str]
    source_path: str


def clean_text(text: str) -> str:
    replacements = {
        "ﬁ": "fi",
        "ﬂ": "fl",
        "ﬀ": "ff",
        "ﬃ": "ffi",
        "ﬄ": "ffl",
        "\u00ad": "",
    }
    for src, dst in replacements.items():
        text = text.replace(src, dst)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"([A-Za-z])-\n([a-z])", r"\1\2", text)
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def read_pdf_pages(path: Path, first_pages: int, last_pages: int) -> str:
    try:
        from pypdf import PdfReader
    except ImportError as exc:
        raise RuntimeError("pypdf is not installed; install with `pip install pypdf`") from exc

    reader = PdfReader(str(path))
    page_count = len(reader.pages)
    page_indexes = set(range(min(first_pages, page_count)))
    if last_pages > 0:
        page_indexes.update(range(max(0, page_count - last_pages), page_count))

    chunks: list[str] = []
    for idx in sorted(page_indexes):
        text = reader.pages[idx].extract_text() or ""
        if text.strip():
            chunks.append(f"--- Page {idx + 1} ---\n{text.strip()}")
    return clean_text("\n\n".join(chunks))


def extract_section(text: str, start_patterns: list[str], end_patterns: list[str], limit: int) -> str:
    start_match = None
    for pattern in start_patterns:
        match = re.search(pattern, text, flags=re.IGNORECASE | re.MULTILINE)
        if match and (start_match is None or match.start() < start_match.start()):
            start_match = match
    if not start_match:
        return ""

    section = text[start_match.end():]
    end_index = len(section)
    for pattern in end_patterns:
        match = re.search(pattern, section, flags=re.IGNORECASE | re.MULTILINE)
        if match:
            end_index = min(end_index, match.start())
    return section[:end_index].strip()[:limit]


def build_model_context(path: Path, first_pages: int, last_pages: int, max_chars: int) -> str:
    text = read_pdf_pages(path, first_pages, last_pages)
    if not text:
        raise RuntimeError("PDF text extraction returned empty content")

    abstract = extract_section(
        text,
        [r"\bAbstract\b[:.\s-]*"],
        [r"\b1\.?\s+Introduction\b", r"\bIntroduction\b", r"\bKeywords\b"],
        4500,
    )
    conclusion = extract_section(
        text,
        [r"\bConclusion(?:s)?\b[:.\s-]*", r"\bDiscussion and Conclusion(?:s)?\b[:.\s-]*"],
        [r"\bReferences\b", r"\bAcknowledg(?:e)?ments\b"],
        3500,
    )

    parts = [f"PDF filename: {path.name}"]
    if abstract:
        parts.append(f"Abstract candidate:\n{abstract}")
    if conclusion:
        parts.append(f"Conclusion candidate:\n{conclusion}")
    parts.append(f"Extracted text sample:\n{text[:max_chars]}")
    return "\n\n".join(parts)[:max_chars]


def prompt_for_card(context: str, source_path: str) -> list[dict[str, str]]:
    schema = {
        "schema_version": "paper_card.v1",
        "title": "",
        "short_title": "",
        "year": None,
        "topic": "",
        "keywords": [],
        "summary": "",
        "main_contribution": "",
        "useful_for": [],
        "source_path": source_path,
    }
    return [
        {
            "role": "system",
            "content": (
                "You extract concise research-paper metadata for a lab RAG system. "
                "Return strict JSON only. Do not include markdown fences or commentary."
            ),
        },
        {
            "role": "user",
            "content": (
                "Create one paper card from the extracted PDF text. "
                "Prefer factual, retrieval-friendly wording. Keep summary within 120 Chinese characters "
                "or 80 English words. Keep main_contribution within 2 short sentences. "
                "Use 3-8 keywords and 2-5 useful_for items. If year is unknown, use null.\n\n"
                f"Required JSON schema:\n{json.dumps(schema, ensure_ascii=False)}\n\n"
                f"Extracted PDF text:\n{context}"
            ),
        },
    ]


def call_chat_completion(endpoint: str, api_key: str, model: str, messages: list[dict[str, str]],
                         temperature: float, timeout: int) -> str:
    payload = json.dumps(
        {
            "model": model,
            "messages": messages,
            "temperature": temperature,
            "response_format": {"type": "json_object"},
        },
        ensure_ascii=False,
    ).encode("utf-8")
    req = urllib.request.Request(
        endpoint,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        detail = body[:1000] if body else exc.reason
        raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
    return data["choices"][0]["message"]["content"]


def parse_json_object(text: str) -> dict[str, Any]:
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        match = re.search(r"\{.*\}", text, flags=re.DOTALL)
        if not match:
            raise
        return json.loads(match.group(0))


def string_list(value: Any) -> list[str]:
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    if isinstance(value, str) and value.strip():
        return [value.strip()]
    return []


def coerce_card(raw: dict[str, Any], source_path: Path) -> PaperCard:
    year_value = raw.get("year")
    year: int | None = None
    if isinstance(year_value, int):
        year = year_value
    elif isinstance(year_value, str) and re.fullmatch(r"\d{4}", year_value.strip()):
        year = int(year_value.strip())

    title = str(raw.get("title") or source_path.stem).strip()
    short_title = str(raw.get("short_title") or title[:80]).strip()
    return PaperCard(
        schema_version="paper_card.v1",
        title=title,
        short_title=short_title,
        year=year,
        topic=str(raw.get("topic") or "").strip(),
        keywords=string_list(raw.get("keywords")),
        summary=str(raw.get("summary") or "").strip(),
        main_contribution=str(raw.get("main_contribution") or "").strip(),
        useful_for=string_list(raw.get("useful_for")),
        source_path=str(source_path),
    )


def card_to_markdown(card: PaperCard) -> str:
    year = str(card.year) if card.year is not None else "unknown"
    keywords = ", ".join(card.keywords) if card.keywords else "unknown"
    useful_for = "\n".join(f"- {item}" for item in card.useful_for) or "- unknown"
    return (
        f"# {card.title}\n\n"
        "<!--\n"
        "doc_type: paper_card\n"
        f"schema_version: {card.schema_version}\n"
        f"short_title: {card.short_title}\n"
        f"year: {year}\n"
        f"topic: {card.topic}\n"
        f"keywords: {keywords}\n"
        f"source_path: {card.source_path}\n"
        "-->\n\n"
        f"## Summary\n\n{card.summary or 'unknown'}\n\n"
        f"## Main Contribution\n\n{card.main_contribution or 'unknown'}\n\n"
        f"## Useful For\n\n{useful_for}\n"
    )


def safe_stem(path: Path) -> str:
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", path.stem).strip("._-")
    return stem or "paper"


def iter_pdfs(root: Path, only: str) -> list[Path]:
    files = sorted(path for path in root.rglob("*.pdf") if path.is_file())
    if not only:
        return files
    return [path for path in files if only in path.relative_to(root).as_posix()]


def write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row, ensure_ascii=False) + "\n")


def process_pdf(path: Path, input_root: Path, output_root: Path, args: argparse.Namespace,
                api_key: str) -> dict[str, Any]:
    rel = path.relative_to(input_root)
    paper_dir = output_root / safe_stem(path)
    json_path = paper_dir / "paper_card.json"
    md_path = paper_dir / "paper_card.md"

    row: dict[str, Any] = {
        "source_path": str(path),
        "output_json": str(json_path),
        "output_markdown": str(md_path),
        "status": "processing",
    }
    try:
        if json_path.exists() and md_path.exists() and not args.force:
            card = coerce_card(json.loads(json_path.read_text(encoding="utf-8")), path)
            row["status"] = "cached"
        else:
            context = build_model_context(path, args.first_pages, args.last_pages, args.max_chars)
            if args.dry_run:
                print(f"\n--- {rel} extracted context preview ---\n{context[: args.preview_chars]}\n")
                row["status"] = "dry_run"
                return row
            if not api_key:
                raise RuntimeError("DEEPSEEK_API_KEY is required unless --dry-run is used")

            content = call_chat_completion(
                args.endpoint,
                api_key,
                args.model,
                prompt_for_card(context, str(path)),
                args.temperature,
                args.timeout,
            )
            card = coerce_card(parse_json_object(content), path)
            paper_dir.mkdir(parents=True, exist_ok=True)
            json_path.write_text(
                json.dumps(asdict(card), ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            md_path.write_text(card_to_markdown(card), encoding="utf-8")
            row["status"] = "ready"

        row.update(asdict(card))
    except Exception as exc:
        row["status"] = "failed"
        row["error"] = str(exc)
        if args.verbose:
            traceback.print_exc()
    return row


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate lightweight JSON and Markdown paper cards from PDF papers."
    )
    parser.add_argument("--input", default="data/tenants/RDMA/paper_pdf", help="Source PDF directory")
    parser.add_argument("--output", default="data/paper_cards/RDMA", help="Paper card output directory")
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT, help="OpenAI-compatible chat completions endpoint")
    parser.add_argument("--model", default="deepseek-chat", help="DeepSeek model name")
    parser.add_argument("--api-key", default="", help="API key; defaults to DEEPSEEK_API_KEY")
    parser.add_argument("--only", default="", help="Only process PDFs whose relative path contains this text")
    parser.add_argument("--limit", type=int, default=0, help="Maximum number of PDFs to process")
    parser.add_argument("--force", action="store_true", help="Regenerate existing paper cards")
    parser.add_argument("--dry-run", action="store_true", help="Only extract and preview text; do not call the API")
    parser.add_argument("--first-pages", type=int, default=3, help="Number of first pages to extract")
    parser.add_argument("--last-pages", type=int, default=2, help="Number of last pages to extract")
    parser.add_argument("--max-chars", type=int, default=18000, help="Maximum extracted text passed to the model")
    parser.add_argument("--preview-chars", type=int, default=4000, help="Preview length for --dry-run")
    parser.add_argument("--temperature", type=float, default=0.1)
    parser.add_argument("--timeout", type=int, default=60, help="API timeout in seconds")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    input_root = Path(args.input).resolve()
    output_root = Path(args.output).resolve()
    api_key = args.api_key or os.environ.get("DEEPSEEK_API_KEY", "")

    if not input_root.exists():
        print(f"Input directory does not exist: {input_root}", file=sys.stderr)
        return 2

    files = iter_pdfs(input_root, args.only)
    if args.limit > 0:
        files = files[: args.limit]
    if not files:
        print("No PDF files matched the input filters", file=sys.stderr)
        return 2

    rows: list[dict[str, Any]] = []
    for path in files:
        row = process_pdf(path, input_root, output_root, args, api_key)
        rows.append(row)
        print(f"{row.get('status', 'unknown'):8s} {path.relative_to(input_root)}")

    if not args.dry_run:
        write_jsonl(output_root / "cards.jsonl", rows)
        failures = [row for row in rows if row.get("status") == "failed"]
        error_path = output_root / "errors.jsonl"
        if failures:
            write_jsonl(error_path, failures)
        elif error_path.exists():
            error_path.unlink()

    ready_count = sum(1 for row in rows if row.get("status") in {"ready", "cached"})
    failed_count = sum(1 for row in rows if row.get("status") == "failed")
    print(f"\nProcessed {len(rows)} PDFs: {ready_count} ready/cached, {failed_count} failed")
    if not args.dry_run:
        print(f"Cards: {output_root / 'cards.jsonl'}")
    return 1 if failed_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
