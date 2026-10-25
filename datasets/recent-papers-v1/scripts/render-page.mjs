import fs from 'node:fs';import path from 'node:path';import {cache,jsonl,sha} from './common.mjs';
const {getDocument}=await import(process.env.PDFJS_MODULE||'pdfjs-dist/legacy/build/pdf.mjs');
const {createCanvas}=await import(process.env.CANVAS_MODULE||'@napi-rs/canvas');
const doc=jsonl('corpus/manifest.jsonl').find(d=>d.id==='bytescale');
const b=fs.readFileSync(path.join(cache,'pdf/bytescale.pdf'));if(sha(b)!==doc.pdf_sha256)throw Error('PDF hash mismatch');
const task=getDocument({data:new Uint8Array(b),useSystemFonts:true,isEvalSupported:false});const pdf=await task.promise;
const page=await pdf.getPage(4);const viewport=page.getViewport({scale:1.4});const canvas=createCanvas(Math.ceil(viewport.width),Math.ceil(viewport.height));
await page.render({canvasContext:canvas.getContext('2d'),viewport}).promise;
fs.mkdirSync(path.join(cache,'pages'),{recursive:true});const dest=path.join(cache,'pages/bytescale-p4.png');fs.writeFileSync(dest,canvas.toBuffer('image/png'));console.log(dest);await task.destroy();
