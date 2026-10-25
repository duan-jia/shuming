// Read-only counterexamples for the frozen recent-papers-v1 grader.
// Usage: node tests/reviews/probe_recent_papers_grader.mjs /path/to/dataset
// Exit 1 means a counterexample still passes the mechanical grader.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const root = path.resolve(process.argv[2] || 'tests/datasets/recent-papers-v1');
const {scoreAgent} = await import(pathToFileURL(path.join(root, 'scripts/grade.mjs')).href);
const read = name => fs.readFileSync(path.join(root, name), 'utf8').trim().split('\n').map(JSON.parse);
const tasks = read('agent/tasks.jsonl');
const fixtures = read('agent/fixtures.jsonl');
const probes = [
    {
        name: 'errored_search_still_supplies_doc_id',
        trace: {task_id: 'agent-001', calls: [
            {name: 'search_knowledge', arguments: {query: 'UCCL'},
                result: {is_error: true, hits: [{doc_id: 1001}]}},
            {name: 'fetch_knowledge', arguments: {doc_id: 1001},
                result: {text_available: true, content: 'synthetic evidence'}}
        ]}
    },
    {
        name: 'wrong_page_image_plus_page4_text',
        trace: {task_id: 'agent-012', calls: [
            {name: 'search_knowledge', arguments: {query: 'ByteScale'}, result: {hits: [{doc_id: 1001}]}},
            {name: 'fetch_knowledge', arguments: {doc_id: 1001, representation: 'pages', start_page: 1, page_count: 1},
                result: {images: [{page: 1, asset: 'synthetic-page1.png'}]}},
            {name: 'fetch_knowledge', arguments: {doc_id: 1001, representation: 'text', start_page: 4, page_count: 1},
                result: {text_available: true, content: 'synthetic text only'}}
        ]}
    }
];
let failures = 0;
for (const probe of probes) {
    const result = scoreAgent(probe.trace, tasks.find(t => t.id === probe.trace.task_id),
        fixtures.find(f => f.id === probe.trace.task_id));
    console.log(JSON.stringify({probe: probe.name, expected_mechanical_pass: false, observed: result}));
    if (result.mechanical_pass) failures++;
}
process.exitCode = failures ? 1 : 0;
