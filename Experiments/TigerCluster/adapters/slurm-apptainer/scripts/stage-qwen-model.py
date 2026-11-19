#!/usr/bin/env python3
"""Download/copy one immutable model revision into project quarantine."""
from __future__ import annotations
import argparse,hashlib,json,os,re,shutil,time,urllib.parse,urllib.request
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
  with path.open('rb') as source:head=source.read(128)
  if head.startswith(b'version https://git-lfs.github.com/spec/v1'):raise ValueError('TRANSFER_LFS_POINTER_REJECTED:'+str(path))
  rows.append({'path':path.relative_to(root).as_posix(),'bytes':path.stat().st_size,'digest':sha(path),'lfsPointer':False})
 return {'schemaVersion':'1.0','repository':repository,'revision':revision,'licenseClass':license_class,'state':'STAGED','sourceBytes':sum(x['bytes'] for x in rows),'files':rows}
def public_snapshot_download(repository,revision,dest,required_files=None):
 if REV.fullmatch(revision) is None:raise ValueError('TRANSFER_REVISION_MUTABLE')
 if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+',repository):raise ValueError('TRANSFER_REPOSITORY_INVALID')
 dest=Path(dest)
 api='https://huggingface.co/api/models/'+repository+'/revision/'+revision+'?blobs=true'
 with urllib.request.urlopen(api,timeout=30) as response:metadata=response.read(4*1024*1024+1)
 if len(metadata)>4*1024*1024:raise ValueError('TRANSFER_METADATA_TOO_LARGE')
 value=json.loads(metadata)
 if value.get('sha')!=revision:raise ValueError('TRANSFER_REVISION_RESOLUTION_MISMATCH')
 rows=value.get('siblings',[])
 if not isinstance(rows,list) or any(not isinstance(row,dict) for row in rows):raise ValueError('TRANSFER_METADATA_INVALID')
 deadline=time.monotonic()+3600 if required_files is not None else None
 if required_files is not None:
  available={row.get('rfilename') for row in rows}
  if not required_files or set(required_files)-available:raise ValueError('TRANSFER_REQUIRED_FILE_MISSING')
  rows=[{'rfilename':name} for name in required_files]
 for row in rows:
  name=row.get('rfilename')
  if not isinstance(name,str) or not name or Path(name).is_absolute() or '..' in Path(name).parts:raise ValueError('TRANSFER_REMOTE_PATH_INVALID')
  target=dest/name
  try:target.resolve().relative_to(dest.resolve())
  except ValueError:raise ValueError('TRANSFER_REMOTE_PATH_INVALID')
  expected=required_files[name] if required_files is not None else None
  if expected is not None and (not isinstance(expected.get('bytes'),int) or expected['bytes']<0 or not re.fullmatch(r'[0-9a-f]{64}',expected.get('sha256',''))):raise ValueError('TRANSFER_FILE_COMMITMENT_INVALID')
  target.parent.mkdir(parents=True,exist_ok=True)
  if target.exists():raise ValueError('TRANSFER_FILE_ALREADY_EXISTS')
  url='https://huggingface.co/'+repository+'/resolve/'+revision+'/'+urllib.parse.quote(name,safe='/')
  owned=False
  try:
   with urllib.request.urlopen(url,timeout=120) as source,target.open('xb') as output:
    owned=True;count=0;digest=hashlib.sha256()
    while True:
     if deadline is not None and time.monotonic()>=deadline:raise TimeoutError('TRANSFER_DEADLINE_EXCEEDED')
     chunk=source.read(1024*1024)
     if not chunk:break
     count+=len(chunk)
     if expected is not None and count>expected['bytes']:raise ValueError('TRANSFER_FILE_SIZE_MISMATCH')
     digest.update(chunk);output.write(chunk)
   if expected is not None and (count!=expected['bytes'] or digest.hexdigest()!=expected['sha256']):raise ValueError('TRANSFER_FILE_COMMITMENT_MISMATCH')
  except BaseException:
   if owned:target.unlink()
   raise
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
