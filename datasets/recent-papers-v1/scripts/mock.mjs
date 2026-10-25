// Evaluation-only normalized mock; never deploy this as an authorization layer.
import {checkArgs} from './grade.mjs';
export function resolveFixture(fixture,name,args){
 const errors=checkArgs(name,args);if(errors.length)throw Error(errors.join('; '));
 const normalized=name==='fetch_knowledge'?{representation:'text',start_page:1,page_count:3,offset:0,...args}:args;
 const matches=fixture.responses.filter(r=>r.name===name&&Object.entries(r.match).every(([k,v])=>normalized[k]===v)).sort((a,b)=>Object.keys(b.match).length-Object.keys(a.match).length);
 if(!matches.length)throw Error('No fixture response matches this call');if(matches[1]&&Object.keys(matches[1].match).length===Object.keys(matches[0].match).length)throw Error('Ambiguous fixture');return structuredClone(matches[0]);
}
