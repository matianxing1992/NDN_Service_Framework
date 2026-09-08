"""Bounded, resumable SSH transport into the existing receiver/submit owners."""
from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

from .yolo_profile import _read_plane
from .yolo_transport import (_path, _roots, _identity, _matches, _capacity,
                             validate_manifest, _receive_files)

MAX_CONTROL = 32 * 1024 * 1024

# This loader runs before importing the received, hash-bound Python package.
# Only the small frozen harness is bootstrapped; payloads travel through rsync.
BOOTSTRAP = r'''
import base64,hashlib,json,os,pathlib,re,sys,tempfile
def pairs(rows):
 value={}
 for k,v in rows:
  if k in value: raise ValueError('DUPLICATE_CONTROL_KEY')
  value[k]=v
 return value
raw=sys.stdin.buffer.read(32*1024*1024+1)
if len(raw)>32*1024*1024: raise ValueError('CONTROL_SIZE')
p=json.loads(raw,object_pairs_hook=pairs)
if set(p)!={'schema','manifest','binding','bootstrap'} or p['schema']!='tiger-yolo-ssh-v1': raise ValueError('CONTROL_SCHEMA')
data={n:base64.b64decode(v,validate=True) for n,v in p['bootstrap'].items()}
if len(data)>128 or sum(map(len,data.values()))>16*1024*1024: raise ValueError('BOOTSTRAP_SIZE')
manifest=data['harness-manifest.json']
if 'sha256:'+hashlib.sha256(manifest).hexdigest()!=p['binding']['harnessDigest']: raise ValueError('BOOTSTRAP_DIGEST')
files=json.loads(manifest,object_pairs_hook=pairs)['files']
if set(data)!=set(files)|{'harness-manifest.json'}: raise ValueError('BOOTSTRAP_INVENTORY')
for n,b in data.items():
 parts=pathlib.PurePosixPath(n)
 if parts.is_absolute() or str(parts)!=n or '..' in parts.parts or not re.fullmatch(r'[A-Za-z0-9_./-]+',n): raise ValueError('BOOTSTRAP_PATH')
 if n in files and (len(b)!=files[n]['bytes'] or 'sha256:'+hashlib.sha256(b).hexdigest()!=files[n]['sha256']): raise ValueError('BOOTSTRAP_FILE')
artifact=pathlib.Path(p['manifest']['roots'][0])
if not artifact.is_absolute() or '..' in artifact.parts or not artifact.is_dir(): raise ValueError('BOOTSTRAP_ROOT')
def safe(path):
 if any(x.is_symlink() for x in (path,*path.parents)): raise ValueError('BOOTSTRAP_SYMLINK')
 return path
safe(artifact)
identity=hashlib.sha256(json.dumps({'manifest':p['manifest'],'binding':p['binding']},sort_keys=True,separators=(',',':'),allow_nan=False).encode()).hexdigest()
stage=artifact/'.incoming'/identity
safe(stage).mkdir(mode=0o700,parents=True,exist_ok=True)
def write(path,payload):
 safe(path)
 path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
 if path.exists():
  if not path.is_file() or path.read_bytes()!=payload: raise ValueError('BOOTSTRAP_EXISTING_CONFLICT')
  return
 fd,tmp=tempfile.mkstemp(prefix='.bootstrap-',dir=path.parent)
 try:
  with os.fdopen(fd,'wb') as f:
   f.write(payload); f.flush(); os.fchmod(f.fileno(),0o444); os.fsync(f.fileno())
  try: os.link(tmp,path,follow_symlinks=False)
  except FileExistsError:
   if path.read_bytes()!=payload: raise ValueError('BOOTSTRAP_EXISTING_CONFLICT')
 finally: os.unlink(tmp)
for n,b in data.items(): write(stage/'bootstrap'/n,b)
directories={stage/'bootstrap'}
for n in data:
 for parent in pathlib.PurePosixPath(n).parents: directories.add(stage/'bootstrap'/parent)
for directory in sorted(directories,key=lambda x:len(x.parts),reverse=True):
 if safe(directory).stat().st_mode&0o222: directory.chmod(0o555)
write(stage/'packet.json',json.dumps(p,sort_keys=True,separators=(',',':'),allow_nan=False).encode())
sys.path.insert(0,str(stage/'bootstrap'))
from runtime.yolo_ssh import remote_phase
print(json.dumps(remote_phase(p,sys.argv[1],stage),sort_keys=True))
'''


