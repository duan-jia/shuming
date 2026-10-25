// Resolve controlled tool responses exactly as the dataset's tested mock does.
// No network or model access. Neither answers nor hidden rules go to the model.
import fs from 'node:fs';
import readline from 'node:readline';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const root=path.resolve(process.argv[2]);
const {resolveFixture}=await import(pathToFileURL(path.join(root,'scripts/mock.mjs')).href);
const fixtures=fs.readFileSync(path.join(root,'agent/fixtures.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
for await(const line of readline.createInterface({input:process.stdin})){
 try{
  const r=JSON.parse(line),fixture=fixtures.find(f=>f.id===r.fixture_id);
  if(!fixture)throw Error('Unknown fixture');
  const response=resolveFixture(fixture,r.name,r.arguments);
  console.log(JSON.stringify(response));
 }catch(error){console.log(JSON.stringify({error:{code:'invalid_fixture_call',message:error.message}}));}
}
