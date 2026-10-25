import fs from 'node:fs';
import {scoreRag} from './grade.mjs';
import {jsonl} from './common.mjs';
if(!process.argv[2])throw Error('Usage: node scripts/score-rag.mjs /path/to/rankings.jsonl [K ...]');
const text=fs.readFileSync(process.argv[2],'utf8').trim();
const runs=text?text.split('\n').map(JSON.parse):[];
const ks=process.argv.length>3?process.argv.slice(3).map(Number):[1,3,5,10];
console.log(JSON.stringify(scoreRag(runs,jsonl('rag/queries.jsonl'),jsonl('rag/qrels.jsonl'),ks),null,2));
