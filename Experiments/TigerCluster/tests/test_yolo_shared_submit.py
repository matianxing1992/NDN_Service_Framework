"""Receiver-side dispatch with real files/journal and explicit Slurm doubles."""
import os
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest

from test_yolo_allocated_runner import allocated
from test_yolo_submit import ROOT
from runtime.yolo_submission import SubmissionJournal, observe_submission


@pytest.fixture
def shared(allocated,monkeypatch):
    module,args,prepared,profile,_,root=allocated
    args.remote_receiver=True
    monkeypatch.delenv('SLURM_JOB_ID')
    artifact=root.parent.parent/'artifacts'
    artifact.mkdir()
    args.profile=artifact/'profile.json'
    args.profile.write_text('{}')
    key=artifact/'private-key'
    key.write_bytes(b'not a real key; path isolation fixture only')
    key.chmod(0o600)
    locks=root.parent.parent/'submission-locks'
    locks.mkdir()
    profile['storage'].update(remoteArtifactRoot=str(artifact),sharedLockRoot=str(locks),peakBytes=0,marginBytes=0)
    profile['security']={'authorityPrivateKey':str(key)}
    profile['cluster'].update(account='devs',memoryGiB=4)
    profile['timing']['progressTimeoutSeconds']=2
    wrapper=Path(prepared['bundle'])/'jobs/yolo/run.sbatch'
    wrapper.parent.mkdir(parents=True)
    wrapper.write_bytes((ROOT/'jobs/yolo/run.sbatch').read_bytes())
    wrapper.chmod(0o555)
    (Path(prepared['bundle'])/'requirements-operator.txt').write_bytes((ROOT/'requirements-operator.txt').read_bytes())
    journal=SubmissionJournal(locks,candidate_id=prepared['contentIdentities']['dispatch'],gate=args.case)
    events=[]
    monkeypatch.setattr(module,'_gate_receipt',lambda *a,**k: events.append('gate'))
    return module,args,prepared,profile,journal,root,events


def scheduler(shared,monkeypatch,*,response='ok',recovery='empty'):
    module,args,prepared,profile,journal,root,events=shared
    commands=[]
    def run(command,**kwargs):
        commands.append(command)
        assert kwargs['timeout']>0 and kwargs['env']=={'PATH':'/usr/bin:/bin','LC_ALL':'C'}
        if command[0]=='/usr/bin/scontrol':
            assert events[-1]=='gate'
            return SimpleNamespace(stdout=b'ClusterName              = itiger\n',stderr=b'',returncode=0)
        if command[0]=='/usr/bin/python3':
            assert command[-1]==str(Path(prepared['bundle'])/'requirements-operator.txt')
            return SimpleNamespace(stdout=b'OPERATOR_REQUIREMENTS_OK\n',stderr=b'',returncode=0)
        row=journal.get(args.run_id)
        if command[0]=='/usr/bin/sbatch':
            assert row['state']=='SUBMITTING' and (root/'submission-intent.json').is_file()
            assert '--no-requeue' in command and '--comment='+row['submissionKey'] in command
            if response=='timeout': raise subprocess.TimeoutExpired(command,2)
            if response=='concurrent-empty-query': journal.mark_unknown(args.run_id)
            return SimpleNamespace(stdout=b'123;itiger\n' if response!='bad' else b'123;other\n',
                                   stderr=b'',returncode=0)
        assert command[0] in ('/usr/bin/sacct','/usr/bin/squeue')
        raw=b''
        if recovery!='empty':
            ids=('123','456') if recovery=='duplicate' else ('123',)
            for job in ids:
                fields=[job,row['submissionKey'],str(os.getuid()),'bigTiger']
                if command[0]=='/usr/bin/sacct': fields.append('itiger')
                raw+=('|'.join(fields)+'\n').encode()
        return SimpleNamespace(stdout=raw,stderr=b'',returncode=0)
    monkeypatch.setattr(subprocess,'run',run)
    return commands


@pytest.mark.parametrize('response',['ok','concurrent-empty-query'])
def test_public_submit_records_before_once_only_sbatch_and_reuses_ack(shared,monkeypatch,response):
    module,args,prepared,profile,journal,root,_=shared
    commands=scheduler(shared,monkeypatch,response=response)
    assert module._submit(args)==0
    assert journal.get(args.run_id)['state']=='SUBMITTED'
    assert module._submit(args)==0
    assert sum(c[0]=='/usr/bin/sbatch' for c in commands)==1


@pytest.mark.parametrize('response',['timeout','bad'])
def test_uncertain_submission_queries_then_recovers_without_resubmitting(shared,monkeypatch,response):
    module,args,prepared,profile,journal,root,_=shared
    first=scheduler(shared,monkeypatch,response=response)
    assert module._submit(args)==78
    assert journal.get(args.run_id)['state']=='SUBMISSION_UNKNOWN'
    assert module._submit(args)==78
    assert sum(c[0]=='/usr/bin/sbatch' for c in first)==1
    later=scheduler(shared,monkeypatch,recovery='found')
    assert module._submit(args)==0
    assert journal.get(args.run_id)['jobId']=='123'
    assert all(c[0]!='/usr/bin/sbatch' for c in later)
    assert len(list(root.glob('submission-query-*.json')))==2


