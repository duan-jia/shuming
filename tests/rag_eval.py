#!/usr/bin/env python3
"""Retrieve and score the current 100-query paper dataset with honest unjudged bounds."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
from eval_common import Client, write_report

DATASET = Path(__file__).parent / 'datasets/recent-papers-v1'


def jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def load_mapping(path, manifest):
    papers = json.loads(path.read_text())['papers']
    expected = {d['id']: d['pdf_sha256'] for d in manifest}
    if len(papers) != len(expected) or {p['paper_id'] for p in papers} != set(expected):
        raise ValueError('Document map must bind each dataset paper exactly once')
    mapping = {}
    for paper in papers:
        doc_id = paper['doc_id']
        if type(doc_id) is not int or doc_id < 1 or doc_id in mapping:
            raise ValueError('Document IDs must be unique positive integers')
        if paper['pdf_sha256'] != expected[paper['paper_id']]:
            raise ValueError('Document map PDF hash differs from pinned corpus')
        mapping[doc_id] = paper['paper_id']
    return mapping


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, default=DATASET)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--base-url', help='Backend URL, e.g. http://127.0.0.1:9007')
    source.add_argument('--predictions', type=Path, help='JSONL: {query_id, doc_ids: [logical paper IDs]}')
    parser.add_argument('--document-map', type=Path, help='Live runs require papers with paper_id, doc_id, pdf_sha256')
    parser.add_argument('--k', nargs='+', type=int, default=[1, 3, 5, 10])
    parser.add_argument('--mode', choices=['parents', 'chunks'], default='parents')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--interval', type=float, default=0, help='Minimum seconds between live queries')
    parser.add_argument('--exclude-document-id', type=int, action='append', default=[], help='Explicit non-corpus distractor; exclusions and original ranks are recorded')
    parser.add_argument('--output', default='.tmp/evaluation/rag.json')
    parser.add_argument('--checkpoint', type=Path, help='Persist one query outcome per line during a live run')
    parser.add_argument('--progress-every', type=int, default=0)
    args = parser.parse_args()
    if any(k < 1 or k > 20 for k in args.k):
        parser.error('k must be between 1 and 20')
    if args.base_url and not args.document_map:
        parser.error('Live evaluation requires --document-map from the pinned corpus import')
    if args.interval < 0:
        parser.error('interval must be nonnegative')
    runs, errors, timings, raw_rankings, exclusions, retries = [], [], {}, [], [], []
    if args.base_url:
        mapping = load_mapping(args.document_map, jsonl(args.dataset / 'corpus/manifest.jsonl'))
        excluded = set(args.exclude_document_id)
        if excluded & set(mapping):
            parser.error('Cannot exclude a document belonging to the pinned corpus')
        request_k = min(20, max(args.k) + len(excluded))
        client = Client(args.base_url, args.timeout)
        previous_start = 0.0
        if args.checkpoint:
            args.checkpoint.parent.mkdir(parents=True, exist_ok=True)
            args.checkpoint.write_text('')
        for query in jsonl(args.dataset / 'rag/queries.jsonl'):
            delay = args.interval - (time.monotonic() - previous_start)
            if delay > 0:
                time.sleep(delay)
            started = previous_start = time.monotonic()
            try:
                for attempt in range(7):
                    rpc = client.json('/mcp/knowledge', {'jsonrpc': '2.0', 'id': query['id'], 'method': 'tools/call',
                    'params': {'name': 'search_knowledge', 'arguments': {
                        'query': query['query'], 'top_k': request_k, 'mode': args.mode}}},
                    headers={'MCP-Protocol-Version': '2025-11-25', 'Accept': 'application/json, text/event-stream'})
                    limited = rpc.get('result', {}).get('isError') is True and 'rate limit' in json.dumps(rpc['result'].get('content', [])).lower()
                    if not limited or attempt == 6:
                        break
                    retries.append({'query_id': query['id'], 'reason': 'server_rate_limit', 'attempt': attempt+1})
                    time.sleep(10)
                if 'error' in rpc or rpc.get('result', {}).get('isError'):
                    raise ValueError('MCP search returned an error')
                items = rpc['result']['structuredContent']['results']
                raw_rankings.append({'query_id': query['id'], 'document_ids': [item['doc_id'] for item in items]})
                selected = []
                for rank, item in enumerate(items, 1):
                    if item['doc_id'] in excluded:
                        exclusions.append({'query_id': query['id'], 'doc_id': item['doc_id'], 'original_rank': rank,
                                           'reason': 'explicitly_outside_frozen_corpus'})
                        continue
                    selected.append(mapping[item['doc_id']])
                # This is a declared closed-corpus ranking; unknown IDs still fail.
                # Repeated chunks remain in their original relative rank slots.
                doc_ids = selected[:max(args.k)]
                runs.append({'query_id': query['id'], 'doc_ids': doc_ids})
            except Exception as error:
                errors.append({'query_id': query['id'], 'error': type(error).__name__})
                runs.append({'query_id': query['id'], 'doc_ids': []})
            timings[query['id']] = time.monotonic() - started
            if args.checkpoint:
                with args.checkpoint.open('a') as checkpoint:
                    checkpoint.write(json.dumps({'ranking': runs[-1], 'elapsed_seconds': timings[query['id']],
                        'errors': [e for e in errors if e['query_id'] == query['id']]}) + '\n')
            if args.progress_every and len(runs) % args.progress_every == 0:
                print('RAG progress', len(runs), 'errors', len(errors), flush=True)
    with tempfile.TemporaryDirectory() as directory:
        rankings = args.predictions.resolve() if args.predictions else Path(directory) / 'rankings.jsonl'
        if not args.predictions:
            rankings.write_text(''.join(json.dumps(row) + '\n' for row in runs))
        result = subprocess.run(['node', str(args.dataset / 'scripts/score-rag.mjs'), str(rankings), *map(str, args.k)],
                                check=True, capture_output=True, text=True)
    report = json.loads(result.stdout)
    report['rankings'] = jsonl(args.predictions) if args.predictions else runs
    report.update({'source': 'live' if args.base_url else 'offline', 'mode': args.mode,
        'dataset_version': json.loads((args.dataset / 'dataset.json').read_text())['version'],
        'dataset_hashes': {name: hashlib.sha256((args.dataset / name).read_bytes()).hexdigest()
                          for name in ('corpus/manifest.jsonl', 'rag/queries.jsonl', 'rag/qrels.jsonl')},
        'errors': errors, 'elapsed_seconds': timings, 'raw_rankings': raw_rankings,
        'excluded_document_ids': args.exclude_document_id, 'exclusions': exclusions, 'retries': retries,
        'query_interval_seconds': args.interval,
        'scope': 'pinned_corpus_with_explicit_logged_exclusions' if args.exclude_document_id else 'unfiltered_backend'})
    if args.document_map:
        report['document_map_sha256'] = hashlib.sha256(args.document_map.read_bytes()).hexdigest()
    report['summary'] = {'queries': report['queries'], 'submitted': report['submitted'],
                         'missing': report['missing_count'], 'errors': len(errors), 'macro': report['macro']}
    write_report(args.output, report)
    return 1 if errors or report['missing_count'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
