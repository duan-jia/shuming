#!/usr/bin/env python3
"""Build document-governance manifests for lab RAG ingestion."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


SUPPORTED_EXTENSIONS = {".pdf", ".pptx", ".docx", ".md", ".markdown", ".txt"}
ORDINALS = {
    "第一次": "01",
    "第二次": "02",
    "第三次": "03",
    "第四次": "04",
    "第五次": "05",
    "第六次": "06",
    "第七次": "07",
    "第八次": "08",
    "第九次": "09",
    "第十次": "10",
}


@dataclass
class GovernanceRow:
    source_path: str
    relative_path: str
    doc_type: str
    owner: str
    topic: str
    status: str
    action: str
    rag_markdown: str
    metadata_path: str
    quality_score: int
    quality_notes: str
    needs_review: bool
    review_reason: str


@dataclass
class ImportRow:
    source_path: str
    markdown_path: str
    filename: str
    doc_type: str
    owner: str
    topic: str
    visibility: str
    tenant_name: str
    import_source: str


@dataclass
class RenameRow:
    old_path: str
    new_path: str
    doc_type: str
    owner: str
    topic: str
    reason: str
    needs_review: bool


def read_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not path.exists():
        return rows
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row, ensure_ascii=False) + "\n")


def infer_doc_type(path: Path) -> str:
    lower = path.as_posix().lower()
    name = path.name.lower()
    if "paper" in lower or path.parent.name.lower() in {"papers", "paper_pdf"}:
        return "paper"
    if "开题" in name or "proposal" in lower:
        return "proposal"
    if "综述" in name or "survey" in lower:
        return "survey"
    if path.suffix.lower() == ".pptx" or "专题" in name or "seminar" in lower:
        return "seminar"
    if "thesis" in lower or "毕业" in name:
        return "thesis"
    return "document"


def infer_owner(path: Path, root: Path, default_owner: str) -> str:
    rel = path.relative_to(root)
    if len(rel.parts) >= 2 and rel.parts[0] not in {"paper_pdf", "papers"}:
        return safe_slug(rel.parts[0])
    return default_owner


def infer_sequence(name: str) -> str:
    for label, number in ORDINALS.items():
        if label in name:
            return number
    return ""


def infer_title_slug(path: Path, doc_type: str) -> str:
    name = path.name
    if doc_type == "proposal":
        return "opening_report"
    if doc_type == "survey":
        return "literature_review"
    if doc_type == "seminar":
        return "advanced_topic"
    return safe_slug(path.stem)


def safe_slug(value: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_").lower()
    return slug or "unknown"


def rename_target(path: Path, root: Path, doc_type: str, owner: str, topic: str) -> tuple[Path, str, bool]:
    stem = safe_slug(path.stem)
    suffix = path.suffix.lower()
    if doc_type == "paper":
        target_name = f"paper_unknown_{stem}{suffix}"
        return root / "papers" / target_name, "group public paper PDFs under papers/", True

    title_slug = infer_title_slug(path, doc_type)
    sequence = infer_sequence(path.name)
    signed = "_signed" if "电子签" in path.name else ""
    if sequence:
        target_name = f"{doc_type}_{sequence}_{owner}_{title_slug}{signed}{suffix}"
        needs_review = False
    else:
        target_name = f"{doc_type}_unknown_{owner}_{title_slug}{signed}{suffix}"
        needs_review = True
    target = root / "student_work" / owner / target_name
    reason = f"group {doc_type} documents by owner and normalize the filename"
    return target, reason, needs_review


def load_card_index(card_root: Path) -> dict[str, dict[str, Any]]:
    cards: dict[str, dict[str, Any]] = {}
    for row in read_jsonl(card_root / "cards.jsonl"):
        source_path = row.get("source_path")
        if source_path:
            cards[str(Path(source_path).resolve())] = row
    for json_path in card_root.rglob("document_card.json"):
        card = read_json(json_path)
        source_path = card.get("source_path")
        if source_path:
            card["output_markdown"] = str(json_path.with_name("document_card.md"))
            cards[str(Path(source_path).resolve())] = card
    return cards


def source_files(root: Path) -> list[Path]:
    return sorted(
        path
        for path in root.rglob("*")
        if path.is_file()
        and path.suffix.lower() in SUPPORTED_EXTENSIONS
        and "rename_manifest" not in path.name
    )


def build_rows(args: argparse.Namespace) -> tuple[list[GovernanceRow], list[ImportRow], list[RenameRow]]:
    source_root = Path(args.input).resolve()
    parsed_root = Path(args.parsed).resolve()
    card_root = Path(args.document_cards).resolve()
    card_index = load_card_index(card_root)

    governance_rows: list[GovernanceRow] = []
    import_rows: list[ImportRow] = []
    rename_rows: list[RenameRow] = []

    for path in source_files(source_root):
        rel = path.relative_to(source_root)
        parsed_md = parsed_root / rel.with_suffix(".md")
        parsed_meta = parsed_md.with_suffix(".metadata.json")
        metadata = read_json(parsed_meta)
        doc_type = str(metadata.get("doc_type") or infer_doc_type(path))
        owner = str(metadata.get("owner") or infer_owner(path, source_root, "lab"))
        topic = str(metadata.get("topic") or args.topic)
        quality_score = int(metadata.get("quality_score") or 0)
        quality_notes = str(metadata.get("quality_notes") or "")
        parse_status = str(metadata.get("parse_status") or "missing_parse")

        card = card_index.get(str(path.resolve()))
        card_md = Path(str(card.get("output_markdown", ""))) if card else Path()
        if card and card_md.exists():
            status = "ready"
            action = "import_document_card"
            rag_markdown = str(card_md)
            needs_review = False
            review_reason = ""
        else:
            status = "needs_document_card"
            action = "generate_document_card"
            rag_markdown = ""
            needs_review = True
            review_reason = "document_card is required for v1 RAG import"

        governance_rows.append(
            GovernanceRow(
                source_path=str(path),
                relative_path=rel.as_posix(),
                doc_type=doc_type,
                owner=owner,
                topic=topic,
                status=status,
                action=action,
                rag_markdown=rag_markdown,
                metadata_path=str(parsed_meta) if parsed_meta.exists() else "",
                quality_score=quality_score,
                quality_notes=quality_notes,
                needs_review=needs_review,
                review_reason=review_reason,
            )
        )

        if status == "ready" and rag_markdown:
            card_rel = Path(rag_markdown).resolve().relative_to(card_root)
            filename_prefix = args.filename_prefix.strip("/")
            if not filename_prefix:
                if args.visibility == "public":
                    filename_prefix = "public/cards"
                else:
                    filename_prefix = f"tenants/{args.tenant_name}/cards"
            import_rows.append(
                ImportRow(
                    source_path=str(path),
                    markdown_path=rag_markdown,
                    filename=f"{filename_prefix}/{card_rel.as_posix()}",
                    doc_type=doc_type,
                    owner=owner,
                    topic=topic,
                    visibility=args.visibility,
                    tenant_name=args.tenant_name if args.visibility == "group" else "",
                    import_source="document_card",
                )
            )

        target, reason, rename_needs_review = rename_target(path, source_root, doc_type, owner, topic)
        rename_rows.append(
            RenameRow(
                old_path=str(path),
                new_path=str(target),
                doc_type=doc_type,
                owner=owner,
                topic=topic,
                reason=reason,
                needs_review=rename_needs_review,
            )
        )

    return governance_rows, import_rows, rename_rows


def render_markdown(
    governance_rows: list[GovernanceRow],
    import_rows: list[ImportRow],
    rename_rows: list[RenameRow],
) -> str:
    status_counts = Counter(row.status for row in governance_rows)
    type_counts = Counter(row.doc_type for row in governance_rows)
    lines = [
        "# Document Governance Plan",
        "",
        "## Summary",
        "",
        f"- Source files: {len(governance_rows)}",
        f"- Ready for RAG import: {len(import_rows)}",
        f"- Need document cards: {status_counts.get('needs_document_card', 0)}",
        f"- Need review: {sum(1 for row in governance_rows if row.needs_review)}",
        "",
        "## Status Counts",
        "",
    ]
    for status, count in sorted(status_counts.items()):
        lines.append(f"- {status}: {count}")
    lines.extend(["", "## Document Type Counts", ""])
    for doc_type, count in sorted(type_counts.items()):
        lines.append(f"- {doc_type}: {count}")
    lines.extend(["", "## RAG Import Candidates", ""])
    if import_rows:
        for row in import_rows:
            scope = row.visibility if row.visibility == "public" else f"tenant:{row.tenant_name}"
            lines.append(f"- `{row.filename}` <- `{row.markdown_path}` ({scope}, {row.import_source})")
    else:
        lines.append("- None")
    lines.extend(["", "## Items Needing Work", ""])
    work_rows = [row for row in governance_rows if row.status != "ready" or row.needs_review]
    if work_rows:
        for row in work_rows:
            reason = f"; {row.review_reason}" if row.review_reason else ""
            lines.append(f"- `{row.relative_path}`: {row.action}{reason}")
    else:
        lines.append("- None")
    lines.extend(["", "## Rename Suggestions", ""])
    for row in rename_rows:
        review = " review" if row.needs_review else ""
        lines.append(f"- `{row.old_path}` -> `{row.new_path}` ({row.doc_type}{review})")
    lines.append("")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create governance, import, and rename manifests for lab RAG documents."
    )
    parser.add_argument("--input", default="data/tenants/RDMA", help="Source document directory")
    parser.add_argument("--parsed", default="data/parsed/tenants/RDMA", help="Parsed Markdown directory")
    parser.add_argument("--document-cards", default="data/document_cards/tenants/RDMA", help="Document-card output directory")
    parser.add_argument("--paper-cards", dest="document_cards", help=argparse.SUPPRESS)
    parser.add_argument("--output-dir", default="data/governance/tenants/RDMA", help="Manifest output directory")
    parser.add_argument("--topic", default="RDMA", help="Default topic label")
    parser.add_argument("--visibility", choices=["group", "public"], default="group")
    parser.add_argument("--tenant-name", default="RDMA", help="Tenant name for group-visible imports")
    parser.add_argument("--filename-prefix", default="", help="Override RAG filename prefix")
    parser.add_argument("--review-score", type=int, default=80, help="Ready docs below this score need review")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    source_root = Path(args.input)
    if not source_root.exists():
        print(f"Input directory does not exist: {source_root}")
        return 2

    output_dir = Path(args.output_dir)
    governance_rows, import_rows, rename_rows = build_rows(args)
    output_dir.mkdir(parents=True, exist_ok=True)

    write_jsonl(output_dir / "governance_manifest.jsonl", [asdict(row) for row in governance_rows])
    write_jsonl(output_dir / "rag_import_manifest.jsonl", [asdict(row) for row in import_rows])
    write_jsonl(output_dir / "rename_manifest.jsonl", [asdict(row) for row in rename_rows])
    (output_dir / "governance_report.md").write_text(
        render_markdown(governance_rows, import_rows, rename_rows),
        encoding="utf-8",
    )

    status_counts = Counter(row.status for row in governance_rows)
    print(f"Scanned {len(governance_rows)} files")
    print(f"Ready for RAG import: {len(import_rows)}")
    print(f"Need document cards: {status_counts.get('needs_document_card', 0)}")
    print(f"Output: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
