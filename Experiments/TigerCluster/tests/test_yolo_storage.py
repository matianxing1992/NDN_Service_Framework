"""Real bounded filesystem, fsync, AF_UNIX, copy and cleanup; no SIF execution."""
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from test_yolo_submit import submit_module
from runtime import yolo_storage as storage


@pytest.fixture
def inputs(tmp_path, monkeypatch):
    monkeypatch.delenv('SLURM_TMPDIR', raising=False)
    root = tmp_path/'run'
    root.mkdir()
    scratch = tmp_path/'scratch'
    scratch.mkdir()
    source = tmp_path/'image.sif'
    source.write_bytes(b'fixture bytes, not an executable SIF\n'*100)
    runtime = dict(sif=str(source), sifBytes=source.stat().st_size,
                   sifSha256=hashlib.sha256(source.read_bytes()).hexdigest())
    profile = dict(storage=dict(scratchRoot=str(scratch), peakBytes=runtime['sifBytes']+4096, marginBytes=4096),
                   timing=dict(stagingSeconds=5))
    prepared = dict(runId='storage-test', candidateDigest='sha256:'+'a'*64,
        case='single-node-gpu', plan=dict(output=str(root)))
    allocation = dict(receipt=dict(rank=0, jobId='123', hostname='n0'))
    return prepared, profile, runtime, allocation, root, source


def instance(values):
    prepared, profile, runtime, allocation, _, _ = values
    return storage.NodeScratch(prepared=prepared, profile=profile, runtime_profile=runtime,
                               allocation=allocation, rank=0)


def test_real_copy_socket_and_cleanup_are_bound_and_durable(inputs):
    prepared, _, runtime, _, root, source = inputs
    with instance(inputs) as scratch:
        path = scratch.path
        copied = Path(scratch.runtime_profile['sif'])
        assert copied.read_bytes()==source.read_bytes()
        assert copied.stat().st_ino != source.stat().st_ino
        assert copied.stat().st_mode & 0o222 == 0
        assert scratch.node.parent == path
        assert json.loads((root/'storage-rank0-start.json').read_text())['socketVerified']
        assert not (root/'storage-rank0.json').exists()
    assert not path.exists() and source.exists()
    rows=storage.verify_storage_cleanup(root, prepared, '123')
    assert rows[0]['sifSha256']=='sha256:'+runtime['sifSha256']
    assert rows[0]['outputCapacity']['quotaVerified'] is False
    with pytest.raises(ValueError, match='STORAGE_ALREADY_USED'):
        instance(inputs)


def test_workload_failure_retains_image_for_possible_unreaped_process(inputs):
    prepared, _, _, _, root, _=inputs
    with pytest.raises(RuntimeError, match='WORKER_FAILED'):
        with instance(inputs) as scratch:
            path=scratch.path
            raise RuntimeError('WORKER_FAILED')
    assert (path/'runtime.sif').is_file()
    assert json.loads((root/'storage-rank0.json').read_text())['status']=='RETAINED_ON_FAILURE'
    with pytest.raises(ValueError, match='STORAGE_CLEANUP_BINDING'):
        storage.verify_storage_cleanup(root, prepared, '123')


@pytest.mark.parametrize('fault', ['hash','size','budget','capacity','filesystem','symlink'])
def test_staging_rejects_before_native_and_retains_failure_record(inputs, monkeypatch, fault):
    prepared, profile, runtime, _, root, source=inputs
    if fault=='hash': runtime['sifSha256']='b'*64
    elif fault=='size': runtime['sifBytes']+=1
    elif fault=='budget': profile['storage']['peakBytes']=1
    elif fault=='capacity':
        monkeypatch.setattr(storage.os, 'statvfs', lambda p: SimpleNamespace(f_bavail=0,f_frsize=4096,f_files=100,f_favail=100))
    elif fault=='filesystem':
        monkeypatch.setattr(storage.subprocess, 'run', lambda *a, **k: SimpleNamespace(stdout=b'nfs\n'))
    elif fault=='symlink':
        alias=source.parent/'alias.sif'
        alias.symlink_to(source)
        runtime['sif']=str(alias)
    with pytest.raises((ValueError,OSError)):
        with instance(inputs): pytest.fail('bad input entered the execution body')
    assert json.loads((root/'storage-rank0.json').read_text())['status']=='STAGING_FAILED'
    assert list(Path(profile['storage']['scratchRoot']).iterdir())==[]


def test_slurm_tmpdir_is_measured_instead_of_silent_shared_storage(inputs, monkeypatch):
    _, profile, _, _, root, _=inputs
    allocated=root/'allocated-tmp'
    allocated.mkdir()
    monkeypatch.setenv('SLURM_TMPDIR', str(allocated))
    with instance(inputs) as scratch:
        assert scratch.path.parent==allocated
        assert scratch.record['scratchCapacity']['path']==str(allocated)


def test_cleanup_failure_is_not_qualified(inputs, monkeypatch):
    prepared, _, _, _, root, _=inputs
    with pytest.raises(OSError):
        with instance(inputs):
            monkeypatch.setattr(storage.shutil,'rmtree',lambda *a, **k: (_ for _ in ()).throw(OSError('busy')))
    assert json.loads((root/'storage-rank0.json').read_text())['status']=='CLEANUP_FAILED'
    with pytest.raises(ValueError): storage.verify_storage_cleanup(root,prepared,'123')


@pytest.mark.parametrize('fault', ['job','filesystem','capacity'])
def test_reanalysis_rejects_changed_storage_evidence(inputs, fault):
    prepared, _, _, _, root, _=inputs
    with instance(inputs): pass
    path=root/'storage-rank0.json'
    value=json.loads(path.read_text())
    if fault=='job': value['jobId']='456'
    elif fault=='filesystem': value['filesystem']='nfs'
    else: value['outputCapacity']['freeBytes']=0
    path.chmod(0o600)
    path.write_text(json.dumps(value))
    with pytest.raises(ValueError): storage.verify_storage_cleanup(root,prepared,'123')


@pytest.mark.parametrize('fault', ['none','host','image'])
def test_two_rank_storage_requires_distinct_hosts_and_same_image(inputs, fault):
    prepared, profile, runtime, allocation, root, _=inputs
    prepared['case']='two-node-gpu'
    with instance(inputs): pass
    second=dict(receipt=dict(rank=1,jobId='123',hostname='n1'))
    with storage.NodeScratch(prepared=prepared,profile=profile,runtime_profile=runtime,
                             allocation=second,rank=1): pass
    path=root/'storage-rank1.json'
    value=json.loads(path.read_text())
    if fault=='host': value['hostname']='n0'
    if fault=='image': value['sifSha256']='sha256:'+'b'*64
    if fault!='none':
        path.chmod(0o600)
        path.write_text(json.dumps(value))
        with pytest.raises(ValueError,match='STORAGE_NODE_OR_SIF_BINDING'):
            storage.verify_storage_cleanup(root,prepared,'123')
    else:
        assert len(storage.verify_storage_cleanup(root,prepared,'123'))==2
