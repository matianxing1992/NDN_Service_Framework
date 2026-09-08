"""SSH control and real filesystem boundaries; no qualification or Slurm jobs."""
import base64
import copy
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from runtime import yolo_ssh as ssh
from runtime.yolo_bundle import MANIFEST, REQUIRED_HARNESS_FILES, verify_harness
from runtime.yolo_submission import SubmissionJournal
from runtime.yolo_transport import inventory
from tools.spec183_dispatch_plane import _sealed_harness


@pytest.fixture
def transfer(tmp_path,monkeypatch):
    artifacts,runs,locks=(tmp_path/name for name in ('artifacts','runs','locks'))
    for root in (artifacts,runs,locks): root.mkdir()
    _sealed_harness(tmp_path)
    source=tmp_path/'harness'
    digest='sha256:'+ssh.hashlib.sha256((source/MANIFEST).read_bytes()).hexdigest()
    run=runs/'ssh-run'; run.mkdir()
    bundle=run/'bundle'; shutil.copytree(source,bundle)
    profile_path=artifacts/'profile.json'; profile_path.write_text('{}'); profile_path.chmod(0o644)
    prepared=dict(runId='ssh-run',case='single-node-gpu',candidateDigest='sha256:'+'c'*64,
                  bundle=str(bundle),harnessManifestSha256=digest,plan={'output':str(run)})
    (run/'prepare.json').write_text(json.dumps(prepared))
    (run/'prepare.json').chmod(0o644)
    payload=artifacts/'payload'; payload.write_bytes(b'0123456789'*100); payload.chmod(0o444)
    manifest=inventory([profile_path,run/'prepare.json',payload,
                       *(bundle/name for name in REQUIRED_HARNESS_FILES|{MANIFEST})],
                       roots=(artifacts,runs),candidate_digest=prepared['candidateDigest'])
    packet=dict(schema='tiger-yolo-ssh-v1',manifest=manifest,
        binding=dict(profile=str(profile_path),runId='ssh-run',case='single-node-gpu',bundle=str(bundle),
                     lockRoot=str(locks),harnessDigest=digest,seconds=30,submit=False),
        bootstrap={name:base64.b64encode((bundle/name).read_bytes()).decode()
                   for name in REQUIRED_HARNESS_FILES|{MANIFEST}})
    stage=artifacts/'.incoming'/ssh._digest({'manifest':manifest,'binding':packet['binding']})[7:]
    stage.mkdir(parents=True); shutil.copytree(source,stage/'bootstrap')
    (stage/'blobs').mkdir()
    monkeypatch.setattr(ssh,'_site',lambda *a:None)
    profile=dict(storage=dict(sharedLockRoot=str(locks),transferTimeoutSeconds=30),
                 runtime={'operatorPython':'/project/env with space/bin/python'})
    return SimpleNamespace(packet=packet,stage=stage,prepared=prepared,profile=profile,
        profile_path=profile_path,payload=payload,locks=locks,source=source)


def stage_and_remove(f):
    # Model independent filesystems using retained payload blobs and missing finals.
    for row in f.packet['manifest']['files']:
        path=Path(row['path'])
        shutil.copyfile(path,f.stage/'blobs'/row['sha256'][7:])
        path.parent.chmod(0o700)
        path.unlink()


def test_receiver_publication_seals_harness_and_readonly_retry_reaches_active_journal(transfer,monkeypatch):
    f=transfer; stage_and_remove(f)
    preview=ssh.remote_phase(f.packet,'preflight',f.stage)
    assert preview['needed']==[] and not preview['complete']
    result=ssh.remote_phase(f.packet,'finish',f.stage)
    assert result['status']=='STAGED' and result['qualification']=='NOT_EVALUATED'
    verify_harness(Path(f.prepared['bundle']),expected_manifest_sha256=f.prepared['harnessManifestSha256'])
    before={row['path']:Path(row['path']).stat().st_ino for row in f.packet['manifest']['files']}
    journal=SubmissionJournal(f.locks,candidate_id='sha256:'+'c'*64,gate='single-node-gpu')
    journal.reserve('ssh-run')
    assert ssh.remote_phase(f.packet,'preflight',f.stage)['complete']
    assert ssh.remote_phase(f.packet,'finish',f.stage)['status']=='STAGED'
    assert before=={name:Path(name).stat().st_ino for name in before}


