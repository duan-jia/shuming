import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import crypto from 'node:crypto';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const local=path.resolve(process.env.EVAL_CACHE_DIR||path.join(root,'.cache'));
fs.mkdirSync(path.join(local,'text'),{recursive:true});
const {getDocument}=await import(process.env.PDFJS_MODULE||'pdfjs-dist/legacy/build/pdf.mjs');
const docs=fs.readFileSync(path.join(root,'corpus/manifest.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
for(const d of docs){
 const dest=path.join(local,'text',d.id+'.json');
 if(fs.existsSync(dest)){const prior=JSON.parse(fs.readFileSync(dest));if(prior.pdf_sha256!==d.pdf_sha256)throw Error('Stale extraction '+d.id);continue;}
 const bytes=fs.readFileSync(path.join(local,'pdf',d.id+'.pdf'));
 if(crypto.createHash('sha256').update(bytes).digest('hex')!==d.pdf_sha256)throw Error('SHA mismatch '+d.id);
 const task=getDocument({data:new Uint8Array(bytes),useSystemFonts:true,isEvalSupported:false});
 const pdf=await task.promise;
 const pages=[];
 for(let n=1;n<=pdf.numPages;n++){
  const page=await pdf.getPage(n), content=await page.getTextContent();
  const text=content.items.map(x=>x.str).join(' ').normalize('NFKC').replace(/\s+/g,' ').trim();
  pages.push({page:n,text,sha256:crypto.createHash('sha256').update(text).digest('hex')});
 }
 fs.writeFileSync(dest,JSON.stringify({id:d.id,pdf_sha256:d.pdf_sha256,extraction:'pdfjs-dist@6.4.299; item strings space joined; NFKC; whitespace collapsed; JS UTF-16 offsets',pages},null,2));
 console.log(d.id,pages.length,'pages');await task.destroy();
}
