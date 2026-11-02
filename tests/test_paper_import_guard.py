import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from paper_card import markdown_for
from paper_import_guard import (
    require_summary_markdown, validate_duplicate_summary, validate_generated_summary,
)


class PaperImportGuardTest(unittest.TestCase):
    def setUp(self):
        self.card = {
            "schema_version": "paper_card.v1", "source_sha256": "a" * 64,
            "model": "fixture-model", "title": "Paper", "authors": [],
            "venue_and_year": "", "keywords": [],
            "problem_and_context": "The problem.",
            "core_idea_and_approach": "The method.",
            "key_takeaways": ["The finding."],
            "scope_and_limitations": "The scope.",
        }
        self.content = markdown_for(self.card)

    def test_generated_card_matches_pdf_and_metadata(self):
        validate_generated_summary(self.content, self.card, "a" * 64)
        renamed = self.content.replace("# Paper\n", "# Corpus Paper Title\n", 1)
        validate_generated_summary(renamed, self.card, "a" * 64)

    def test_wrong_source_and_non_model_metadata_are_rejected(self):
        for changes in ({"source_sha256": "b" * 64},
                        {"schema_version": "corpus_import.v1"}, {"model": ""}):
            with self.subTest(changes=changes), self.assertRaisesRegex(RuntimeError, "provenance"):
                validate_generated_summary(self.content, dict(self.card, **changes), "a" * 64)

    def test_markdown_cannot_be_swapped_under_valid_metadata(self):
        changed = self.content.replace("The method.", "A different method.")
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            validate_generated_summary(changed, self.card, "a" * 64)

    def test_duplicate_full_text_is_not_reused(self):
        full_text = "# Paper\n\n## Paper | PDF page 1 | block 1\nOriginal paper."
        with self.assertRaisesRegex(RuntimeError, "document 29.*no valid summary"):
            validate_duplicate_summary({"id": 29, "content": full_text}, self.content)
        with self.assertRaisesRegex(RuntimeError, "not extracted PDF text"):
            require_summary_markdown(self.content + "\n" + full_text)

    def test_missing_or_empty_summary_is_not_reused(self):
        for value in (None, "", self.content.replace("The scope.", "")):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                validate_duplicate_summary({"id": 29, "content": value}, self.content)

    def test_matching_duplicate_is_reused_with_line_ending_tolerance(self):
        validate_duplicate_summary(
            {"id": 29, "content": self.content.replace("\n", "\r\n")}, self.content
        )

    def test_different_summary_is_not_silently_overwritten(self):
        original = {"id": 29, "content": self.content.replace("The method.", "Old method.")}
        with self.assertRaisesRegex(RuntimeError, "different summary"):
            validate_duplicate_summary(original, self.content)
        self.assertIn("Old method.", original["content"])


if __name__ == "__main__":
    unittest.main()
