import fs from 'node:fs';import {scoreRag} from './grade.mjs';
if(!process.argv[2])throw Error('Usage: node scripts/score-rag.mjs /path/to/rankings.jsonl');
const lines=fs.readFileSync(process.argv[2],'utf8').trim();const runs=lines?lines.split('\n').map(JSON.parse):[];console.log(JSON.stringify(scoreRag(runs),null,2));
