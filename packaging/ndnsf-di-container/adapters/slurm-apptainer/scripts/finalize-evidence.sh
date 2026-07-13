#!/bin/sh
set -eu
exec python3 - "$@" <<'PY'
import argparse,hashlib,json,os,shutil,sys,tempfile
p=argparse.ArgumentParser()
for name in ('staging','destination','run-id','state'): p.add_argument('--'+name,required=True)
p.add_argument('--exit-code',required=True,type=int); a=p.parse_args()
src=os.path.abspath(a.staging); dst=os.path.abspath(a.destination)
if os.path.exists(dst): print('EVIDENCE_DESTINATION_EXISTS',file=sys.stderr);sys.exit(a.exit_code or 6)
os.makedirs(src,exist_ok=True);os.makedirs(os.path.dirname(dst),exist_ok=True)
terminal={'runId':a.run_id,'state':a.state,'originalExitCode':a.exit_code,'physicalProduction':'DEFERRED'}
open(os.path.join(src,'terminal.json'),'w').write(json.dumps(terminal,indent=2,sort_keys=True)+'\n')
stage=tempfile.mkdtemp(prefix='.'+os.path.basename(dst)+'.',dir=os.path.dirname(dst))
try:
    files={}
    for name in sorted(os.listdir(src)):
        path=os.path.join(src,name)
        if os.path.isfile(path):
            shutil.copy2(path,os.path.join(stage,name));files[name]='sha256:'+hashlib.sha256(open(path,'rb').read()).hexdigest()
    open(os.path.join(stage,'promotion-manifest.json'),'w').write(json.dumps({'runId':a.run_id,'files':files},indent=2,sort_keys=True)+'\n')
    os.replace(stage,dst)
except Exception:
    shutil.rmtree(stage,ignore_errors=True);raise
sys.exit(a.exit_code)
PY