def _digest(value):
    return 'sha256:'+hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()).hexdigest()


def _project_layout(roots):
    if any(not str(root).startswith('/project/') for root in roots):
        raise ValueError('SSH_PROJECT_LAYOUT_REQUIRED')


def _write_record(path, value):
    payload=json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)
    with Path(path).open('x') as stream:
        stream.write(payload); stream.flush(); os.fsync(stream.fileno())


def _binding(packet):
    if set(packet)!={'schema','manifest','binding','bootstrap'} or packet['schema']!='tiger-yolo-ssh-v1':
        raise ValueError('SSH_PACKET_SCHEMA')
    value=packet['binding']; roots=_roots(packet['manifest']['roots'])
    validate_manifest(packet['manifest'],roots=roots)
    if (not isinstance(value,dict) or set(value)!={'profile','runId','case','bundle','lockRoot','harnessDigest','seconds','submit'}
            or not isinstance(value['runId'],str) or re.fullmatch(r'[a-z][a-z0-9-]{1,63}',value['runId']) is None
            or value['case'] not in ('single-node-gpu','two-node-gpu','negative-dependency')
            or type(value['seconds']) is not int or not 10<=value['seconds']<=86400
            or type(value['submit']) is not bool):
        raise ValueError('SSH_BINDING')
    profile=_path(value['profile']); bundle=_path(value['bundle']); lock=_path(value['lockRoot'])
    names={row['path'] for row in packet['manifest']['files']}
    if (roots[0] not in profile.parents or str(profile) not in names
            or bundle!=roots[1]/value['runId']/'bundle'
            or str(bundle.parent/'prepare.json') not in names
            or any(lock==root or lock in root.parents or root in lock.parents for root in roots)):
        raise ValueError('SSH_LAYOUT_BINDING')
    return value,roots


def _missing(manifest):
    missing=[]
    for row in manifest['files']:
        path=_path(row['path'])
        if path.exists():
            if not _matches(path,row): raise ValueError('SSH_DESTINATION_CONFLICT')
        else: missing.append(row)
    return missing


def _harness_directories(manifest):
    """Validate transported harness contents before any directory-mode change."""
    from .yolo_bundle import MANIFEST, _load
    rows={row['path']:row for row in manifest['files']}
    directories=set()
    for row in manifest['files']:
        path=Path(row['path'])
        if path.name!=MANIFEST: continue
        _,payloads=_load(path,row['sha256'])
        root=path.parent
        files={str(root/name) for name in payloads}|{str(path)}
        expected={root}
        for name in payloads:
            expected.update(root/parent for parent in Path(name).parents)
        if not files<=rows.keys(): raise ValueError('SSH_HARNESS_TRANSPORT_INVENTORY')
        for name in files:
            if not _matches(Path(name),rows[name]) or Path(name).stat().st_mode&0o222:
                raise ValueError('SSH_HARNESS_FILE_MODE')
        for directory in expected:
            _path(str(directory))
            if not directory.is_dir(): raise ValueError('SSH_HARNESS_DIRECTORY')
            for child in directory.iterdir():
                _path(str(child))
                if child not in expected and str(child) not in files:
                    raise ValueError('SSH_HARNESS_EXTRA_CONTENT')
            directories.add(directory)
    return directories


