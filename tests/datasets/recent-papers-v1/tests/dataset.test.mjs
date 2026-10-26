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
test('image fixture accepts omitted count and explicit legal ranges',()=>{for(const count of [undefined,1,2,3]){const args={doc_id:1001,representation:'pages',start_page:4,...(count===undefined?{}:{page_count:count})};const r=resolveFixture(f('agent-012'),'fetch_knowledge',args);assert.deepEqual(r.result.images.map(x=>x.page),Array.from({length:count??3},(_,i)=>4+i));}});
test('offset read timeout or missing result cannot pass; successful retry can',()=>{const id='agent-009',calls=[call(id,'search_knowledge',{query:'UCCL'}),call(id,'fetch_knowledge',{doc_id:1001,start_page:1,page_count:1,offset:0}),call(id,'fetch_knowledge',{doc_id:1001,start_page:1,page_count:1,offset:120})];for(const failure of [{error:{code:'upstream_timeout'}},{}]){const b={task_id:id,calls:structuredClone(calls)};delete b.calls[2].result;Object.assign(b.calls[2],failure);assert(!scoreAgent(b,t(id),f(id)).mechanical_pass);}const good={task_id:id,calls};assert(scoreAgent(good,t(id),f(id)).mechanical_pass);const retry=structuredClone(calls[2]);delete retry.result;retry.error={code:'upstream_timeout'};good.calls.splice(2,0,retry);assert(scoreAgent(good,t(id),f(id)).mechanical_pass);});
test('page read timeout cannot pass required pagination',()=>{const id='agent-010',b={task_id:id,calls:[call(id,'search_knowledge',{query:'SHIFT'}),call(id,'fetch_knowledge',{doc_id:1001,start_page:3,page_count:1,offset:0}),call(id,'fetch_knowledge',{doc_id:1001,start_page:4,page_count:1,offset:0})]};assert(scoreAgent(b,t(id),f(id)).mechanical_pass);delete b.calls[2].result;b.calls[2].error={code:'upstream_timeout'};assert(!scoreAgent(b,t(id),f(id)).mechanical_pass);});
test('ordinary required fetch must have successful nonempty evidence',()=>{const b=basic();delete b.calls[1].result;b.calls[1].error={code:'upstream_timeout'};assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);});
test('Swift requirements answer, mock and body evidence stay aligned',()=>{const q=jsonl('rag/queries.jsonl').find(q=>q.id==='rag-004'),e=jsonl('rag/evidence.jsonl').find(e=>e.id===q.evidence_ids[0]);assert.equal(q.reference_answer,t('agent-002').reference_answer);assert.equal(q.reference_answer,f('agent-002').responses.find(r=>r.name==='fetch_knowledge').result.content);assert.match(q.reference_answer,/100微秒/);assert.match(q.reference_answer,/初始化/);assert.match(q.reference_answer,/占比/);assert.equal(e.pdf_page,4);assert.equal(e.source_section,'3.1');assert.equal(q.evidence_depth,'body');});
test('expected text unavailable task can still mechanically complete',()=>{const id='agent-013',b={task_id:id,calls:[call(id,'search_knowledge',{query:'ByteScale'}),call(id,'fetch_knowledge',{doc_id:1001,representation:'text'})]};assert(scoreAgent(b,t(id),f(id)).mechanical_pass);});
function offsetTrace(){const id='agent-009';return {task_id:id,calls:[call(id,'search_knowledge',{query:'UCCL'}),call(id,'fetch_knowledge',{doc_id:1001,start_page:1,page_count:1,offset:0}),call(id,'fetch_knowledge',{doc_id:1001,start_page:1,page_count:1,offset:120})]};}
function prematurePage(){return {name:'fetch_knowledge',arguments:{doc_id:1001,representation:'text',start_page:2,page_count:1,offset:0},result:{doc_id:1001,text_available:true,content:'Synthetic page 2',next_offset:null,next_page:null}};}
function permutations(items){if(items.length<2)return [items];return items.flatMap((x,i)=>permutations(items.filter((_,j)=>j!==i)).map(rest=>[x,...rest]));}
test('all permutations reject early next-page attempts, not later repaired coverage',()=>{const b=offsetTrace(),reads=[b.calls[1],b.calls[2],prematurePage()];for(const perm of permutations([0,1,2])){const r=scoreAgent({task_id:b.task_id,calls:[b.calls[0],...perm.map(i=>reads[i])]},t(b.task_id),f(b.task_id));assert.equal(r.mechanical_pass,perm.join(',')==='0,1,2',perm.join(','));}});
test('failed or missing early page attempt remains an ordering violation',()=>{for(const failure of [{error:{code:'upstream_timeout'}},{}]){const b=offsetTrace(),early=prematurePage();delete early.result;Object.assign(early,failure);b.calls.splice(2,0,early);assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);}});
test('original first request may retry without advancing its cursor',()=>{const b=offsetTrace(),failed=structuredClone(b.calls[1]);delete failed.result;failed.error={code:'upstream_timeout'};b.calls.splice(1,0,failed);assert(scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);});
test('page pagination enforces each request order and permits current-page retry',()=>{const id='agent-010',search=call(id,'search_knowledge',{query:'SHIFT'}),p3=call(id,'fetch_knowledge',{doc_id:1001,start_page:3,page_count:1,offset:0}),p4=call(id,'fetch_knowledge',{doc_id:1001,start_page:4,page_count:1,offset:0});assert(!scoreAgent({task_id:id,calls:[search,p4,p3]},t(id),f(id)).mechanical_pass);const failed=structuredClone(p4);delete failed.result;failed.error={code:'upstream_timeout'};assert(scoreAgent({task_id:id,calls:[search,p3,failed,p4]},t(id),f(id)).mechanical_pass);assert(!scoreAgent({task_id:id,calls:[search,failed,p3,p4]},t(id),f(id)).mechanical_pass);});
function webTrace(id){return {task_id:id,calls:f(id).responses.map(r=>call(id,r.name,{...(r.name.startsWith('search_')?{query:'paper'}:{}),...r.match})),answer:'fixture answer'};}
test('all webpage-evidence tasks require successful nonempty reads',()=>{for(const id of ['agent-014','agent-015','agent-019','agent-020']){const good=webTrace(id);assert(scoreAgent(good,t(id),f(id)).mechanical_pass,id);const wrongUrl=structuredClone(good);wrongUrl.calls.find(c=>c.name==='fetch_web').arguments.url='https://unrelated.invalid/paper';assert(!scoreAgent(wrongUrl,t(id),f(id)).mechanical_pass,id+' wrong URL');for(const bad of [{error:{code:'upstream_timeout'}},{},{result:{}},{result:{content:''}},{result:{content:' \n '}},{result:{content:123}},{result:{is_error:true,content:'not evidence'}}]){const trace=structuredClone(good),at=trace.calls.findIndex(c=>c.name==='fetch_web');const original=trace.calls[at];trace.calls[at]={name:original.name,arguments:original.arguments,...bad};assert(!scoreAgent(trace,t(id),f(id)).mechanical_pass,id+' '+JSON.stringify(bad));}}});
test('webpage timeout followed by successful read may recover within budget',()=>{for(const id of ['agent-014','agent-015','agent-019','agent-020']){const trace=webTrace(id),at=trace.calls.findIndex(c=>c.name==='fetch_web'),failed=structuredClone(trace.calls[at]);delete failed.result;failed.error={code:'upstream_timeout'};trace.calls.splice(at,0,failed);assert(scoreAgent(trace,t(id),f(id)).mechanical_pass,id);}});
test('expected persistent search timeouts do not require webpage evidence',()=>{for(const id of ['agent-017','agent-025']){const r=f(id).responses[0],trace={task_id:id,calls:[call(id,r.name,{query:'paper'})]};assert(scoreAgent(trace,t(id),f(id)).mechanical_pass,id);}});
test('contradictory cursors cannot advance or be hidden by later success',()=>{for(const cursor of [0,119,'120',null]){const b=offsetTrace(),wrong=structuredClone(b.calls[1]);wrong.result.next_offset=cursor;b.calls.splice(1,0,wrong);assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass,String(cursor));}});

