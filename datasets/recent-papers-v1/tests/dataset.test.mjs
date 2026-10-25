import test from 'node:test';import assert from 'node:assert/strict';
import {validateDataset} from '../scripts/validate.mjs';import {jsonl} from '../scripts/common.mjs';import {checkArgs,scoreAgent,scoreRag} from '../scripts/grade.mjs';import {resolveFixture} from '../scripts/mock.mjs';
const tasks=jsonl('agent/tasks.jsonl'),fixtures=jsonl('agent/fixtures.jsonl');
const t=id=>tasks.find(t=>t.id===id),f=id=>fixtures.find(f=>f.id===id);
function call(id,name,arguments_){const r=resolveFixture(f(id),name,arguments_);return {name,arguments:arguments_,...(r.result?{result:r.result}:{error:r.error})};}
function basic(id='agent-001'){return {task_id:id,calls:[call(id,'search_knowledge',{query:'UCCL'}),call(id,'fetch_knowledge',{doc_id:1001,representation:'text'})],answer:'控制路径在主机CPU。'};}
test('dataset reference consistency and frozen cardinalities',()=>assert.equal(validateDataset().review_records,130));
test('tool argument limits and unknown fields',()=>{assert(checkArgs('search_web',{query:'q',count:51}).length);assert(checkArgs('fetch_knowledge',{doc_id:1,page_count:4}).length);assert(checkArgs('search_knowledge',{query:'q',tenant_id:2}).length);assert(checkArgs('fetch_web',{url:3}).length);});
test('Unicode limits count characters, not UTF-16 code units',()=>assert.equal(checkArgs('search_web',{query:'研'.repeat(128)}).length,0));
test('normalized mock fills text representation default',()=>assert(resolveFixture(f('agent-001'),'fetch_knowledge',{doc_id:1001}).result));
test('mock refuses nonexistent doc ID',()=>assert.throws(()=>resolveFixture(f('agent-001'),'fetch_knowledge',{doc_id:999})));
test('successful mechanical trace still requires semantic review',()=>{const r=scoreAgent(basic(),t('agent-001'),f('agent-001'));assert(r.mechanical_pass);assert(r.semantic_review_required);assert(!('overall_pass' in r));});
test('fetch before search is rejected',()=>{const b=basic();b.calls.reverse();assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);});
test('forbidden web tool detected',()=>{const b=basic();b.calls.push({name:'search_web',arguments:{query:'q'},result:{results:[]}});assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);});
test('cross-document task cannot pass with one fetched paper',()=>{const b=basic('agent-007');assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);});
test('permission denial is expected, not a required successful fetch',()=>{const id='agent-022';const b={task_id:id,calls:[call(id,'search_knowledge',{query:'UCCL'}),call(id,'fetch_knowledge',{doc_id:1001})]};assert(scoreAgent(b,t(id),f(id)).mechanical_pass);b.calls.push(b.calls[1]);assert(!scoreAgent(b,t(id),f(id)).mechanical_pass);});
test('offset pagination preserves page range and follows cursor',()=>{const id='agent-009',b={task_id:id,calls:[call(id,'search_knowledge',{query:'UCCL'}),call(id,'fetch_knowledge',{doc_id:1001,start_page:1,page_count:1,offset:0}),call(id,'fetch_knowledge',{doc_id:1001,start_page:1,page_count:1,offset:120})]};assert(scoreAgent(b,t(id),f(id)).mechanical_pass);b.calls[2].arguments.start_page=2;assert(!scoreAgent(b,t(id),f(id)).mechanical_pass);});
test('image calls refused for text-only capabilities',()=>{const b=basic('agent-013');b.calls[1].arguments.representation='pages';assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);});
test('RAG duplicate hits waste slots and do not inflate recall',()=>{const q=[{id:'x'}],rels=[{query_id:'x',doc_id:'uccl',relevance:2},{query_id:'x',doc_id:'pcm',relevance:2}];const r=scoreRag([{query_id:'x',doc_ids:['uccl','uccl','pcm']}],q,rels,[2]);assert.equal(r.macro[2].known_positive_recall,0.5);});
test('missing retrieval runs count as zero, not excluded',()=>{const r=scoreRag([], [{id:'x'}],[{query_id:'x',doc_id:'uccl',relevance:2}],[1]);assert.equal(r.missing_count,1);assert.equal(r.macro[1].known_positive_recall,0);});
test('RAG rejects unknown paper IDs and duplicate query results',()=>{assert.throws(()=>scoreRag([{query_id:'rag-001',doc_ids:['unknown']}])) ;assert.throws(()=>scoreRag([{query_id:'rag-001',doc_ids:[]},{query_id:'rag-001',doc_ids:[]}])) ;});
test('source and figure tasks have populated reproducible fixtures',()=>{assert(f('agent-011').responses.some(r=>r.result?.download_url));assert.equal(f('agent-012').setup.required_local_assets[0],'pages/bytescale-p4.png');});
