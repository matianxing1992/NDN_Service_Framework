#!/usr/bin/env python3
"""Download/copy one immutable model revision into project quarantine."""
from __future__ import annotations
import argparse,hashlib,json,os,re,shutil,urllib.parse,urllib.request
from pathlib import Path
REV=re.compile(r'^[0-9a-f]{40}$')
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return 'sha256:'+h.hexdigest()
def allowed(path):
 value=str(path.resolve())
 return value.startswith('/project/') or os.environ.get('NDNSF_SPEC109_ALLOW_TEST_ROOT')=='1'
def manifest(root,repository,revision,license_class):
 rows=[]
 for path in sorted(root.rglob('*')):
  if path.is_symlink():raise ValueError('TRANSFER_SYMLINK_REJECTED:'+str(path))
  if not path.is_file():continue
  head=path.read_bytes()[:128]
  if head.startswith(b'version https://git-lfs.github.com/spec/v1'):raise ValueError('TRANSFER_LFS_POINTER_REJECTED:'+str(path))
  rows.append({'path':path.relative_to(root).as_posix(),'bytes':path.stat().st_size,'digest':sha(path),'lfsPointer':False})
 return {'schemaVersion':'1.0','repository':repository,'revision':revision,'licenseClass':license_class,'state':'STAGED','sourceBytes':sum(x['bytes'] for x in rows),'files':rows}
def public_snapshot_download(repository,revision,dest):
 api='https://huggingface.co/api/models/'+repository+'/revision/'+revision+'?blobs=true'
 with urllib.request.urlopen(api,timeout=30) as response:value=json.load(response)
 if value.get('sha')!=revision:raise ValueError('TRANSFER_REVISION_RESOLUTION_MISMATCH')
 for row in value.get('siblings',[]):
  name=row.get('rfilename')
  if not isinstance(name,str) or not name or '..' in Path(name).parts:raise ValueError('TRANSFER_REMOTE_PATH_INVALID')
  target=dest/name;target.parent.mkdir(parents=True,exist_ok=True)
  url='https://huggingface.co/'+repository+'/resolve/'+revision+'/'+urllib.parse.quote(name,safe='/')
  with urllib.request.urlopen(url,timeout=120) as source,target.open('wb') as output:shutil.copyfileobj(source,output,1024*1024)
def main():
 p=argparse.ArgumentParser();p.add_argument('--repository',required=True);p.add_argument('--revision',required=True);p.add_argument('--destination',required=True);p.add_argument('--manifest',required=True);p.add_argument('--license-class',required=True);p.add_argument('--source-dir');a=p.parse_args()
 if REV.fullmatch(a.revision) is None:raise SystemExit('TRANSFER_REVISION_MUTABLE')
 dest=Path(a.destination)
 if not allowed(dest):raise SystemExit('TRANSFER_DESTINATION_INVALID')
 if dest.exists():raise SystemExit('TRANSFER_PARTIAL_ALREADY_EXISTS')
 dest.parent.mkdir(parents=True,exist_ok=True)
 if a.source_dir:shutil.copytree(a.source_dir,dest,symlinks=False)
 else:
  try:from huggingface_hub import snapshot_download
  except ImportError:public_snapshot_download(a.repository,a.revision,dest)
  else:snapshot_download(repo_id=a.repository,revision=a.revision,local_dir=str(dest),local_dir_use_symlinks=False,resume_download=True)
 value=manifest(dest,a.repository,a.revision,a.license_class);target=Path(a.manifest);target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps(value,indent=2,sort_keys=True)+'\n');print(json.dumps(value,sort_keys=True))
if __name__=='__main__':main()
