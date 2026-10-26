import fs from 'node:fs';import {jsonl} from './common.mjs';import {scoreAgent} from './grade.mjs';
if(!process.argv[2])throw Error('Usage: node scripts/score-agent.mjs /path/to/normalized-traces.jsonl');
const tasks=jsonl('agent/tasks.jsonl'),fixtures=jsonl('agent/fixtures.jsonl');const text=fs.readFileSync(process.argv[2],'utf8').trim();const rows=text?text.split('\n').map(JSON.parse):[];const seen=new Set();
const results=rows.map(r=>{const t=tasks.find(t=>t.id===r.task_id);if(!t||seen.has(r.task_id))throw Error('Unknown or duplicate task');seen.add(r.task_id);return scoreAgent(r,t,fixtures.find(f=>f.id===t.fixture_id));});
console.log(JSON.stringify({scope:'mechanical_trace_checks_not_overall_agent_quality',total_tasks:tasks.length,submitted:rows.length,missing:tasks.filter(t=>!seen.has(t.id)).map(t=>t.id),mechanical_pass_rate_with_missing_as_zero:results.filter(r=>r.mechanical_pass).length/tasks.length,semantic_review_required:true,results},null,2));
