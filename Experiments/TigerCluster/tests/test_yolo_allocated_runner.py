"""Real journal/CLI boundary with explicit srun and collector doubles."""
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from test_yolo_submit import submit_module
from runtime.yolo_submission import SubmissionJournal


@pytest.fixture
def allocated(tmp_path, monkeypatch):
    module = submit_module()
    output = tmp_path / 'runs'
    root = output / 'gpu-run'
    (root / 'bundle').mkdir(parents=True)
    locks = tmp_path / 'locks'
    locks.mkdir()
    digest = 'sha256:'+'a'*64
    prepared = dict(case='single-node-gpu', runId='gpu-run', profileDigest=digest,
        candidateDigest='sha256:'+'b'*64, contentIdentities=dict.fromkeys(
            ('inputs', 'runtime', 'dispatch'), digest), bundle=str(root/'bundle'),
        plan={'output': str(root)})
    profile = dict(storage=dict(sharedRunRoot=str(output), sharedLockRoot=str(locks)),
        cluster=dict(partition='bigTiger', gpuClass='rtx_6000', wallTimeSeconds=600, cpusPerNode=4),
        timing=dict(cleanupSeconds=30))
    report = dict(integrity='VERIFIED', documentDigest=digest,
                  identities=prepared['contentIdentities'])
    journal = SubmissionJournal(locks, candidate_id=digest, gate='single-node-gpu')
    journal.reserve('gpu-run')
    journal.mark_submitting('gpu-run')
    journal.record_submission('gpu-run', '123')
    monkeypatch.setenv('SLURM_JOB_ID', '123')
    monkeypatch.setattr(module, '_dispatch_report', lambda _: (report, profile))
    monkeypatch.setattr(module, '_load_prepared', lambda *_: prepared)
    monkeypatch.setattr(module, '_enter_frozen', lambda *_: None)
    args = SimpleNamespace(profile=tmp_path/'profile.json', run_id='gpu-run',
                           output=output, case='single-node-gpu')
    return module, args, prepared, profile, journal, root


@pytest.mark.parametrize('fault', ['none', 'forced', 'unreaped', 'nonzero'])
def test_srun_exit_only_reaches_collector_after_clean_reaping(allocated, monkeypatch, fault):
    from runtime import worker
    module, args, prepared, profile, journal, root = allocated
    events = []
    monkeypatch.setattr(module, '_gate_receipt', lambda *a, **k: events.append('gate'))
    def finite(name, command, log, cleanup, **kwargs):
        assert events == ['gate']
        assert journal.get('gpu-run')['state'] == 'RUNNING'
        assert command[:4] == ['/usr/bin/srun', '--exact', '--nodes=1', '--ntasks=1']
        assert '--kill-on-bad-exit=1' in command and '--gpus-per-task=1' in command
        assert command[command.index('-B')+2] == 'rank'
        assert kwargs['seconds'] == 570
        cleanup.append(dict(name=name, kind='finite', reaped=fault != 'unreaped',
                            forced=fault == 'forced', exitCode=1 if fault == 'nonzero' else 0))
        events.append('srun')
    monkeypatch.setattr(worker, 'run_finite_application', finite)
    def collect(unused):
        assert events == ['gate', 'srun']
        module._verify_srun_cleanup(root, prepared)
        events.append('collect')
        return 78
    monkeypatch.setattr(module, '_collect', collect)
    if fault == 'none':
        assert module._run(args) == 78
        assert events[-1] == 'collect'
    else:
        with pytest.raises(module.ClosureError, match='SRUN_CLEANUP_FAILED'):
            module._run(args)
        assert 'collect' not in events
    assert json.loads((root/'srun-cleanup.json').read_text())['jobId'] == '123'
    # Only an external observer of terminal Slurm state may close the journal.
    assert journal.get('gpu-run')['state'] == 'RUNNING'


@pytest.mark.parametrize('fault', ['job', 'state', 'shared-root', 'case'])
def test_unbound_batch_never_launches(allocated, monkeypatch, fault):
    module, args, _, profile, journal, _ = allocated
    if fault == 'job':
        monkeypatch.setenv('SLURM_JOB_ID', '456')
    elif fault == 'state':
        journal.mark_running('gpu-run', '123')
    elif fault == 'shared-root':
        profile['storage']['sharedRunRoot'] += '-other'
    else:
        args.case = 'two-node-gpu'
    with pytest.raises(module.ClosureError):
        module._run(args)


def test_single_gpu_owner_checks_allocation_before_native_or_issuer(allocated, monkeypatch):
    from runtime import yolo_operator, yolo_allocation
    _, _, prepared, profile, _, _ = allocated
    profile['timing']['progressTimeoutSeconds'] = 10
    def rejected(**kwargs):
        raise ValueError('SLURM_TASK_BINDING')
    def forbidden(**kwargs):
        pytest.fail('wrong allocation reached issuer/native owner')
    monkeypatch.setattr(yolo_allocation, 'capture_task_allocation', rejected)
    monkeypatch.setattr(yolo_operator, '_execute_single_node', forbidden)
    with pytest.raises(ValueError, match='SLURM_TASK_BINDING'):
        yolo_operator.execute_single_gpu_run(prepared=prepared, profile=profile,
            resolved={}, allocation_expected={})


