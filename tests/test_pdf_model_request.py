import io
import json
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from pdf_to_markdown import call_model


class PdfModelRequestTest(unittest.TestCase):
    def test_deepseek_card_requests_reserve_budget_for_json_output(self):
        for endpoint, expected in [
            ('https://api.deepseek.com', {'type': 'disabled'}),
            ('https://api.deepseek.com/v1', {'type': 'disabled'}),
            ('https://example.org/v1', None),
        ]:
            with self.subTest(endpoint=endpoint):
                reply = {'choices': [{'message': {'content': '{"title":"A Paper"}'}}]}
                with patch('urllib.request.urlopen', return_value=io.BytesIO(json.dumps(reply).encode())) as send:
                    result = call_model(endpoint, 'model', 'fixture-key', [], 5)
                body = json.loads(send.call_args.args[0].data)
                self.assertEqual(body.get('thinking'), expected)
                self.assertEqual(body['max_tokens'], 3500)
                self.assertEqual(result['title'], 'A Paper')
