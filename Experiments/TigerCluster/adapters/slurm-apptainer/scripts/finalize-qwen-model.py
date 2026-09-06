#!/usr/bin/env python3
"""Verify quarantine bytes and atomically promote a completed model."""
from __future__ import annotations
import argparse,hashlib,json,os
from pathlib import Path
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return 'sha256:'+h.hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--partial',required=True);p.add_argument('--manifest',required=True);p.add_argument('--final',required=True);p.add_argument('--sealed-manifest',required=True);p.add_argument('--registry-entry');p.add_argument('--model-id');p.add_argument('--family',default='Qwen2.5-Instruct');p.add_argument('--size-class');a=p.parse_args()
 partial=Path(a.partial);final=Path(a.final);value=json.loads(Path(a.manifest).read_text())
 if value.get('state')!='STAGED':raise SystemExit('TRANSFER_STATE_INVALID')
 expected={x['path'] for x in value['files']};actual={x.relative_to(partial).as_posix() for x in partial.rglob('*') if x.is_file()}
 if expected!=actual:raise SystemExit('TRANSFER_FILE_SET_MISMATCH')
 for row in value['files']:
  path=partial/row['path']
  if path.stat().st_size!=row['bytes'] or sha(path)!=row['digest']:raise SystemExit('TRANSFER_DIGEST_MISMATCH:'+row['path'])
 if final.exists():raise SystemExit('TRANSFER_FINAL_ALREADY_EXISTS')
 final.parent.mkdir(parents=True,exist_ok=True);os.rename(partial,final);value['state']='SEALED';value['projectPath']=str(final);value['manifestDigest']='sha256:'+hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest();Path(a.sealed_manifest).parent.mkdir(parents=True,exist_ok=True);Path(a.sealed_manifest).write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')
 if a.registry_entry:
  if not a.model_id or not a.size_class:raise SystemExit('REGISTRY_IDENTITY_REQUIRED')
  by_path={row['path']:row for row in value['files']}
  if 'LICENSE' not in by_path:raise SystemExit('REGISTRY_LICENSE_MISSING')
  tokenizer=[row for row in value['files'] if row['path'].startswith(('tokenizer','vocab','merges'))]
  if not tokenizer:raise SystemExit('REGISTRY_TOKENIZER_MISSING')
  registry={'modelId':a.model_id,'family':a.family,'sizeClass':a.size_class,'repository':value['repository'],'revision':value['revision'],'tokenizerDigest':'sha256:'+hashlib.sha256(json.dumps(tokenizer,sort_keys=True,separators=(',',':')).encode()).hexdigest(),'licenseClass':value['licenseClass'],'licenseDigest':by_path['LICENSE']['digest'],'files':value['files'],'sourceBytes':value['sourceBytes'],'state':'SEALED','projectPath':str(final)}
  registry['registryDigest']='sha256:'+hashlib.sha256(json.dumps(registry,sort_keys=True,separators=(',',':')).encode()).hexdigest();target=Path(a.registry_entry);target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps(registry,indent=2,sort_keys=True)+'\n')
 print(json.dumps(value,sort_keys=True))
if __name__=='__main__':main()
