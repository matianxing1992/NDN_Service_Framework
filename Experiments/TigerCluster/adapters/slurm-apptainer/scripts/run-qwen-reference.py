#!/usr/bin/env python3
"""Deterministic full-model Qwen correctness oracle."""
from __future__ import annotations
import argparse,hashlib,json,random,time
from pathlib import Path
def digest(value):return 'sha256:'+hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--model',required=True);p.add_argument('--prompt-file',required=True);p.add_argument('--max-new-tokens',type=int,choices=(1,2,32),required=True);p.add_argument('--output',required=True);p.add_argument('--seed',type=int,default=109);a=p.parse_args()
 from transformers import AutoModelForCausalLM,AutoTokenizer
 import torch
 random.seed(a.seed);torch.manual_seed(a.seed);torch.use_deterministic_algorithms(True,warn_only=True)
 prompts=json.loads(Path(a.prompt_file).read_text());tokenizer=AutoTokenizer.from_pretrained(a.model,local_files_only=True);model=AutoModelForCausalLM.from_pretrained(a.model,local_files_only=True,torch_dtype=torch.float16).to('cuda').eval()
 rows=[]
 for item in prompts:
  text=item['text'];inputs=tokenizer(text,return_tensors='pt').to('cuda');torch.cuda.synchronize();started=time.perf_counter();out=model.generate(**inputs,max_new_tokens=a.max_new_tokens,do_sample=False);torch.cuda.synchronize();elapsed=time.perf_counter()-started
  input_ids=inputs['input_ids'][0].tolist();output_ids=out[0].tolist()[len(input_ids):];rows.append({'promptId':item['id'],'inputTokenIds':input_ids,'outputTokenIds':output_ids,'inputDigest':digest(input_ids),'outputDigest':digest(output_ids),'elapsedSeconds':elapsed})
 result={'schemaVersion':'1.0','modelPath':a.model,'maxNewTokens':a.max_new_tokens,'decode':'greedy','seed':a.seed,'rows':rows};Path(a.output).write_text(json.dumps(result,indent=2,sort_keys=True)+'\n');print(json.dumps(result,sort_keys=True))
if __name__=='__main__':main()
