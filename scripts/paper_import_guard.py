"""Validation for importing model paper cards, including duplicate PDFs.

This module performs no API calls or writes. A duplicate PDF is reusable only
when its stored body is the expected summary, rather than an extracted source.
"""

from __future__ import annotations

import re
from typing import Any, Mapping

from paper_card import markdown_for


def _normalized(content: str) -> str:
    return content.replace("\r\n", "\n").strip()


def require_summary_markdown(content: str) -> None:
    if not isinstance(content, str) or re.search(
        r"^## .*\| PDF page \d+ \| block \d+", content, flags=re.MULTILINE
    ):
        raise RuntimeError("Expected a model paper summary, not extracted PDF text.")
    sections = {}
    for section in re.split(r"^## ", content, flags=re.MULTILINE)[1:]:
        title, _, body = section.partition("\n")
        sections[title.strip()] = body.strip()
    if not all(sections.get(title) for title in (
        "Problem and Context", "Core Idea and Approach",
        "Key Takeaways", "Scope and Limitations",
    )):
        raise RuntimeError("Paper summary is missing required nonempty sections.")


def validate_generated_summary(content: str, metadata: Mapping[str, Any],
                               source_sha256: str) -> None:
    require_summary_markdown(content)
    if (metadata.get("schema_version") != "paper_card.v1"
            or metadata.get("source_sha256") != source_sha256
            or not isinstance(metadata.get("model"), str)
            or not metadata["model"].strip()):
        raise RuntimeError("Paper summary provenance does not match the source PDF.")
    try:
        rendered = markdown_for(dict(metadata))
    except (KeyError, TypeError) as exc:
        raise RuntimeError("Paper summary metadata is incomplete.") from exc
    # Historical imports replace the H1 with the corpus's original title.
    # Every other part must still match the generated JSON card.
    if _normalized(content).partition("\n")[2] != _normalized(rendered).partition("\n")[2]:
        raise RuntimeError("Summary Markdown does not match its generated JSON card.")


def validate_duplicate_summary(document: Mapping[str, Any],
                               expected_content: str) -> None:
    require_summary_markdown(expected_content)
    existing = document.get("content")
    try:
        require_summary_markdown(existing)
    except RuntimeError as exc:
        raise RuntimeError(
            f"Duplicate document {document.get('id', '?')} has no valid summary; "
            "repair its content and retrieval index before importing again."
        ) from exc
    if _normalized(existing) != _normalized(expected_content):
        raise RuntimeError(
            f"Duplicate document {document.get('id', '?')} has a different summary; "
            "review its content and retrieval index before reusing it."
        )