def test_partial_or_corrupt_blob_resumes_but_active_namespace_blocks_publication(transfer):
    f=transfer; original=f.payload.read_bytes(); f.payload.unlink()
    row=next(r for r in f.packet['manifest']['files'] if r['path']==str(f.payload))
    blob=f.stage/'blobs'/row['sha256'][7:]; blob.write_bytes(original[:21])
    assert ssh.remote_phase(f.packet,'preflight',f.stage)['needed']==[dict(row,blob=str(blob))]
    assert blob.read_bytes()==original[:21]
    blob.write_bytes(b'X'*len(original))
    assert ssh.remote_phase(f.packet,'preflight',f.stage)['needed']
    assert not blob.exists() and len(list(blob.parent.glob('*.rejected-*')))==1
    journal=SubmissionJournal(f.locks,candidate_id='sha256:'+'c'*64,gate='single-node-gpu')
    journal.reserve('other-run')
    with pytest.raises(ValueError,match='TRANSPORT_ACTIVE_SUBMISSION'):
        ssh.remote_phase(f.packet,'preflight',f.stage)
    assert not f.payload.exists()


def test_finish_calls_only_existing_frozen_submit_owner(transfer,monkeypatch):
    f=transfer; f.packet['binding']['submit']=True
    stage=f.stage.parent/ssh._digest({'manifest':f.packet['manifest'],'binding':f.packet['binding']})[7:]
    f.stage.rename(stage)
    commands=[]
    def run(argv,**kwargs):
        commands.append(argv)
        assert kwargs['timeout']==30
        return SimpleNamespace(returncode=78,stdout=b'{"status":"SUBMISSION_UNKNOWN"}\n',stderr=b'')
    monkeypatch.setattr(ssh.subprocess,'run',run)
    outcome=ssh.remote_phase(f.packet,'finish',stage)
    assert len(commands)==1 and commands[0][-1]=='--remote-receiver'
    assert commands[0][2]==str(Path(f.prepared['bundle'])/'jobs/yolo/submit.py')
    assert outcome['exitCode']==78 and outcome['manifestDigest']==ssh._digest(f.packet['manifest'])
    assert len(list(stage.glob('submit-observation-*.json')))==1


def test_complete_files_with_writable_harness_need_sealing_before_submit(transfer):
    f=transfer; bundle=Path(f.prepared['bundle']); bundle.chmod(0o755)
    assert not ssh.remote_phase(f.packet,'preflight',f.stage)['complete']
    ssh.remote_phase(f.packet,'finish',f.stage)
    assert not bundle.stat().st_mode&0o222


def test_extra_harness_file_rejected_without_permission_mutation(transfer):
    f=transfer; bundle=Path(f.prepared['bundle']); bundle.chmod(0o755)
    (bundle/'unexpected').write_text('unexpected')
    with pytest.raises(ValueError,match='SSH_HARNESS_EXTRA_CONTENT'):
        ssh.remote_phase(f.packet,'finish',f.stage)
    assert bundle.stat().st_mode&0o222


@pytest.mark.parametrize('fault',['hash','path','duplicate','valid-loader'])
def test_real_bootstrap_rejects_bad_control_and_seals_valid_source_before_dispatch(transfer,fault):
    f=transfer; packet=copy.deepcopy(f.packet)
    if fault=='hash': packet['bootstrap']['runtime/yolo_ssh.py']=base64.b64encode(b'changed').decode()
    if fault=='path': packet['bootstrap']['../escape']=base64.b64encode(b'x').decode()
    raw=json.dumps(packet).encode()
    if fault=='duplicate': raw=b'{"schema":0,'+raw[1:]
    # Deliberately invalid phase stops the real received module before site calls.
    result=subprocess.run([sys.executable,'-B','-c',ssh.BOOTSTRAP,'invalid-phase'],
                          input=raw,capture_output=True,timeout=10)
    assert result.returncode!=0 and not result.stdout
    assert (b'SSH_STAGE_BINDING' if fault=='valid-loader' else b'BOOTSTRAP_' if fault!='duplicate'
            else b'DUPLICATE_CONTROL_KEY') in result.stderr
    if fault=='valid-loader':
        verify_harness(f.stage/'bootstrap',expected_manifest_sha256=f.prepared['harnessManifestSha256'])


