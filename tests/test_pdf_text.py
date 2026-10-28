import concurrent.futures
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from pdf_text import extract_pages, VERSION
from native_mcp_e2e import fixture_pdf


class PdfTextTests(unittest.TestCase):
    def test_complete_page_text_and_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'paper.pdf'
            source.write_bytes(fixture_pdf())
            first = extract_pages(source, root / 'cache')
            self.assertEqual(first['page_count'], 4)
            self.assertIn('row 59', first['pages'][0]['text'])
            self.assertEqual(first['pages'][2]['text'], '')
            self.assertIn('page 4', first['pages'][3]['text'])
            self.assertEqual(first['extractor_version'], VERSION)
            with patch('pdf_text.subprocess.run', side_effect=AssertionError('must reuse cache')):
                self.assertEqual(extract_pages(source, root / 'cache'), first)
            artifact = next((root / 'cache').glob('*.pages.json'))
            artifact.write_text('{broken')
            self.assertEqual(extract_pages(source, root / 'cache'), first)

    def test_concurrent_requests_extract_once(self):
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'paper.pdf'; source.write_bytes(fixture_pdf())
            original = subprocess.run
            with patch('pdf_text.subprocess.run', wraps=original) as run:
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    results = list(pool.map(lambda _: extract_pages(source, root / 'cache'), range(4)))
            self.assertTrue(all(result == results[0] for result in results))
            self.assertEqual(run.call_count, 2)  # One pdfinfo and one pdftotext.
            self.assertEqual(len(list((root / 'cache').glob('*.pages.json'))), 1)


if __name__ == '__main__':
    unittest.main()
