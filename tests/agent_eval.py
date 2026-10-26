#!/usr/bin/env python3
"""Score normalized Agent traces using the current 30-task dataset.

Tool names alone in SSE cannot establish argument/evidence correctness. Supply
normalized calls with arguments/results; no hidden task labels are sent to models.
"""
import argparse
import json
from pathlib import Path
import subprocess
from eval_common import write_report

DATASET = Path(__file__).parent / 'datasets/recent-papers-v1'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, default=DATASET)
    parser.add_argument('--predictions', type=Path, required=True, help='Normalized trace JSONL with task_id, calls, answer')
    parser.add_argument('--output', default='.tmp/evaluation/agent.json')
    args = parser.parse_args()
    result = subprocess.run(['node', str(args.dataset / 'scripts/score-agent.mjs'), str(args.predictions.resolve())],
                            check=True, capture_output=True, text=True)
    report = json.loads(result.stdout)
    report['dataset_version'] = json.loads((args.dataset / 'dataset.json').read_text())['version']
    report['source'] = 'supplied_normalized_traces'
    report['summary'] = {k: report[k] for k in ('total_tasks', 'submitted', 'missing', 'mechanical_pass_rate_with_missing_as_zero')}
    write_report(args.output, report)
    return 0 if not report['missing'] and all(r['mechanical_pass'] for r in report['results']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
