#!/usr/bin/env python3
"""Compress one text-based PDF into a compact Markdown document card."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from paper_card import build_prompt, markdown_for, normalize_card


DEFAULT_ENDPOINT = "http://127.0.0.1:8010/v1"
DEFAULT_MODEL = "Qwen3.8-27B"


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def load_remote_config(config_path: Path) -> tuple[str, str, str]:
    config: dict[str, Any] = {}
    if config_path.exists():
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"cannot read config: {exc}") from exc

    remote = config.get("remote_engine", {})
    if not isinstance(remote, dict):
        remote = {}
    endpoint = os.environ.get("REMOTE_ENGINE_ENDPOINT") or str(
        remote.get("endpoint") or DEFAULT_ENDPOINT
    )
    model = os.environ.get("REMOTE_ENGINE_MODEL") or str(
        remote.get("model") or DEFAULT_MODEL
    )
    api_key = (
        os.environ.get("REMOTE_ENGINE_API_KEY")
        or str(remote.get("api_key") or "")
        or os.environ.get("DEEPSEEK_API_KEY")
    )
    return endpoint, model, api_key


def chat_completions_url(endpoint: str) -> str:
    endpoint = endpoint.rstrip("/")
    if endpoint.endswith("/chat/completions"):
        return endpoint
    return f"{endpoint}/chat/completions"


def validate_pdf(path: Path) -> None:
    if not path.is_file() or path.suffix.lower() != ".pdf":
        raise RuntimeError("input must be an existing .pdf file")
    with path.open("rb") as stream:
        if stream.read(5) != b"%PDF-":
            raise RuntimeError("file content is not a PDF")


def clean_text(text: str) -> str:
    replacements = {
        "ﬁ": "fi",
        "ﬂ": "fl",
        "ﬀ": "ff",
        "ﬃ": "ffi",
        "ﬄ": "ffl",
        "\u00ad": "",
    }
    for source, replacement in replacements.items():
        text = text.replace(source, replacement)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"([A-Za-z])-\n([a-z])", r"\1\2", text)
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def extract_pdf(path: Path, max_chars: int) -> tuple[str, int, int]:
    try:
        from pypdf import PdfReader
    except ImportError as exc:
        raise RuntimeError("pypdf is required; install it with `python3 -m pip install pypdf`") from exc

    reader = PdfReader(str(path))
    pages = [clean_text(page.extract_text() or "") for page in reader.pages]
    nonempty = [(index + 1, text) for index, text in enumerate(pages) if text]
    if not nonempty:
        raise RuntimeError("PDF contains no extractable text; scanned PDFs are not supported")

    full_length = sum(len(text) for _, text in nonempty)
    if full_length <= max_chars:
        selected = nonempty
    else:
        per_page = max(400, max_chars // len(nonempty))
        selected = [(number, text[:per_page]) for number, text in nonempty]

    context = "\n\n".join(
        f"--- Page {number} ---\n{text}" for number, text in selected
    )[:max_chars]
    return context, len(reader.pages), full_length


def call_model(endpoint: str, model: str, api_key: str, messages: list[dict[str, str]], timeout: int) -> dict[str, Any]:
    if not api_key:
        raise RuntimeError("remote_engine.api_key or REMOTE_ENGINE_API_KEY is required")
    payload = json.dumps(
        {
            "model": model,
            "messages": messages,
            "temperature": 0.1,
            "max_tokens": 3500,
            "response_format": {"type": "json_object"},
        },
        ensure_ascii=False,
    ).encode("utf-8")
    request = urllib.request.Request(
        chat_completions_url(endpoint),
        data=payload,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:1000]
        raise RuntimeError(f"model API returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"model API request failed: {exc.reason}") from exc

    content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
    if not content:
        raise RuntimeError("model API returned no content")
    try:
        return json.loads(content)
    except json.JSONDecodeError:
        match = re.search(r"\{.*\}", content, flags=re.DOTALL)
        if not match:
            raise RuntimeError("model response is not valid JSON")
        return json.loads(match.group(0))


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            stream.write(content)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Compress one text-based PDF into Markdown and JSON.")
    parser.add_argument("--file", required=True, help="PDF file to process")
    parser.add_argument("--output", default="data/processed", help="Output directory")
    parser.add_argument("--config", default="config.json", help="Project configuration file")
    parser.add_argument("--max-chars", type=int, default=100_000,
                        help="Maximum extracted characters sent to the model")
    parser.add_argument("--timeout", type=int, default=180, help="Model API timeout in seconds")
    parser.add_argument("--force", action="store_true", help="Replace an existing matching result")
    parser.add_argument("--dry-run", action="store_true",
                        help="Validate and extract the PDF without calling the model or writing output")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    source = Path(args.file).resolve()
    output_dir = Path(args.output).resolve()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = project_root() / config_path

    try:
        validate_pdf(source)
        source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        context, page_count, extracted_chars = extract_pdf(source, args.max_chars)
        markdown_path = output_dir / f"{source.stem}.md"
        json_path = output_dir / f"{source.stem}.json"

        if args.dry_run:
            print(json.dumps({
                "status": "dry_run",
                "source_pdf": source.name,
                "source_sha256": source_hash,
                "page_count": page_count,
                "extracted_chars": extracted_chars,
                "selected_chars": len(context),
            }, ensure_ascii=False))
            return 0

        if markdown_path.exists() or json_path.exists():
            if not args.force:
                raise RuntimeError("output already exists; use --force to replace it")

        endpoint, model, api_key = load_remote_config(config_path)
        raw = call_model(endpoint, model, api_key, build_prompt(source.name, context), args.timeout)
        card = normalize_card(raw, source, source_hash, model, page_count, extracted_chars)
        if extracted_chars < 2_000:
            card["needs_review"] = True
            card["review_reason"] = "The PDF contains less than 2,000 extracted characters."
        write_atomic(json_path, json.dumps(card, ensure_ascii=False, indent=2) + "\n")
        write_atomic(markdown_path, markdown_for(card))
        print(json.dumps({
            "status": "ready",
            "markdown_path": str(markdown_path),
            "json_path": str(json_path),
            "model": model,
            "source_sha256": source_hash,
            "title": card["title"],
            "needs_review": card["needs_review"],
            "review_reason": card["review_reason"],
        }, ensure_ascii=False))
        return 0
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