test('error flags on knowledge search cannot authorize a later document read',()=>{
 for(const failure of [{is_error:true},{isError:true},{error:{code:'failed'}}]) {
  const b=basic();Object.assign(b.calls[0].result,failure);
  assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);
 }
 for(const failure of [{is_error:true},{isError:true},{error:{code:'failed'}}]) {
  const b=basic();Object.assign(b.calls[0],failure);
  assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);
 }
});
test('an invalid search cannot provide IDs or URLs even if its payload contains hits',()=>{
 const b=basic();b.calls[0].arguments.query='';
 assert(!scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);
 const id='agent-014';const b2={task_id:id,calls:[call(id,'search_web',{query:'Hopper'}),call(id,'fetch_web',{url:f(id).responses.find(r=>r.name==='fetch_web').match.url})]};
 // Supply a URL directly, while marking its originating search as failed.
 const url=b2.calls[1].arguments.url;b2.calls[0].result={is_error:true,results:[{url}]};
 assert(!scoreAgent(b2,t(id),f(id)).mechanical_pass);
});
test('failed search followed by a valid retry can provide evidence',()=>{
 const b=basic();const failed=structuredClone(b.calls[0]);failed.result.is_error=true;b.calls.unshift(failed);
 assert(scoreAgent(b,t(b.task_id),f(b.task_id)).mechanical_pass);
});
test('page 4 image must be in the same successful call, not page 1 image plus page 4 text',()=>{
 const id='agent-012',b={task_id:id,calls:[call(id,'search_knowledge',{query:'ByteScale'}),
  {name:'fetch_knowledge',arguments:{doc_id:1001,representation:'pages',start_page:1,page_count:1},result:{images:[{page:1,asset:'synthetic.png'}]}},
  {name:'fetch_knowledge',arguments:{doc_id:1001,representation:'text',start_page:4,page_count:1},result:{content:'text only',text_available:true}}]};
 assert(!scoreAgent(b,t(id),f(id)).mechanical_pass);
 for(const count of [undefined,1,2,3]) {
  b.calls=[b.calls[0],call(id,'fetch_knowledge',{doc_id:1001,representation:'pages',start_page:4,...(count===undefined?{}:{page_count:count})})];
  assert(scoreAgent(b,t(id),f(id)).mechanical_pass);
 }
});
test('wrong document, returned page, missing image or error cannot satisfy image requirement',()=>{
 const id='agent-012',good={task_id:id,calls:[call(id,'search_knowledge',{query:'ByteScale'}),call(id,'fetch_knowledge',{doc_id:1001,representation:'pages',start_page:4,page_count:1})]};
 for(const mutate of [c=>{c.result.doc_id=1002},c=>{c.result.images[0].page=1},c=>{c.result.images=[]},c=>{c.result.isError=true}]) {
  const b=structuredClone(good);mutate(b.calls[1]);assert(!scoreAgent(b,t(id),f(id)).mechanical_pass);
 }
});
test('additional Ofan positive is referenced by question, qrels and evidence',()=>{
 const q=jsonl('rag/queries.jsonl').find(q=>q.id==='rag-084');
 const r=jsonl('rag/qrels.jsonl').find(r=>r.query_id===q.id&&r.doc_id==='ofan');
 assert(r);assert(r.evidence_ids.every(id=>q.evidence_ids.includes(id)));
 assert.equal(jsonl('rag/evidence.jsonl').find(e=>e.id===r.evidence_ids[0]).pdf_page,2);
});
test('unjudged retrievals produce bounds instead of fabricated exact precision or recall',()=>{
 const q=[{id:'x',qrels_complete:false}],rels=[{query_id:'x',doc_id:'uccl',relevance:2}];
 const r=scoreRag([{query_id:'x',doc_ids:['uccl','pcm']}],q,rels,[3]).rows[0].scores[3];
 assert.equal(r.precision,null);assert.equal(r.recall,null);assert.equal(r.precision_lower_bound,1/3);assert.equal(r.precision_upper_bound,2/3);assert.equal(r.known_positive_recall,1);
 const judged=scoreRag([{query_id:'x',doc_ids:['uccl','pcm']}],q,[...rels,{query_id:'x',doc_id:'pcm',relevance:0}],[3]);
 assert.equal(judged.macro[3].precision,1/3);assert.equal(judged.macro[3].recall,null);
});
test('exact recall requires full explicit judgments; positives are not silently declared complete',()=>{
 const docs=jsonl('corpus/manifest.jsonl'),q=[{id:'x',qrels_complete:true}];
 const rels=docs.map(d=>({query_id:'x',doc_id:d.id,relevance:d.id==='uccl'?2:0}));
 assert.equal(scoreRag([{query_id:'x',doc_ids:['uccl']}],q,rels,[1]).macro[1].recall,1);
 assert.throws(()=>scoreRag([],q,rels.slice(1),[1]));
 assert.throws(()=>scoreRag([],q,rels,[0]));
});

