"""Exercise the live-model harness with local fake replies, never a paid API."""
import json
from pathlib import Path
import tempfile
import unittest

from agent_model_eval import DATASET, execute_task, jsonl, tools_for


class FakeModel:
    def __init__(self):
        self.requests = []

    def complete(self, messages, tools, max_tokens):
        self.requests.append(json.loads(json.dumps(messages)))
        count = sum(m['role'] == 'tool' for m in messages)
        call = {'id': f'call-{count}', 'type': 'function', 'function': {
            'name': 'search_knowledge' if count == 0 else 'fetch_knowledge',
            'arguments': json.dumps({'query': 'UCCL'} if count == 0 else {'doc_id': 1001})}}
        message = {'role': 'assistant', 'content': None, 'tool_calls': [call]} if count < 2 else {'role': 'assistant', 'content': 'UCCL控制路径在主机CPU执行。'}
        return {'model': 'local-fake', 'choices': [{'message': message, 'finish_reason': 'tool_calls' if count < 2 else 'stop'}],
                'usage': {'prompt_tokens': 10, 'completion_tokens': 2}}


class HarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tasks = {t['id']: t for t in jsonl(DATASET / 'agent/tasks.jsonl')}
        cls.fixtures = {f['id']: f for f in jsonl(DATASET / 'agent/fixtures.jsonl')}
        cls.contracts = json.loads((DATASET / 'agent/tool-contracts.json').read_text())['tools']

    def test_real_tool_loop_records_args_results_and_hides_grader_labels(self):
        model = FakeModel()
        with tempfile.TemporaryDirectory() as directory:
            trace, run = execute_task(self.tasks['agent-001'], self.fixtures['agent-001'], self.contracts,
                                      model, DATASET, Path(directory))
        self.assertEqual(run['status'], 'completed')
        self.assertEqual([c['name'] for c in trace['calls']], ['search_knowledge', 'fetch_knowledge'])
        self.assertEqual(trace['calls'][1]['arguments']['doc_id'], 1001)
        self.assertTrue(trace['calls'][1]['result']['content'])
        first = json.dumps(model.requests[0])
        for hidden in ('required_tools', 'reference_answer', 'fetch_doc_id_must_come_from_successful_search', 'fixtures'):
            self.assertNotIn(hidden, first)
        self.assertEqual(run['usage']['prompt_tokens'], 30)

    def test_unsupported_image_case_is_explicitly_skipped(self):
        model = FakeModel()
        with tempfile.TemporaryDirectory() as directory:
            trace, run = execute_task(self.tasks['agent-012'], self.fixtures['agent-012'], self.contracts,
                                      model, DATASET, Path(directory), image_inputs=False)
        self.assertEqual(run['status'], 'skipped')
        self.assertEqual(model.requests, [])
        self.assertEqual(trace['calls'], [])

    def test_text_only_schema_does_not_offer_page_images(self):
        tools = tools_for(self.tasks['agent-001'], self.contracts, False)
        fetch = next(t for t in tools if t['function']['name'] == 'fetch_knowledge')
        self.assertEqual(fetch['function']['parameters']['properties']['representation']['enum'], ['source', 'text'])


if __name__ == '__main__':
    unittest.main()
