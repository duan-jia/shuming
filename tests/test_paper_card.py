import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from paper_card import build_prompt, markdown_for, normalize_card  # noqa: E402


class PaperCardTest(unittest.TestCase):
    def setUp(self) -> None:
        self.raw = {
            "title": "A Useful Paper",
            "authors": ["Ada Author", "Ben Builder"],
            "venue_and_year": "ExampleConf 2026",
            "keywords": ["retrieval", "reasoning"],
            "problem_and_context": "The paper studies a concrete retrieval problem.",
            "core_idea_and_approach": "It proposes and evaluates a compact mechanism.",
            "key_takeaways": ["The mechanism reduces the relevant bottleneck."],
            "scope_and_limitations": "The evaluation covers one constrained setting.",
            "verification_flags": [],
            "needs_review": False,
            "review_reason": "",
        }

    def normalize(self, raw=None):
        return normalize_card(
            raw or self.raw,
            Path("source.pdf"),
            "abc123",
            "example-model",
            12,
            34_000,
        )

    def test_normalize_adds_provenance(self) -> None:
        card = self.normalize()

        self.assertEqual(card["schema_version"], "paper_card.v1")
        self.assertEqual(card["source_pdf"], "source.pdf")
        self.assertEqual(card["source_sha256"], "abc123")
        self.assertEqual(card["page_count"], 12)
        self.assertFalse(card["needs_review"])

    def test_missing_or_temporary_title_is_rejected(self) -> None:
        for title in [None, "", " \n ", 42, "00000000-0000-0000-0000-000000000001"]:
            with self.subTest(title=title):
                raw = dict(self.raw, title=title)
                with self.assertRaisesRegex(RuntimeError, "title could not be identified"):
                    self.normalize(raw)

    def test_multiline_title_is_joined_for_filename(self) -> None:
        card = self.normalize(dict(self.raw, title="  A Useful\nPaper\tTitle  "))
        self.assertEqual(card["title"], "A Useful Paper Title")

    def test_markdown_uses_concise_four_section_structure(self) -> None:
        markdown = markdown_for(self.normalize())

        expected_headings = [
            "## Problem and Context",
            "## Core Idea and Approach",
            "## Key Takeaways",
            "## Scope and Limitations",
        ]
        self.assertTrue(all(heading in markdown for heading in expected_headings))
        self.assertNotIn("## Summary", markdown)
        self.assertNotIn("Citation key", markdown)

    def test_missing_required_content_is_rejected(self) -> None:
        raw = dict(self.raw)
        raw["problem_and_context"] = ""

        with self.assertRaisesRegex(RuntimeError, "problem_and_context"):
            self.normalize(raw)

    def test_unknown_metadata_is_marked_for_verification(self) -> None:
        raw = dict(self.raw)
        raw["authors"] = []
        raw["venue_and_year"] = ""
        markdown = markdown_for(self.normalize(raw))

        self.assertIn("[VERIFY: authors unavailable", markdown)
        self.assertIn("[VERIFY: venue and year unavailable", markdown)

    def test_verification_flags_are_kept_out_of_markdown(self) -> None:
        raw = dict(self.raw)
        raw["verification_flags"] = ["[VERIFY: confirm an internal discrepancy]"]
        card = self.normalize(raw)

        self.assertEqual(card["verification_flags"], raw["verification_flags"])
        self.assertNotIn("Verification Notes", markdown_for(card))
        self.assertNotIn(raw["verification_flags"][0], markdown_for(card))

    def test_prompt_requires_source_grounding(self) -> None:
        messages = build_prompt("source.pdf", "paper text")
        prompt_text = "\n".join(message["content"] for message in messages)

        self.assertIn("Base the card only on the supplied paper text", prompt_text)
        self.assertIn("problem_and_context", prompt_text)
        self.assertIn("600-1,000 English words", prompt_text)
        self.assertIn("needs_review", prompt_text)

    def test_invalid_review_decision_is_sent_to_review(self) -> None:
        raw = dict(self.raw)
        raw["needs_review"] = "false"

        card = self.normalize(raw)

        self.assertTrue(card["needs_review"])
        self.assertTrue(card["review_reason"])

    def test_incomplete_suspicious_document_can_enter_review(self) -> None:
        raw = dict(self.raw)
        raw["needs_review"] = True
        raw["review_reason"] = "The input is not a complete paper."
        raw["problem_and_context"] = ""

        card = self.normalize(raw)

        self.assertTrue(card["needs_review"])
        self.assertIn("[REVIEW:", card["problem_and_context"])


if __name__ == "__main__":
    unittest.main()
