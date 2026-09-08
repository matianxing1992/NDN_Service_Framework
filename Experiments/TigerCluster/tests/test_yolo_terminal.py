"""Read-only scheduler seam plus real retained receipts/journal; no Slurm job."""
import json
import os
import subprocess
from types import SimpleNamespace

import pytest

from test_yolo_allocated_runner import allocated
from test_yolo_gate_reuse import gate_pair
from test_yolo_submit import file_ref
from runtime import yolo_allocation as allocation


def accounting(key, *, uid=1000, state='COMPLETED', code='0:0'):
    return ('123|'+state+'|'+code+'|'+key+'|'+str(uid)+'|bigTiger|itiger\n').encode()


def test_terminal_completed_is_identity_evidence_only():
    key='spec183-'+'a'*64
    receipt=allocation.validate_terminal_allocation(accounting(key), b'',
        job_id='123',submission_key=key,partition='bigTiger',uid=1000)
    assert receipt['state']=='COMPLETED' and 'qualification' not in receipt


@pytest.mark.parametrize('fault', ['missing','duplicate','running','truncated','comment',
    'job','uid','partition','cluster','exit','queued','comment-queued','queue-format'])
def test_terminal_rejects_ambiguous_or_live_evidence(fault):
    key='spec183-'+'a'*64
    raw=accounting(key)
    queue=b''
    if fault=='missing': raw=b''
    elif fault=='duplicate': raw+=raw
    elif fault=='running': raw=accounting(key,state='RUNNING')
    elif fault=='truncated': raw=accounting(key,state='OUT_OF_ME+')
    elif fault=='comment': raw=raw.replace(key.encode(),b'wrong')
    elif fault=='job': raw=raw.replace(b'123|',b'124|')
    elif fault=='uid': raw=raw.replace(b'1000|',b'1001|')
    elif fault=='partition': raw=raw.replace(b'bigTiger',b'smallTiger')
    elif fault=='cluster': raw=raw.replace(b'itiger',b'other')
    elif fault=='exit': raw=accounting(key,code='unknown')
    elif fault=='queued': queue=b'123|other|1000|bigTiger\n'
    elif fault=='comment-queued': queue=('456|'+key+'|1000|bigTiger\n').encode()
    else: queue=b'not a queue row\n'
    with pytest.raises(ValueError):
        allocation.validate_terminal_allocation(raw,queue,
            job_id='123',submission_key=key,partition='bigTiger',uid=1000)


def test_query_uses_finite_accounting_then_live_queue_and_no_mutations(monkeypatch):
    monkeypatch.delenv('SLURM_JOB_ID',raising=False)
    monkeypatch.setattr(allocation.os,'getuid',lambda:1000)
    key='spec183-'+'a'*64
    calls=[]
    def run(command,**kwargs):
        calls.append(command)
        assert kwargs['check'] and 0<kwargs['timeout']<=5
        assert kwargs['env']=={'PATH':'/usr/bin:/bin','LC_ALL':'C'}
        return SimpleNamespace(stdout=accounting(key) if len(calls)==1 else b'',stderr=b'')
    monkeypatch.setattr(allocation.subprocess,'run',run)
    result=allocation.capture_terminal_allocation(job_id='123',submission_key=key,
        partition='bigTiger',seconds=5)
    assert [c[0] for c in calls]==['/usr/bin/sacct','/usr/bin/squeue']
    assert '--jobs=123' in calls[0] and '--user=1000' in calls[1]
    assert result['receipt']['exitCode']=='0:0'


@pytest.mark.parametrize('outcome,state,code,expected', [
    ('PASS','COMPLETED','0:0','PASS'),('PASS','TIMEOUT','0:0','FAIL'),
    ('PASS','COMPLETED','1:0','FAIL'),('FAIL','COMPLETED','0:0','FAIL'),
    ('INCOMPLETE','COMPLETED','0:0','INCOMPLETE')])
def test_reconcile_closes_real_journal_only_from_bound_observation(allocated,monkeypatch,
                                                                 outcome,state,code,expected):
    module,args,prepared,profile,journal,root=allocated
    monkeypatch.delenv('SLURM_JOB_ID')
    args.reconcile=True
    profile['timing']['progressTimeoutSeconds']=5
    key=journal.get(args.run_id)['submissionKey']
    raw=accounting(key,uid=os.getuid(),state=state,code=code)
    receipt=allocation.validate_terminal_allocation(raw,b'',job_id='123',
        submission_key=key,partition='bigTiger',uid=os.getuid())
    calls=[]
    def capture(**kwargs):
        calls.append(kwargs)
        return dict(receipt=receipt,accounting=raw.decode(),queue='')
    monkeypatch.setattr(allocation,'capture_terminal_allocation',capture)
    module._write_readonly(root/'srun-cleanup.json',dict(runId=args.run_id,
        jobId='123',candidateDigest=prepared['candidateDigest'],cleanup=[dict(name='yolo-srun',
        kind='finite',reaped=True,forced=False,exitCode=0)]))
    assert module._maybe_reconcile(args,profile,prepared,outcome)==expected
    assert journal.get(args.run_id)['state']==expected
    assert module._verify_terminal_record(root,prepared)['status']==expected
    # Durable terminal observation survives a crash between receipt and journal
    # and permits an idempotent retry without another scheduler query.
    assert module._maybe_reconcile(args,profile,prepared,outcome)==expected
    assert len(calls)==1