def _site(bootstrap, seconds):
    from .yolo_submission import verify_operator_python
    result=subprocess.run(['/usr/bin/scontrol','show','config'],check=True,capture_output=True,
        timeout=seconds,env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
    if (len(result.stdout)>4*1024*1024 or result.stderr
            or re.findall(rb'^ClusterName\s*=\s*(\S+)\s*$',result.stdout,re.M)!=[b'itiger']):
        raise ValueError('SSH_RECEIVER_CLUSTER')
    verify_operator_python(bootstrap,seconds=seconds,operator_python=sys.executable)


def remote_phase(packet, phase, stage):
    """Execute only from the verified bootstrap; stdout is a small control reply."""
    from .yolo_bundle import verify_harness
    from .yolo_submission import transport_guard
    value,roots=_binding(packet)
    expected=roots[0]/'.incoming'/_digest({'manifest':packet['manifest'],'binding':value})[7:]
    if _path(str(stage))!=expected or phase not in ('preflight','finish'):
        raise ValueError('SSH_STAGE_BINDING')
    verify_harness(stage/'bootstrap',expected_manifest_sha256=value['harnessDigest'])
    _site(stage/'bootstrap',value['seconds'])
    manifest=packet['manifest']; missing=_missing(manifest)
    unsealed=[] if missing else [p for p in _harness_directories(manifest) if p.stat().st_mode&0o222]
    if phase=='preflight':
        needed=[]
        if missing or unsealed:
            with transport_guard(value['lockRoot']):
                blobs=stage/'blobs'; _path(str(blobs)).mkdir(mode=0o700,exist_ok=True)
                by_hash={row['sha256']:row for row in missing}
                capacity=[(None,Path(row['path']),row) for row in missing]
                for digest,row in sorted(by_hash.items()):
                    target=_path(str(blobs/digest[7:]))
                    count=0
                    if target.exists():
                        found=_identity(target); count=found['bytes']
                        if found['sha256']==digest and count==row['bytes']: continue
                        if count>=row['bytes']:
                            target.rename(blobs/(digest[7:]+'.rejected-'+str(time.time_ns())))
                            count=0
                    needed.append(dict(row,blob=str(target)))
                    capacity.append((None,target,dict(bytes=row['bytes']-count)))
                _capacity(capacity)
        return dict(schema='tiger-yolo-ssh-preflight-v1',manifestDigest=_digest(manifest),
                    stage=str(stage),needed=needed,complete=not (missing or unsealed))
    if missing or unsealed:
        with transport_guard(value['lockRoot']):
            transport=_receive_files(manifest,roots=roots,staging=stage)
            for directory in sorted(_harness_directories(manifest),key=lambda p:len(p.parts),reverse=True):
                if directory.stat().st_mode&0o222:
                    directory.chmod(0o555)
                    fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY)
                    try: os.fsync(fd)
                    finally: os.close(fd)
    else:
        # Read-only retries must reach unknown-job reconciliation even while its
        # journal is active; do not reacquire a publication guard for no writes.
        transport=dict(status='CONTENT_VERIFIED',qualification='NOT_EVALUATED',files=len(manifest['files']))
    prepared=_read_plane(roots[1]/value['runId']/'prepare.json')
    if (prepared.get('candidateDigest')!=manifest['candidateDigest']
            or prepared.get('harnessManifestSha256')!=value['harnessDigest']):
        raise ValueError('SSH_PREPARATION_BINDING')
    verify_harness(Path(value['bundle']),expected_manifest_sha256=value['harnessDigest'])
    identity=dict(manifestDigest=_digest(manifest),stage=str(stage))
    if not value['submit']:
        return dict(status='STAGED',qualification='NOT_EVALUATED',transport=transport,exitCode=78,**identity)
    command=[sys.executable,'-B',str(Path(value['bundle'])/'jobs/yolo/submit.py'),'submit',
        '--profile',value['profile'],'--run-id',value['runId'],'--output',str(roots[1]),
        '--case',value['case'],'--remote-receiver']
    result=subprocess.run(command,check=False,capture_output=True,timeout=value['seconds'])
    if len(result.stdout)+len(result.stderr)>4*1024*1024: raise ValueError('SSH_SUBMIT_OUTPUT')
    record=dict(status='REMOTE_SUBMIT_RETURNED',qualification='NOT_EVALUATED',transport=transport,
        exitCode=result.returncode,stdout=result.stdout.decode(),stderr=result.stderr.decode(),**identity)
    _write_record(stage/('submit-observation-'+str(time.time_ns())+'.json'),record)
    return record


