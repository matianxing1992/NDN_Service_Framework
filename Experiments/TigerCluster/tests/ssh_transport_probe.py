#!/usr/bin/env python3
"""Tiny same-path local/Tiger transport probe in NEW roots; never submit a job."""
import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from runtime.yolo_bundle import MANIFEST, REQUIRED_HARNESS_FILES
from runtime.yolo_ssh import coordinate, _project_layout
from runtime.yolo_transport import _path, inventory
from tools.spec183_dispatch_plane import _sealed_harness


def probe(root,operator):
    root=_path(str(root)); _project_layout((root,))
    if not root.name.startswith('spec183-ssh-probe-'): raise ValueError('PROBE_ROOT_NAME')
    root.mkdir(mode=0o700)  # Never clear or reuse an existing probe root.
    artifacts,runs,locks=(root/name for name in ('artifacts','runs','locks'))
    for path in (artifacts,runs,locks): path.mkdir(mode=0o700)
    _sealed_harness(root)
    run=runs/'fixture-run'; run.mkdir()
    bundle=run/'bundle'; shutil.copytree(root/'harness',bundle)
    import hashlib
    digest='sha256:'+hashlib.sha256((bundle/MANIFEST).read_bytes()).hexdigest()
    prepared=dict(runId=run.name,case='single-node-gpu',candidateDigest='sha256:'+'f'*64,
        bundle=str(bundle),harnessManifestSha256=digest,plan={'output':str(run)})
    (run/'prepare.json').write_text(json.dumps(prepared)); (run/'prepare.json').chmod(0o644)
    profile=dict(storage=dict(sharedLockRoot=str(locks),transferTimeoutSeconds=600),
                 runtime=dict(operatorPython=operator))
    profile_path=artifacts/'fixture-profile.json'
    profile_path.write_text(json.dumps(profile)); profile_path.chmod(0o644)
    payload=artifacts/'synthetic-payload.txt'
    payload.write_bytes(b'Spec183 transport fixture, no SIF or model.\n'*32); payload.chmod(0o444)
    manifest=inventory([profile_path,run/'prepare.json',payload,
                       *(bundle/name for name in REQUIRED_HARNESS_FILES|{MANIFEST})],
                       roots=(artifacts,runs),candidate_digest=prepared['candidateDigest'])
    create="from pathlib import Path; import sys; p=Path(sys.argv[1]); p.mkdir(mode=0o700); [(p/n).mkdir(mode=0o700) for n in ('artifacts','runs','locks')]"
    subprocess.run(['/usr/bin/ssh','-o','BatchMode=yes','-o','ConnectTimeout=10','itiger',
                    shlex.join([operator,'-B','-c',create,str(root)])],check=True,timeout=30)
    outcomes=[]
    for _ in range(2):
        outcomes.append(coordinate(manifest,profile_path=profile_path,profile=profile,
                                   prepared=prepared,submit=False))
    assert all(row['status']=='STAGED' and row['qualification']=='NOT_EVALUATED' for row in outcomes)
    assert outcomes[0]['manifestDigest']==outcomes[1]['manifestDigest']
    result=dict(scope='LOGIN_NODE_TRANSPORT_ONLY',qualification='NOT_EVALUATED',
        root=str(root),files=len(manifest['files']),bytes=sum(row['bytes'] for row in manifest['files']),
        harnessDigest=digest,first=outcomes[0],retry=outcomes[1])
    (root/'probe-result.json').write_text(json.dumps(result,sort_keys=True))
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--operator-python',required=True)
    args=parser.parse_args()
    print(json.dumps(probe(args.root,args.operator_python),sort_keys=True))