def test_collect_scheduler_timeout_is_not_model_failure_or_journal_release(allocated,monkeypatch):
    module,args,prepared,profile,journal,root=allocated
    monkeypatch.delenv('SLURM_JOB_ID')
    args.reconcile=True
    profile['timing']['progressTimeoutSeconds']=5
    (root/'collection-input.json').write_text('{}')
    verdict=dict(status='PASS',runId=args.run_id,candidateDigest=prepared['candidateDigest'],
                 collectorSchema='tiger-yolo-collector-v1')
    monkeypatch.setattr(module,'_reanalyze_retained',lambda *a: verdict)
    def timeout(**kwargs): raise subprocess.TimeoutExpired('sacct',5)
    monkeypatch.setattr(allocation,'capture_terminal_allocation',timeout)
    with pytest.raises(module.ClosureError,match='TERMINAL_RECONCILIATION'):
        module._collect(args)
    assert journal.get(args.run_id)['state']=='SUBMITTED'
    assert not (root/'collection-failure.json').exists()
    assert not (root/'allocation-terminal.json').exists()
    assert json.loads((root/'verdict.json').read_text())==verdict


def test_reconcile_inside_job_is_rejected_before_scheduler_or_journal(allocated):
    module,args,prepared,profile,journal,root=allocated
    args.reconcile=True
    with pytest.raises(module.ClosureError,match='TERMINAL_OBSERVER_CONTEXT'):
        module._maybe_reconcile(args,profile,prepared,'PASS')
    assert journal.get(args.run_id)['state']=='SUBMITTED'


@pytest.mark.parametrize('fault',['none','missing','failed','wrong-job'])
def test_gpu_gate_requires_bound_successful_terminal_record(gate_pair,monkeypatch,fault):
    from runtime.yolo_submission import SubmissionJournal
    module,previous,current,verdict,profile,root=gate_pair
    previous['case']=previous['plan']['case']='single-node-gpu'
    previous['candidateDigest']=module._prepared_candidate(previous)
    current['case']=current['plan']['case']='two-node-gpu'
    current['candidateDigest']=module._prepared_candidate(current)
    prior_root=root/previous['runId']
    (prior_root/'prepare.json').write_text(json.dumps(previous))
    verdict.update(case=previous['case'],candidateDigest=previous['candidateDigest'])
    (prior_root/'verdict.json').write_text(json.dumps(verdict))
    profile['release']['gates']={'singleNodeGpu':file_ref(prior_root/'verdict.json')}
    monkeypatch.setattr(module,'_reanalyze_retained',lambda *a: verdict)
    locks=root/'locks'
    locks.mkdir()
    journal=SubmissionJournal(locks,candidate_id=previous['contentIdentities']['dispatch'],gate=previous['case'])
    key=journal.reserve(previous['runId'])['submissionKey']
    raw=accounting(key,uid=os.getuid())
    receipt=allocation.validate_terminal_allocation(raw,b'',job_id='123',submission_key=key,
        partition='bigTiger',uid=os.getuid())
    if fault!='missing':
        module._write_readonly(prior_root/'allocation-terminal.json',dict(runId=previous['runId'],
            candidateDigest=previous['candidateDigest'],status='FAIL' if fault=='failed' else 'PASS',
            observation=dict(receipt=receipt,accounting=raw.decode(),queue='')))
    module._write_readonly(prior_root/'srun-cleanup.json',dict(runId=previous['runId'],
        candidateDigest=previous['candidateDigest'],jobId='456' if fault=='wrong-job' else '123',
        cleanup=[dict(name='yolo-srun',kind='finite',reaped=True,forced=False,exitCode=0)]))
    if fault=='none':
        assert module._gate_receipt(root/'profile.json',profile,'singleNodeGpu',prepared=current)['receipt']==verdict
    else:
        with pytest.raises(module.ClosureError,match='GATE_RETAINED_EVIDENCE'):
            module._gate_receipt(root/'profile.json',profile,'singleNodeGpu',prepared=current)


def test_frozen_entry_preserves_explicit_reconciliation_option(allocated,monkeypatch):
    from runtime import yolo_bundle
    module,args,prepared,profile,journal,root=allocated
    # The allocated fixture replaces _enter_frozen; load the actual CLI owner.
    from test_yolo_submit import submit_module
    module=submit_module()
    args.reconcile=True
    prepared['harnessManifestSha256']='sha256:'+'d'*64
    args.profile.write_text('{"runtime":{}}')
    prepared['profileDigest']=module._json_digest({'runtime':{}})
    monkeypatch.setattr(yolo_bundle,'verify_harness',lambda *a,**k: None)
    commands=[]
    monkeypatch.setattr(subprocess,'run',lambda command,**k: commands.append(command) or SimpleNamespace(returncode=78))
    assert module._enter_frozen(args,prepared,'collect')==78
    assert commands[0][-1]=='--reconcile'