test('closed-corpus review explicitly judges every query against every paper',()=>{
 const qs=jsonl('rag/queries.jsonl'),docs=jsonl('corpus/manifest.jsonl'),rels=jsonl('rag/qrels.jsonl');
 assert.equal(rels.length,qs.length*docs.length);
 assert.equal(rels.filter(r=>r.relevance>0).length,122);
 assert.equal(rels.filter(r=>r.relevance===0).length,2878);
 for(const q of qs){assert(q.qrels_complete);const rows=rels.filter(r=>r.query_id===q.id);assert.deepEqual(new Set(rows.map(r=>r.doc_id)),new Set(docs.map(d=>d.id)));}
});
test('complete review produces exact macro P/R and no unjudged bounds gap',()=>{
 const qs=jsonl('rag/queries.jsonl'),rels=jsonl('rag/qrels.jsonl');
 const runs=qs.map(q=>({query_id:q.id,doc_ids:rels.filter(r=>r.query_id===q.id&&r.relevance>0).map(r=>r.doc_id)}));
 const report=scoreRag(runs);
 assert.equal(report.macro[10].recall,1);assert.equal(report.macro[10].unjudged_documents,0);
 assert.equal(report.macro[10].precision_lower_bound,report.macro[10].precision_upper_bound);
 assert.equal(report.macro[10].precision,report.macro[10].precision_lower_bound);
 const wrong=scoreRag([{query_id:'rag-006',doc_ids:['pcm']}]);
 assert.equal(wrong.rows.find(r=>r.query_id==='rag-006').scores[1].precision,0);
});
test('namesakes and generic topic overlap are explicitly negative',()=>{
 const rels=jsonl('rag/qrels.jsonl'),grade=(q,d)=>rels.find(r=>r.query_id===q&&r.doc_id===d).relevance;
 assert.equal(grade('rag-004','pcm'),0); // Swift CC is not the elastic RDMA control-plane system.
 assert.equal(grade('rag-061','charllm'),0); // Hopper GPU architecture is not Hopper path selection.
 assert.equal(grade('rag-014','lemix'),0); // Training Varuna is not the RDMA failover paper.
 assert.equal(grade('rag-084','psp'),0); // A constant in a threshold heuristic is not O(1) queue scaling.
 assert.equal(grade('rag-011','rdmacell'),1); // Actual partial answer evidence, not just a keyword.
 assert.equal(grade('rag-073','spritz'),1);
});
