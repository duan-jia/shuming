import fs from 'node:fs';import path from 'node:path';import {execFileSync} from 'node:child_process';
import {cache,jsonl,sha,safeId} from './common.mjs';
const docs=jsonl('corpus/manifest.jsonl');fs.mkdirSync(path.join(cache,'pdf'),{recursive:true});
for(const d of docs){
 const id=safeId(d.id);if(!/^\d{4}\.\d{4,5}$/.test(d.arxiv_id)||!/^v[1-9]\d*$/.test(d.version))throw Error('Invalid pinned identifier');
 const expected=`https://arxiv.org/pdf/${d.arxiv_id}${d.version}`;if(d.pdf_url!==expected)throw Error('Untrusted download URL');
 const dest=path.join(cache,'pdf',id+'.pdf');
 if(fs.existsSync(dest)){if(sha(fs.readFileSync(dest))!==d.pdf_sha256)throw Error(`Existing hash mismatch: ${id}; preserve and inspect manually`);console.log('verified cached',id);continue;}
 const tmp=dest+'.part';
 try{execFileSync('curl',['--fail','--location','--silent','--show-error','--proto','=https','--proto-redir','=https','--max-time','120','--retry','2',expected,'--output',tmp],{stdio:'inherit',timeout:370000});const b=fs.readFileSync(tmp);if(b.subarray(0,5).toString()!=='%PDF-'||sha(b)!==d.pdf_sha256)throw Error('PDF/hash mismatch '+id);fs.renameSync(tmp,dest);console.log('downloaded',id);}finally{fs.rmSync(tmp,{force:true});}
 await new Promise(r=>setTimeout(r,3000));
}
