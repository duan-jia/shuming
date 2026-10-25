import fs from 'node:fs';import path from 'node:path';import {fileURLToPath} from 'node:url';import crypto from 'node:crypto';
export const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
export const cache=path.resolve(process.env.EVAL_CACHE_DIR||path.join(root,'.cache'));
export const sha=s=>crypto.createHash('sha256').update(s).digest('hex');
export function jsonl(file){return fs.readFileSync(path.join(root,file),'utf8').trim().split('\n').filter(Boolean).map((line,i)=>{try{return JSON.parse(line)}catch{throw Error(`Invalid JSON ${file}:${i+1}`)}})}
export function writeJsonl(file,rows){fs.writeFileSync(path.join(root,file),rows.map(r=>JSON.stringify(r)).join('\n')+'\n')}
export function safeId(id){if(typeof id!=='string'||!/^[-a-z0-9]+$/.test(id))throw Error('Unsafe identifier');return id}