def coordinate(manifest, *, profile_path, profile, prepared, submit=True):
    """Sender-side owner. Timeouts retain staging and never imply a failed job."""
    from .yolo_bundle import MANIFEST, REQUIRED_HARNESS_FILES, verify_harness
    bundle=Path(prepared['bundle'])
    verify_harness(bundle,expected_manifest_sha256=prepared['harnessManifestSha256'])
    bootstrap={name:base64.b64encode((bundle/name).read_bytes()).decode('ascii')
               for name in sorted(REQUIRED_HARNESS_FILES|{MANIFEST})}
    seconds=profile['storage'].get('transferTimeoutSeconds',1800)
    packet=dict(schema='tiger-yolo-ssh-v1',manifest=manifest,bootstrap=bootstrap,binding=dict(
        profile=str(Path(profile_path).absolute()),runId=prepared['runId'],case=prepared['case'],
        bundle=str(bundle),lockRoot=profile['storage']['sharedLockRoot'],
        harnessDigest=prepared['harnessManifestSha256'],seconds=seconds,submit=submit))
    binding,roots=_binding(packet)
    _project_layout(roots)
    payload=json.dumps(packet,sort_keys=True,separators=(',',':')).encode()
    if len(payload)>MAX_CONTROL: raise ValueError('SSH_CONTROL_SIZE')
    stage=roots[0]/'.incoming'/_digest({'manifest':manifest,'binding':binding})[7:]
    deadline=time.monotonic()+seconds
    def remaining():
        left=deadline-time.monotonic()
        if left<=0: raise TimeoutError('SSH_TRANSPORT_DEADLINE')
        return left
    operator=profile['runtime'].get('operatorPython','/usr/bin/python3')
    def remote(phase):
        command=['/usr/bin/ssh','-o','BatchMode=yes','-o','ConnectTimeout=10','itiger',
            shlex.join([operator,'-B','-c',BOOTSTRAP,phase])]
        result=subprocess.run(command,input=payload,check=True,capture_output=True,timeout=remaining())
        if len(result.stdout)+len(result.stderr)>4*1024*1024: raise ValueError('SSH_CONTROL_OUTPUT')
        return json.loads(result.stdout)
    attempt=Path(prepared['plan']['output'])/('transport-attempt-'+str(time.time_ns())+'.json')
    try:
        preview=remote('preflight')
        if (not isinstance(preview,dict) or set(preview)!={'schema','manifestDigest','stage','needed','complete'}
                or preview.get('schema')!='tiger-yolo-ssh-preflight-v1'
                or preview.get('manifestDigest')!=_digest(manifest) or preview.get('stage')!=str(stage)
                or not isinstance(preview.get('needed'),list) or type(preview['complete']) is not bool
                or (preview['complete'] and preview['needed'])):
            raise ValueError('SSH_PREFLIGHT_BINDING')
        known={row['path']:row for row in manifest['files']}; seen=set()
        for row in preview['needed']:
            original=known.get(row['path']) if isinstance(row,dict) and isinstance(row.get('path'),str) else None
            if (original is None or row!=dict(original,blob=str(stage/'blobs'/original['sha256'][7:]))
                    or row['sha256'] in seen):
                raise ValueError('SSH_NEEDED_FILE_BINDING')
            seen.add(row['sha256'])
        for row in preview['needed']:
            command=['/usr/bin/rsync','--partial','--append-verify','--protect-args','--perms','--chmod=F600',
                '-e','/usr/bin/ssh -o BatchMode=yes -o ConnectTimeout=10','--',row['path'],'itiger:'+row['blob']]
            subprocess.run(command,check=True,capture_output=True,timeout=remaining())
        outcome=remote('finish')
        if (not isinstance(outcome,dict) or type(outcome.get('exitCode')) is not int
                or outcome.get('manifestDigest')!=_digest(manifest) or outcome.get('stage')!=str(stage)
                or not isinstance(outcome.get('stdout',''),str) or not isinstance(outcome.get('stderr',''),str)
                or outcome.get('qualification')!='NOT_EVALUATED'):
            raise ValueError('SSH_FINISH_REPLY')
        _write_record(attempt,dict(stage=str(stage),manifestDigest=_digest(manifest),outcome=outcome))
        return outcome
    except (OSError,ValueError,subprocess.SubprocessError) as exc:
        error=getattr(exc,'stderr',None) or b''
        if isinstance(error,bytes): error=error.decode('utf-8',errors='replace')
        _write_record(attempt,dict(status='REMOTE_STATE_UNRESOLVED',stage=str(stage),
            manifestDigest=_digest(manifest),errorType=type(exc).__name__,
            stderr=error[:65536]))
        raise