@pytest.mark.parametrize('fault',[None,'second-row','path-type','timeout','wrong-finish'])
def test_sender_validates_whole_reply_before_rsync_and_records_uncertain_outcome(transfer,monkeypatch,fault):
    f=transfer
    # Only the project-prefix requirement is replaced for this tmp filesystem test.
    monkeypatch.setattr(ssh,'_project_layout',lambda roots:None)
    commands=[]
    def run(argv,**kwargs):
        commands.append(argv)
        assert 0<kwargs['timeout']<=30
        if argv[0]=='/usr/bin/rsync':
            assert {'--partial','--append-verify','--protect-args','--perms','--chmod=F600'}<=set(argv)
            return SimpleNamespace(returncode=0,stdout=b'',stderr=b'')
        remote=shlex.split(argv[-1])
        assert remote[0]==f.profile['runtime']['operatorPython'] and remote[3]==ssh.BOOTSTRAP
        packet=json.loads(kwargs['input'])
        stage=Path(packet['manifest']['roots'][0])/'.incoming'/ssh._digest(
            {'manifest':packet['manifest'],'binding':packet['binding']})[7:]
        if fault=='timeout': raise subprocess.TimeoutExpired(argv,30)
        if remote[-1]=='preflight':
            row=packet['manifest']['files'][0]
            needed=[dict(row,blob=str(stage/'blobs'/row['sha256'][7:]))]
            if fault=='second-row': needed.append({'path':'/etc/passwd'})
            if fault=='path-type': needed.append({'path':[]})
            value=dict(schema='tiger-yolo-ssh-preflight-v1',manifestDigest=ssh._digest(packet['manifest']),
                       stage=str(stage),needed=needed,complete=False)
        else:
            value=dict(status='REMOTE_SUBMIT_RETURNED',qualification='NOT_EVALUATED',exitCode=78,
                manifestDigest=ssh._digest(packet['manifest']),stage=str(stage),stdout='',stderr='')
            if fault=='wrong-finish': value['stage']='/wrong'
        return SimpleNamespace(stdout=json.dumps(value).encode(),stderr=b'')
    monkeypatch.setattr(ssh.subprocess,'run',run)
    def coordinate():
        return ssh.coordinate(f.packet['manifest'],profile_path=f.profile_path,
                              profile=f.profile,prepared=f.prepared)
    if fault:
        with pytest.raises((ValueError,subprocess.TimeoutExpired)): coordinate()
    else: assert coordinate()['exitCode']==78
    records=list(Path(f.prepared['plan']['output']).glob('transport-attempt-*.json'))
    assert len(records)==1
    record=json.loads(records[0].read_text())
    if fault: assert record['status']=='REMOTE_STATE_UNRESOLVED'
    if fault in ('second-row','path-type','timeout'): assert len(commands)==1


def test_sender_refuses_nonproject_layout(transfer):
    with pytest.raises(ValueError,match='SSH_PROJECT_LAYOUT_REQUIRED'):
        ssh.coordinate(transfer.packet['manifest'],profile_path=transfer.profile_path,
                       profile=transfer.profile,prepared=transfer.prepared)


def test_real_rsync_resumes_prefix_and_keeps_private_writable_staging(tmp_path):
    source=tmp_path/'readonly-source'; target=tmp_path/'partial'
    source.write_bytes(bytes(range(256))*1024); source.chmod(0o444)
    target.write_bytes(source.read_bytes()[:12345]); target.chmod(0o600)
    result=subprocess.run(['/usr/bin/rsync','--partial','--append-verify','--protect-args',
        '--perms','--chmod=F600','--',str(source),str(target)],capture_output=True,timeout=10)
    assert result.returncode==0,result.stderr
    assert target.read_bytes()==source.read_bytes() and target.stat().st_mode&0o777==0o600
