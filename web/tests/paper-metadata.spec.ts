import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { GET, PUT } from '../src/app/api/library/documents/[id]/metadata/route.ts';
test('all visible-document readers can persist JSON; visibility and validation still enforced', async () => {
 const dir=await mkdtemp(path.join(tmpdir(),'metadata-')); const previous=process.env.PDF_PROCESSED_DIR; const original=globalThis.fetch;
 process.env.PDF_PROCESSED_DIR=dir;
 let status=200;
 globalThis.fetch=async()=>Response.json(status===200?{id:1,can_delete:false}:{error:'denied'},{status});
 const params={params:Promise.resolve({id:'1'})};
 const req=(method:string,body?:string,auth=true)=>new Request('http://localhost/api/library/documents/1/metadata',{method,headers:auth?{Authorization:'Bearer reader'}:{},body});
 try {
  assert.equal((await GET(req('GET',undefined,false),params)).status,401);
  assert.equal((await PUT(req('PUT','{'),params)).status,400);
  assert.equal((await PUT(req('PUT','[]'),params)).status,400);
  assert.equal((await PUT(req('PUT',JSON.stringify({publication_year:9999,publication_venue:'X'})),params)).status,400);
  const body={title:'Edited title',authors:['Reader'],publication_year:2026,publication_venue:"ACM SIGCOMM (SIGCOMM '26)",custom_notes:{verified:true}};
  assert.equal((await PUT(req('PUT',JSON.stringify(body)),params)).status,200);
  const value=await(await GET(req('GET'),params)).json();
  assert.equal(value.title,body.title);assert.equal(value.publication_venue,'ACM SIGCOMM');assert.deepEqual(value.custom_notes,body.custom_notes);
  status=404;assert.equal((await GET(req('GET'),params)).status,404);assert.equal((await PUT(req('PUT',JSON.stringify(body)),params)).status,404);
 } finally {globalThis.fetch=original;if(previous===undefined)delete process.env.PDF_PROCESSED_DIR;else process.env.PDF_PROCESSED_DIR=previous;await rm(dir,{recursive:true,force:true});}
});
