import fs from 'node:fs';import {jsonl} from './common.mjs';
const contracts=JSON.parse(fs.readFileSync(new URL('../agent/tool-contracts.json',import.meta.url))).tools;
export function checkArgs(name,args){const errors=[],s=contracts[name];if(!s)return ['unknown tool'];if(!args||typeof args!=='object'||Array.isArray(args))return ['arguments must be object'];for(const k of s.required)if(!(k in args))errors.push('missing '+k);for(const [k,v]of Object.entries(args)){const p=s.properties[k];if(!p){errors.push('unknown argument '+k);continue;}if(p.enum&&!p.enum.includes(v))errors.push('invalid enum '+k);if(p.type==='integer'&&(!Number.isInteger(v)||v<p.min||v>p.max))errors.push('invalid integer '+k);if(p.type==='string'&&(typeof v!=='string'||!v.trim()||Array.from(v).length<p.min||Array.from(v).length>p.max))errors.push('invalid string '+k);if(p.type==='boolean'&&typeof v!=='boolean')errors.push('invalid boolean '+k);}return errors;}
export function scoreRag(runs,queries=jsonl('rag/queries.jsonl'),qrels=jsonl('rag/qrels.jsonl'),ks=[1,3,5,10]){
 const known=new Set(jsonl('corpus/manifest.jsonl').map(d=>d.id));const qids=new Set(queries.map(q=>q.id));const byRun=new Map();for(const r of runs){if(!qids.has(r.query_id)||byRun.has(r.query_id)||!Array.isArray(r.doc_ids))throw Error('Unknown, duplicate or malformed query run');if(r.doc_ids.some(d=>!known.has(d)))throw Error('Unknown paper ID in ranking');byRun.set(r.query_id,r.doc_ids);}
 const rows=queries.map(q=>{const relevant=new Set(qrels.filter(x=>x.query_id===q.id&&x.relevance>0).map(x=>x.doc_id));if(!relevant.size)throw Error('Unanswerable case requires separate metric');const ranked=byRun.get(q.id)||[];const scores={};for(const k of ks){const prefix=ranked.slice(0,k),hit=new Set(prefix.filter(d=>relevant.has(d))).size,rank=prefix.findIndex(d=>relevant.has(d));scores[k]={known_positive_recall:hit/relevant.size,known_positive_hit:Number(hit>0),known_first_relevant_rr:rank<0?0:1/(rank+1)};}return {query_id:q.id,missing:!byRun.has(q.id),scores};});
 const macro=Object.fromEntries(ks.map(k=>[k,Object.fromEntries(['known_positive_recall','known_positive_hit','known_first_relevant_rr'].map(m=>[m,rows.reduce((s,r)=>s+r.scores[k][m],0)/rows.length]))]));
 return {label:'silver_known_positive_diagnostic_not_complete_relevance_gold',queries:rows.length,submitted:byRun.size,missing_count:rows.filter(r=>r.missing).length,duplicate_policy:'Repeated papers consume retrieval slots but do not add relevance credit',macro,rows};
}
export function successfulKnowledgeRead(c){
 if(c.name!=='fetch_knowledge'||c.error||c.is_error===true||!c.result||c.result.is_error===true||typeof c.result!=='object')return false;
 const rep=c.arguments?.representation||'text';
 if(rep==='pages')return Array.isArray(c.result.images)&&c.result.images.length>0;
 if(rep==='source')return typeof c.result.download_url==='string'&&c.result.download_url.length>0;
 return c.result.text_available!==false&&typeof c.result.content==='string'&&c.result.content.trim().length>0;
}
export function successfulWebRead(c){
 return c.name==='fetch_web'&&!c.error&&c.is_error!==true&&c.result&&c.result.is_error!==true&&typeof c.result.content==='string'&&c.result.content.trim().length>0;
}
export function paginationErrors(fetches,fixture){
 const steps=fixture.responses.filter(r=>r.name==='fetch_knowledge');
 const normalize=a=>({doc_id:a?.doc_id,representation:a?.representation??'text',start_page:a?.start_page??1,page_count:a?.page_count??3,offset:a?.offset??0});
 const same=(a,b)=>Object.keys(a).every(k=>a[k]===b[k]);
 const errors=[];let expected=normalize(steps[0].match),completed=0;
 for(let i=0;i<fetches.length;i++){
  const c=fetches[i],args=normalize(c.arguments);
  // An out-of-order attempt remains a violation even if it fails or later reads repair coverage.
  if(!expected||!same(args,expected)){errors.push(i+': request violates current pagination cursor/range');continue;}
  // Retrying the current failed request is permitted; failure never advances the cursor.
  if(!successfulKnowledgeRead(c))continue;
  const offset=c.result.next_offset??null,page=c.result.next_page??null;
  if((offset!==null&&(!Number.isInteger(offset)||offset<=args.offset))||(page!==null&&(!Number.isInteger(page)||page<=args.start_page))){errors.push(i+': invalid pagination response cursor');continue;}
  if(completed<steps.length){
   const step=steps[completed];
   if(!same(args,normalize(step.match))||offset!==(step.result.next_offset??null)||page!==(step.result.next_page??null)){errors.push(i+': response does not complete the required pagination step');continue;}
   completed++;
  }
  expected=offset!==null?{...args,offset}:page!==null?{...args,start_page:page,offset:0}:null;
 }
 if(completed<steps.length)errors.push('required pagination did not complete successfully');
 return errors;
}
export function scoreAgent(trace,task,fixture){
 const errors=[],calls=trace.calls||[];if(!Array.isArray(calls))throw Error('calls must be array');
 if(trace.task_id!==task.id)throw Error('Mismatched task');if(calls.length<task.minimum_calls||calls.length>task.maximum_calls)errors.push('call budget');
 const seen=new Set(),fetched=new Set(),urls=new Set();
 for(let i=0;i<calls.length;i++){
  const c=calls[i],a=c.arguments;errors.push(...checkArgs(c.name,a).map(e=>`${i}: ${e}`));if(!task.available_tools.includes(c.name)||task.forbidden_tools.includes(c.name))errors.push(`${i}: forbidden tool`);
  if(!a||typeof a!=='object')continue;
  if(c.name==='fetch_knowledge'){
   if(!seen.has(a.doc_id))errors.push(`${i}: doc_id not returned by prior successful search`);
   if((a.representation||'text')==='pages'&&!task.model_capabilities.image_inputs)errors.push(`${i}: image call on text-only model`);
   if(successfulKnowledgeRead(c)){for(const [paper,id]of Object.entries(fixture.bindings))if(id===a.doc_id)fetched.add(paper);}
  }
  if(c.name==='search_knowledge'&&!c.error){for(const h of c.result?.hits||[])if(Number.isInteger(h.doc_id)&&h.doc_id>0)seen.add(h.doc_id);}
  if(c.name==='search_web'&&!c.error){for(const h of c.result?.results||[])if(typeof h.url==='string')urls.add(h.url);}
  if(c.name==='fetch_web'&&(task.checks.includes('fetch_url_from_search_result')||(task.required_tools.includes('search_web')&&task.required_tools.includes('fetch_web')))&&!urls.has(a.url))errors.push(`${i}: URL not returned by prior search`);
 }
 for(const name of task.required_tools)if(!calls.some(c=>c.name===name))errors.push('missing tool '+name);
 if(task.required_tools.includes('fetch_web')&&!calls.some(successfulWebRead))errors.push('required webpage evidence did not return successfully with nonempty content');
 for(const [before,after]of task.precedence){const b=calls.findIndex(c=>c.name===before),a=calls.findIndex(c=>c.name===after);if(a>=0&&(b<0||b>=a))errors.push(`order ${before} before ${after}`);}
 for(const p of task.fetch_paper_ids)if(!fetched.has(p))errors.push('missing fetched paper '+p);
 const fetches=calls.filter(c=>c.name==='fetch_knowledge');
 if(task.checks.includes('fetch_representation_source')&&!fetches.some(c=>successfulKnowledgeRead(c)&&c.arguments?.representation==='source'))errors.push('source representation missing');
 if(task.checks.includes('fetch_representation_pages')&&!fetches.some(c=>successfulKnowledgeRead(c)&&c.arguments?.representation==='pages'))errors.push('pages representation missing');
 if(task.checks.includes('fetch_page_4')&&!fetches.some(c=>successfulKnowledgeRead(c)&&c.arguments?.start_page===4))errors.push('page 4 missing');
 if(task.checks.includes('search_web_count_equals_3')&&!calls.some(c=>c.name==='search_web'&&c.arguments?.count===3))errors.push('count must equal 3');
 if(['agent-009','agent-010'].includes(task.id))errors.push(...paginationErrors(fetches,fixture));
 if(task.id==='agent-022'){const at=calls.findIndex(c=>c.error?.code==='forbidden');if(at<0||at!==calls.length-1)errors.push('must stop on permission denial');}
 if(task.id==='agent-015'&&!calls.some(c=>successfulWebRead(c)&&c.arguments?.url===fixture.responses[0].match.url))errors.push('wrong direct URL');
 if(task.id==='agent-020'){const local=calls.find(c=>c.name==='search_knowledge');if(!local||local.error||!Array.isArray(local.result?.hits)||local.result.hits.length)errors.push('web fallback requires confirmed knowledge miss');}
 return {task_id:task.id,mechanical_pass:errors.length===0,errors,semantic_review_required:true,semantic_rubric:task.checks,warning:'Mechanical checks are not an overall agent pass; independently review correctness, citations, refusal and leakage. This scorer trusts normalized trace results and is not an audit-proof execution attestor.'};
}