def test_duplicate_jobs_remain_unknown_and_do_not_launch_third(shared,monkeypatch):
    module,args,prepared,profile,journal,root,_=shared
    scheduler(shared,monkeypatch,response='timeout')
    assert module._submit(args)==78
    commands=scheduler(shared,monkeypatch,recovery='duplicate')
    with pytest.raises(module.ClosureError,match='DUPLICATE_SUBMISSION'):
        module._submit(args)
    assert journal.get(args.run_id)['state']=='SUBMISSION_UNKNOWN'
    assert all(c[0]!='/usr/bin/sbatch' for c in commands)


@pytest.mark.parametrize('fault',['profile','output','reference','key-mode','inside-job','site','python','wrapper-mode'])
def test_receiver_rejects_bad_staging_before_reserving_or_sbatch(shared,monkeypatch,fault):
    module,args,prepared,profile,journal,root,_=shared
    if fault=='profile': args.profile=root.parent/'outside.json'
    elif fault=='output': args.output=root.parent/'elsewhere'
    elif fault=='reference': profile['oracle']={'input':dict(path='/tmp/outside',bytes=1,sha256='sha256:'+'a'*64)}
    elif fault=='key-mode': Path(profile['security']['authorityPrivateKey']).chmod(0o644)
    elif fault=='inside-job': monkeypatch.setenv('SLURM_JOB_ID','999')
    elif fault=='wrapper-mode': (Path(prepared['bundle'])/'jobs/yolo/run.sbatch').chmod(0o444)
    commands=[]
    def run(command,**kwargs):
        commands.append(command)
        if fault=='python' and command[0]=='/usr/bin/python3':
            raise subprocess.CalledProcessError(1,command,stderr=b'ModuleNotFoundError')
        assert fault in ('site','python') and command==['/usr/bin/scontrol','show','config']
        return SimpleNamespace(stdout=b'ClusterName = other\n' if fault=='site' else b'ClusterName = itiger\n',stderr=b'',returncode=0)
    monkeypatch.setattr(subprocess,'run',run)
    if fault in ('profile','output','reference'):
        assert module._submit(args)==78
    else:
        with pytest.raises(module.ClosureError): module._submit(args)
    assert not journal.path.exists()
    assert all(c[0]!='/usr/bin/sbatch' for c in commands)


def test_matching_comment_with_wrong_owner_is_rejected(monkeypatch):
    key='spec183-'+'a'*64
    def run(command,**kwargs):
        return SimpleNamespace(stdout=('123|'+key+'|'+str(os.getuid()+1)+'|bigTiger|itiger\n').encode(),stderr=b'')
    monkeypatch.setattr(subprocess,'run',run)
    with pytest.raises(ValueError,match='SUBMISSION_QUERY_BINDING'):
        observe_submission(submission_key=key,partition='bigTiger',since='2026-09-07',seconds=2)


@pytest.mark.parametrize('fault',[False,True])
def test_local_submit_coordinates_transport_after_gates_without_local_sbatch(shared,monkeypatch,fault):
    from runtime import yolo_ssh
    module,args,prepared,profile,journal,root,events=shared
    args.remote_receiver=False
    original=Path.is_file
    monkeypatch.setattr(Path,'is_file',lambda p:False if str(p)=='/usr/bin/scontrol' else original(p))
    def manifest(*a):
        assert events==['gate']
        events.append('inventory')
        return {'fixture':'already gate-checked'}
    monkeypatch.setattr(module,'_transport_manifest',manifest)
    def coordinate(value,**kwargs):
        assert events==['gate','inventory'] and kwargs['prepared'] is prepared
        events.append('ssh')
        if fault: raise subprocess.TimeoutExpired('ssh',30)
        return dict(exitCode=78,stdout='receiver observation\n',stderr='')
    monkeypatch.setattr(yolo_ssh,'coordinate',coordinate)
    monkeypatch.setattr(module,'_submit_shared',lambda *a:pytest.fail('sender called receiver locally'))
    assert module._submit(args)==78
    assert events==['gate','inventory','ssh'] and not journal.path.exists()


def test_batch_waits_for_submitter_ack_without_claiming_its_own_job(allocated,monkeypatch):
    module,args,prepared,profile,_,root=allocated
    locks=root.parent.parent/'ack-locks'
    locks.mkdir()
    profile['storage']['sharedLockRoot']=str(locks)
    profile['timing']['progressTimeoutSeconds']=1
    journal=SubmissionJournal(locks,candidate_id=prepared['contentIdentities']['dispatch'],gate=args.case)
    journal.reserve(args.run_id)
    journal.mark_submitting(args.run_id)
    original=SubmissionJournal.get
    calls=[]
    def get(self,run):
        calls.append(run)
        if len(calls)==2: self.record_submission(run,'123')
        return original(self,run)
    monkeypatch.setattr(SubmissionJournal,'get',get)
    _,_,_,expected=module._allocated_context(args)
    assert expected['job_id']=='123' and len(calls)==2