def test_rank_entry_passes_journal_identity_to_real_owner_boundary(allocated, monkeypatch):
    from runtime import yolo_operator, yolo_profile
    module, args, prepared, profile, journal, _ = allocated
    journal.mark_running('gpu-run', '123')
    resolved = {'declared': 'issuer mapping fixture'}
    monkeypatch.setattr(yolo_profile, 'resolve_provision_inputs', lambda *a, **k: resolved)
    calls = []
    monkeypatch.setattr(yolo_operator, 'execute_single_gpu_run', lambda **k: calls.append(k))
    assert module._rank(args) == 0
    assert len(calls) == 1
    assert calls[0]['prepared'] == prepared and calls[0]['resolved'] is resolved
    assert calls[0]['allocation_expected'] == dict(job_id='123',
        submission_key=journal.get('gpu-run')['submissionKey'], partition='bigTiger', gpu_type='rtx_6000')


def test_reanalysis_rejects_cleanup_from_a_different_job(allocated, monkeypatch):
    module, _, prepared, _, _, root = allocated
    (root/'srun-cleanup.json').write_text(json.dumps(dict(runId=prepared['runId'],
        jobId='123', candidateDigest=prepared['candidateDigest'], cleanup=[dict(
            name='yolo-srun', kind='finite', reaped=True, forced=False, exitCode=0)])))
    monkeypatch.setattr(module, '_collection_file', lambda _: root/'collection-input.json')
    monkeypatch.setattr(module, '_load_collection_input', lambda *a, **k: (
        dict(kind='normal', allocationExpected={'job_id': '456'}), 'sha256:'+'c'*64))
    with pytest.raises(module.ClosureError, match='SRUN_COLLECTION_JOB_BINDING'):
        module._reanalyze_retained(root, prepared)


def _two_node(allocated, mode='two-node-gpu'):
    module, args, prepared, profile, _, root = allocated
    args.case = prepared['case'] = mode
    journal = SubmissionJournal(Path(profile['storage']['sharedLockRoot']),
        candidate_id=prepared['contentIdentities']['dispatch'], gate=mode)
    journal.reserve(args.run_id)
    journal.mark_submitting(args.run_id)
    journal.record_submission(args.run_id, '123')
    return module, args, prepared, profile, journal, root


@pytest.mark.parametrize('mode', ['two-node-gpu', 'negative-dependency'])
def test_two_node_batch_joins_only_after_both_task_exit(allocated, monkeypatch, mode):
    from runtime import worker, yolo_profile, yolo_operator
    module, args, prepared, profile, journal, root = _two_node(allocated, mode)
    gate = 'twoNodeGpu' if mode == 'negative-dependency' else 'singleNodeGpu'
    events = []
    monkeypatch.setattr(module, '_gate_receipt', lambda p, v, gate, **k: events.append(gate))
    def finite(name, command, log, cleanup, **kw):
        assert command[:4] == ['/usr/bin/srun', '--exact', '--nodes=2', '--ntasks=2']
        control = json.loads((root/'distributed-control.json').read_text())
        assert control['jobId'] == '123' and len(control['probeId']) == 32
        assert journal.get(args.run_id)['state'] == 'RUNNING'
        cleanup.append(dict(name=name, kind='finite', reaped=True, forced=False, exitCode=0))
        events.append('srun')
    monkeypatch.setattr(worker, 'run_finite_application', finite)
    monkeypatch.setattr(yolo_profile, 'resolve_provision_inputs', lambda *a, **k: {})
    def join(**kw):
        assert events == [gate, 'srun']
        module._verify_srun_cleanup(root, prepared)
        events.append('join')
    monkeypatch.setattr(yolo_operator, 'finalize_distributed_run', join)
    monkeypatch.setattr(module, '_collect', lambda a: events.append('collect') or 78)
    assert module._run(args) == 78
    assert events == [gate,'srun','join','collect']


@pytest.mark.parametrize('mode', ['two-node-gpu', 'negative-dependency'])
def test_two_node_task_uses_attested_rank_entry(allocated, monkeypatch, mode):
    from runtime import yolo_profile, yolo_operator
    module, args, prepared, _, journal, _ = _two_node(allocated, mode)
    journal.mark_running(args.run_id, '123')
    monkeypatch.setenv('SLURM_PROCID', '1')
    monkeypatch.setattr(yolo_profile, 'resolve_provision_inputs', lambda *a, **k: {})
    calls=[]
    monkeypatch.setattr(yolo_operator, 'execute_distributed_rank', lambda **k: calls.append(k))
    assert module._rank(args)==0
    assert len(calls)==1 and calls[0]['rank']==1 and calls[0]['prepared']==prepared
