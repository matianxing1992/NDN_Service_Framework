"""Explicit inventory against filesystem fixtures, never runtime qualification."""
import hashlib
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from runtime.yolo_transport import candidate_inventory
from runtime.yolo_bundle import REQUIRED_HARNESS_FILES


@pytest.fixture
def candidate(tmp_path):
    artifacts, runs = tmp_path/'artifacts', tmp_path/'runs'
    def file(path, payload=b'fixture'):
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(payload); path.chmod(0o444)
        return dict(path=str(path),bytes=len(payload),sha256='sha256:'+hashlib.sha256(payload).hexdigest())
    def doc(path,value): return file(path,json.dumps(value,sort_keys=True).encode())
    def bundle(path):
        rows={}
        for name in REQUIRED_HARNESS_FILES:
            ref=file(path/name,name.encode()); rows[name]={k:ref[k] for k in ('bytes','sha256')}
        return doc(path/'harness-manifest.json',{'schema':'tiger-yolo-harness-v1','files':rows})
    harness=bundle(artifacts/'harness')
    profile=dict(storage=dict(remoteArtifactRoot=str(artifacts),sharedRunRoot=str(runs)),
        evidence={'harnessManifest':harness},release={})
    for name in ('inputs','runtime','dispatch'):
        ref=file(artifacts/name/'payload')
        profile['release'][name]=doc(artifacts/name/'plane.json',{'files':{'payload':dict(ref,path='payload')}})
    current=runs/'current'
    run_bundle=bundle(current/'bundle')
    prepared=dict(candidateDigest='sha256:'+'c'*64,plan={'output':str(current)},
                  bundle=str(current/'bundle'),harnessManifestSha256=run_bundle['sha256'])
    doc(current/'prepare.json',prepared)
    package=artifacts/'package'
    graph=file(package/'canonical/yolo26n.onnx',b'graph')
    weights=file(package/'canonical/yolo26n.weights',b'weights')
    oracle=file(package/'oracle/output.npy',b'oracle')
    fixture=file(artifacts/'repository/tests/fixture.ppm',b'fixture')
    model=dict(graph=dict(graphBytes=graph['bytes'],graphDigest=graph['sha256']),
        weights=dict(bytes=weights['bytes'],digest=weights['sha256'],path='canonical/yolo26n.weights'),
        oracle=dict(outputPath='output.npy',outputDigest=oracle['sha256']),
        fixture=dict(path='tests/fixture.ppm',sha256=fixture['sha256'][7:]))
    profile['workload']={'packageManifest':doc(package/'manifest.json',model)}
    public=file(artifacts/'trust/authority.pub',b'public')
    secret=artifacts/'private/authority.key'; file(secret,b'synthetic key fixture'); secret.chmod(0o600)
    provision=dict(publicInputs={'key':public},authorityPrivateKey=str(secret),package=str(package))
    old=runs/'previous'; old_bundle=bundle(old/'bundle')
    doc(old/'prepare.json',dict(case='local-cpu',bundle=str(old/'bundle'),
        harnessManifestSha256=old_bundle['sha256']))
    log=file(old/'node0/logs/BackboneNeck.log',b'log fixture')
    doc(old/'node0/node-receipt.json',{'launches':[dict(logPath='logs/BackboneNeck.log',
        logBytes=log['bytes'],logDigest=log['sha256'])]})
    file(old/'node0/BackboneNeck/trace.json',b'profile fixture')
    # This directory is intentionally NOT part of the public collector closure.
    file(old/'node0/BackboneNeck/.ndn/private.db',b'do not transport')
    requests=[]
    for i in range(2):
        for name in ('graph-reference.json','lifecycle.jsonl','yolo-numerical.json',
                     'yolo-response.bin','yolo-public-assignments.json'):
            file(old/'node0/user/requests'/str(i)/name)
        requests.append({'execution':{'roles':{'BackboneNeck':dict(rank=0,
            native={'observation':{'providerProfilePath':'/output/trace.json'}})}}})
    doc(old/'collection-input.json',dict(kind='normal',nodes={'0':{'root':'node0'}},
        references=[dict(package=os.path.relpath(package,old),repository=str(artifacts/'repository'))]))
    verdict={'requestResults':requests}
    verdict_ref=doc(old/'verdict.json',verdict)
    gates={'localSif':dict(path=verdict_ref['path'],receipt=verdict)}
    profile['release']['gates']={'localSif':verdict_ref}
    path=artifacts/'profile.json'; doc(path,profile)
    return path,profile,prepared,provision,gates,secret


def test_inventory_covers_transitive_inputs_and_retained_results_without_private_homes(candidate):
    path,profile,prepared,provision,gates,secret=candidate
    result=candidate_inventory(path,profile,prepared,provision=provision,gates=gates)
    rows={row['path']:row for row in result['files']}
    assert rows[str(secret)]['private'] and rows[str(secret)]['mode']==0o600
    assert sum(row['private'] for row in rows.values())==1
    assert not any('.ndn' in name for name in rows)
    for suffix in ('canonical/yolo26n.onnx','canonical/yolo26n.weights','oracle/output.npy',
                   'repository/tests/fixture.ppm','node0/logs/BackboneNeck.log',
                   'node0/BackboneNeck/trace.json','node0/user/requests/1/yolo-response.bin'):
        assert any(name.endswith(suffix) for name in rows),suffix
    assert result['candidateDigest']==prepared['candidateDigest']


