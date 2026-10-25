"""Current paper dataset CLI adapters; metric definitions live in Node grader tests."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch
from urllib.error import HTTPError

from eval_common import Client
from rag_eval import DATASET, jsonl, load_mapping

ROOT = Path(__file__).resolve().parent


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def run_eval(self, script, predictions, *extra):
        source, output = self.directory / 'predictions.jsonl', self.directory / 'output.json'
        source.write_text(''.join(json.dumps(p) + '\n' for p in predictions))
        result = subprocess.run([sys.executable, str(ROOT / script), '--predictions', str(source),
            '--output', str(output), *extra], capture_output=True, text=True, timeout=20)
        self.assertTrue(output.exists(), result.stderr)
        return result, json.loads(output.read_text())

    def test_rag_uses_current_queries_and_additional_positive(self):
        result, report = self.run_eval('rag_eval.py', [{'query_id': 'rag-084', 'doc_ids': ['ofan']}], '--k', '1')
        self.assertEqual(result.returncode, 1)  # Missing 99 queries stay in denominator.
        self.assertEqual(report['queries'], 100)
        self.assertEqual(report['dataset_version'], '0.1.4')
        row = next(r for r in report['rows'] if r['query_id'] == 'rag-084')
        self.assertEqual(row['scores']['1']['known_positive_recall'], .5)
        self.assertEqual(row['scores']['1']['recall'], .5)

    def test_missing_agent_traces_are_zero_not_silent_pass(self):
        result, report = self.run_eval('agent_eval.py', [{'task_id': 'agent-027', 'calls': [], 'answer': 'hello'}])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report['total_tasks'], 30)
        self.assertEqual(len(report['missing']), 29)
        self.assertTrue(report['semantic_review_required'])
        self.assertNotIn('overall_pass', report)

    def test_mapping_requires_exact_pinned_pdfs_and_unique_ids(self):
        manifest = [{'id': 'paper', 'pdf_sha256': 'sha'}]
        target = self.directory / 'map.json'
        good = {'papers': [{'paper_id': 'paper', 'doc_id': 7, 'pdf_sha256': 'sha'}]}
        target.write_text(json.dumps(good))
        self.assertEqual(load_mapping(target, manifest), {7: 'paper'})
        for field, value in [('pdf_sha256', 'other'), ('doc_id', True), ('paper_id', 'wrong')]:
            changed = json.loads(json.dumps(good)); changed['papers'][0][field] = value
            target.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                load_mapping(target, manifest)

    def test_live_mapping_is_required_before_network_access(self):
        result = subprocess.run([sys.executable, str(ROOT / 'rag_eval.py'), '--base-url', 'http://127.0.0.1:1'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn('--document-map', result.stderr)


class LiveRagTests(unittest.TestCase):
    def setUp(self):
        self.requests = []
        requests = self.requests
        state = self.state = {'distractor': False}
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append((self.path, body, dict(self.headers)))
                if self.path == '/redirect':
                    self.send_response(302); self.send_header('Location', '/unexpected'); self.end_headers()
                    return
                # One query fails; every other query returns pinned paper doc_id=100.
                response = {'jsonrpc': '2.0', 'id': body['id'], 'result': {'isError': body['id'] == 'rag-002',
                    'structuredContent': {'results': [{'doc_id': 100, 'content': 'A paper discussing rate limiting.'}]}}}
                if state['distractor']:
                    response['result']['structuredContent']['results'].insert(0, {'doc_id': 1})
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers()
                self.wfile.write(json.dumps(response).encode())
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close); self.addCleanup(self.server.shutdown)
        self.base = f'http://127.0.0.1:{self.server.server_port}'
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def test_live_rankings_are_mapped_and_failed_queries_remain_in_denominator(self):
        manifest = jsonl(DATASET / 'corpus/manifest.jsonl')
        mapping = {'papers': [{'paper_id': d['id'], 'doc_id': 100+i, 'pdf_sha256': d['pdf_sha256']}
                              for i, d in enumerate(manifest)]}
        target, output = self.directory / 'map.json', self.directory / 'report.json'
        target.write_text(json.dumps(mapping))
        result = subprocess.run([sys.executable, str(ROOT / 'rag_eval.py'), '--base-url', self.base,
            '--document-map', str(target), '--output', str(output), '--k', '1', '3'],
            env={**os.environ, 'EMOS_EVAL_TOKEN': 'fixture-token'}, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report['summary']['queries'], 100)
        self.assertEqual(report['summary']['errors'], 1)
        bad = next(r for r in report['rows'] if r['query_id'] == 'rag-002')
        self.assertEqual(bad['scores']['1']['known_positive_recall'], 0)
        self.assertEqual(len(self.requests), 100)
        path, body, headers = self.requests[0]
        self.assertEqual(path, '/mcp/knowledge')
        self.assertEqual(body['params']['arguments']['mode'], 'parents')
        self.assertEqual(headers['Authorization'], 'Bearer fixture-token')
        self.assertNotIn('fixture-token', output.read_text())

    def test_explicit_exclusion_keeps_original_ranks_and_never_hides_unknowns(self):
        self.state['distractor'] = True
        manifest = jsonl(DATASET / 'corpus/manifest.jsonl')
        mapping = {'papers': [{'paper_id': d['id'], 'doc_id': 100+i, 'pdf_sha256': d['pdf_sha256']}
                              for i, d in enumerate(manifest)]}
        target, output = self.directory / 'map.json', self.directory / 'scoped.json'
        target.write_text(json.dumps(mapping))
        result = subprocess.run([sys.executable, str(ROOT / 'rag_eval.py'), '--base-url', self.base,
            '--document-map', str(target), '--exclude-document-id', '1', '--output', str(output), '--k', '1'],
            env={**os.environ, 'EMOS_EVAL_TOKEN': 'fixture-token'}, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stderr)  # One deliberate server error.
        report = json.loads(output.read_text())
        self.assertEqual(report['excluded_document_ids'], [1])
        self.assertEqual(report['raw_rankings'][0]['document_ids'], [1, 100])
        self.assertEqual(report['exclusions'][0]['original_rank'], 1)
        self.assertEqual(report['rankings'][0]['doc_ids'], ['uccl'])
        self.assertEqual(report['rows'][0]['scores']['1']['precision'], 1)
        self.assertEqual(self.requests[0][1]['params']['arguments']['top_k'], 2)

    def test_redirects_do_not_forward_credentials(self):
        with patch.dict(os.environ, {'EMOS_EVAL_TOKEN': 'fixture-token'}):
            with self.assertRaises(HTTPError):
                Client(self.base).json('/redirect', {})
        self.assertEqual([r[0] for r in self.requests], ['/redirect'])


if __name__ == '__main__':
    unittest.main()
