// Host-native, loopback-only model profile. No network calls, downloads or deployment by default.
import fs from 'node:fs';import path from 'node:path';import {fileURLToPath,pathToFileURL} from 'node:url';import {spawn} from 'node:child_process';
export const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
export function profile(){return JSON.parse(fs.readFileSync(path.join(root,'deploy/models/qwen38-27b-3080.json')));}
function integer(v,name,min,max){if(!/^\d+$/.test(String(v))||Number(v)<min||Number(v)>max)throw Error(`Invalid ${name}`);return String(Number(v));}
export function command(env=process.env){
 const p=profile();return {binary:env.LLAMA_SERVER_BIN||'llama-server',args:['--model',path.resolve(root,env.MODEL_PATH||p.file),'--alias',p.model,'--host','127.0.0.1','--port','8010','--ctx-size',String(p.context_window_tokens),'--parallel',String(p.parallel),'--n-gpu-layers',integer(env.MODEL_GPU_LAYERS??p.gpu_layers,'GPU layers',0,1000),'--batch-size',String(p.batch_size),'--ubatch-size',String(p.ubatch_size),'--threads',integer(env.MODEL_THREADS??p.threads,'threads',1,1024),'--jinja','--chat-template-kwargs',JSON.stringify({enable_thinking:p.thinking})]};
}
export function configured(base){
 const p=profile();return {...base,remote_engine:{...base.remote_engine,endpoint:p.endpoint,model:p.model,api_key:'local-no-auth',context_window_tokens:p.context_window_tokens,supports_images:false},rag:{...base.rag,query_rewrite:{...base.rag?.query_rewrite,enabled:false,model:p.model}},agent:{...base.agent,timeout_seconds:300,max_tool_result_bytes:6000}};
}
function main(args){
 if(args.length===0||args[0]==='--help'){console.log('Usage: node deploy/models/local-model.mjs plan | run | config <input.json> <new-output.json>\nplan: prints launch arguments only; run: explicitly starts installed llama-server; config: creates a new private config, never overwrites.');return;}
 const action=args[0];if(action==='plan'){console.log(JSON.stringify({...command(),status:profile().status},null,2));return;}
 if(action==='config'){
  if(args.length!==3)throw Error('config requires input and new output');
  const value=configured(JSON.parse(fs.readFileSync(args[1],'utf8')));
  fs.writeFileSync(args[2],JSON.stringify(value,null,2)+'\n',{flag:'wx',mode:0o600});console.log('Created new local model config; original input was not modified.');return;
 }
 if(action==='run'){
  const c=command();if(!fs.statSync(c.args[1]).isFile())throw Error('MODEL_PATH is not a model file');
  const child=spawn(c.binary,c.args,{stdio:'inherit',shell:false,env:{...process.env,CUDA_VISIBLE_DEVICES:process.env.CUDA_VISIBLE_DEVICES||'0'}});
  for(const signal of ['SIGINT','SIGTERM'])process.on(signal,()=>child.kill(signal));
  child.on('error',e=>{console.error(e.message);process.exitCode=1;});child.on('exit',(code,signal)=>{process.exitCode=code??(signal==='SIGINT'?130:1);});return;
 }
 throw Error('Unknown action: '+action);
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href){try{main(process.argv.slice(2));}catch(e){console.error(e.message);process.exitCode=1;}}