def test_host_and_gpu_prerequisite_specific_files_are_included(candidate):
    path,profile,prepared,provision,gates,_=candidate
    artifacts=Path(profile['storage']['remoteArtifactRoot'])
    old=Path(gates['localSif']['path']).parent
    def put(path,value):
        payload=json.dumps(value).encode()
        path.parent.mkdir(parents=True,exist_ok=True)
        if path.exists(): path.chmod(0o600)
        path.write_bytes(payload); path.chmod(0o444)
        return dict(path=str(path),bytes=len(payload),sha256='sha256:'+hashlib.sha256(payload).hexdigest())
    saved=json.loads((old/'prepare.json').read_text()); saved['case']='single-node-gpu'
    put(old/'prepare.json',saved)
    required=[old/'allocation-terminal.json',old/'srun-cleanup.json',old/'storage-rank0.json',
              old/'node0/slurm-allocation.json',old/'node0/gpu-probe.json']
    for file in required: put(file,{'fixture':'GPU-specific retained record'})
    seal=put(artifacts/'host/source-seal.json',{'fixture':'source seal'})
    evidence=put(artifacts/'host/normal/log.json',{'fixture':'host evidence'})
    host={'sourceSeal':seal,'cases':{'normal':{'evidence':{'log':evidence}}}}
    host_ref=put(artifacts/'host/host-gate.json',host)
    gates['hostMinindn']=dict(path=host_ref['path'],receipt=host)
    result=candidate_inventory(path,profile,prepared,provision=provision,gates=gates)
    names={row['path'] for row in result['files']}
    assert set(map(str,required)) <= names
    assert {seal['path'],evidence['path'],host_ref['path']} <= names


@pytest.mark.parametrize('fault',['weights','log','harness','outside','missing-request'])
def test_changed_or_missing_inventory_is_rejected(candidate,fault,tmp_path):
    path,profile,prepared,provision,gates,_=candidate
    old=Path(gates['localSif']['path']).parent
    if fault=='weights': target=Path(provision['package'])/'canonical/yolo26n.weights'
    elif fault=='log': target=old/'node0/logs/BackboneNeck.log'
    elif fault=='harness': target=Path(prepared['bundle'])/'runtime/worker.py'
    elif fault=='missing-request': target=old/'node0/user/requests/1/yolo-response.bin'
    else:
        provision['authorityPrivateKey']=str(tmp_path/'outside')
        target=None
    if target:
        target.chmod(0o600)
        if fault=='missing-request': target.unlink()
        else: target.write_bytes(b'changed')
    with pytest.raises((ValueError,OSError)):
        candidate_inventory(path,profile,prepared,provision=provision,gates=gates)


@pytest.mark.parametrize('qualified',[False,True])
def test_public_transport_plan_requires_gate_and_never_invokes_scheduler(tmp_path,monkeypatch,capsys,qualified):
    from test_yolo_submit import submit_module
    from runtime import yolo_profile,yolo_transport
    module=submit_module()
    digest='sha256:'+'a'*64
    saved=dict(case='single-node-gpu',profileDigest=digest,contentIdentities={},
               candidateDigest=digest,plan={})
    profile={'release':{'gates':{}}}
    args=SimpleNamespace(case='single-node-gpu',profile=tmp_path/'profile.json',
                         output=tmp_path/'runs',run_id='plan-run',plan_transport=True)
    monkeypatch.setattr(module,'_dispatch_report',lambda _: (dict(integrity='VERIFIED',
        documentDigest=digest,identities={}),profile))
    monkeypatch.setattr(module,'_load_prepared',lambda *a: saved)
    monkeypatch.setattr(module,'_enter_frozen',lambda *a: None)
    events=[]
    def gate(*a,**k):
        events.append('gate')
        if not qualified: raise module.ClosureError('GATE_NOT_QUALIFIED:localSif')
        return {'fixture':'qualified gate double'}
    monkeypatch.setattr(module,'_gate_receipt',gate)
    monkeypatch.setattr(yolo_profile,'resolve_provision_inputs',lambda *a,**k: {})
    def inventory(*a,**k):
        assert events==['gate']
        events.append('inventory')
        return {'fixture':'inventory double'}
    monkeypatch.setattr(yolo_transport,'candidate_inventory',inventory)
    monkeypatch.setattr(module.subprocess,'run',lambda *a,**k: pytest.fail('external call'))
    if qualified:
        assert module._submit(args)==78
        assert json.loads(capsys.readouterr().out)['qualification']=='NOT_EVALUATED'
        assert events==['gate','inventory']
    else:
        with pytest.raises(module.ClosureError,match='GATE_NOT_QUALIFIED'):
            module._submit(args)
        assert events==['gate']
