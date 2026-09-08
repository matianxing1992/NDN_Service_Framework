"""Two concurrent rank owners; explicit native/allocation boundary doubles."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from test_yolo_local_execution import frozen_bundle, scratch_double
from test_yolo_collection import plan_fixture, kwargs as collection_kwargs
from runtime import yolo_operator as operator


@pytest.mark.parametrize('fault', ['none', 'prepare', 'request', 'missing-request', 'budget'])
@pytest.mark.parametrize('mode', ['two-node-gpu', 'negative-dependency'])
def test_two_ranks_share_one_preparation_and_probe_without_partial_success(tmp_path, monkeypatch, frozen_bundle, scratch_double, fault, mode):
    from runtime import yolo_allocation, yolo_bundle, yolo_result, yolo_graph_reference
    from runtime.yolo_worker import assigned_roles
    bundle, harness = frozen_bundle
    root = tmp_path/'run'
    root.mkdir()
    candidate = 'sha256:'+'a'*64
    roles = {r: '/two-test/'+r for n in (0,1) for r in assigned_roles('two-node-gpu', n)}
    plan = dict(schema='tiger-yolo-run-plan-v1', runId='two-test', case=mode,
        applicationName='/two-test', output=str(root), identities=roles,
        nodes=[dict(rank=n, roles=list(assigned_roles('two-node-gpu', n))) for n in (0,1)],
        requests=[dict(index=n, warmup=(n == 0 and mode != 'negative-dependency'),
                       requestId='/two-test/'+str(n), output=str(root/'node0/user/requests'/str(n)))
                  for n in range(1 if mode == 'negative-dependency' else 4)])
    prepared = dict(plan=plan, runId=plan['runId'], case=plan['case'], bundle=str(bundle),
                    harnessManifestSha256=harness, candidateDigest=candidate)
    expected = dict(job_id='123', submission_key='spec183-'+'b'*64, partition='bigTiger', gpu_type='rtx_6000')
    control = dict(runId=plan['runId'], candidateDigest=candidate, jobId='123', probeId='c'*32)
    (root/'distributed-control.json').write_text(json.dumps(control))
    profile = dict(cluster=dict(wallTimeSeconds=1 if fault == 'budget' else 30, tcpPort=18363),
        timing=dict(progressTimeoutSeconds=1, requestDeadlineMs=1000, stagingSeconds=2,
                    startupSeconds=2, cleanupSeconds=1), storage=dict(peakBytes=100, marginBytes=10),
        security=dict(protectionEpoch='epoch-1'))
    resolved = dict(descriptor=dict(plan=plan, runtimeCandidateDigest=candidate),
                    runtimeProfile={'sif': '/fixture.sif'}, package=str(tmp_path))
    receipt = dict(graphDigest='sha256:'+'d'*64, catalogueDigest='sha256:'+'e'*64,
        placementCandidateId='yolo', placementCandidateDigest='sha256:'+'f'*64,
        catalogueDataName='/two-test/catalogue', catalogueSigner='/two-test/controller')
    provisioned, launched, accepted = [], [], []
    def capture(**kw):
        assert kw['node_count'] == 2 and kw['rank'] in (0,1)
        return {'receipt': {'hosts': ['n0','n1'], 'visible': '0'}}
    monkeypatch.setattr(yolo_allocation, 'capture_task_allocation', capture)
    monkeypatch.setattr(operator, '_allocation_endpoints', lambda *a: [
        dict(rank=n, address='10.0.0.'+str(n+1), port=18363) for n in (0,1)])
    def stage(value, target):
        target.mkdir()
        return 'sha256:'+'1'*64
    monkeypatch.setattr(operator, 'stage_provision_inputs', stage)
    def provision(**kw):
        provisioned.append(kw)
        if fault == 'prepare':
            raise ValueError('ISSUER_FAILED')
        return dict(receiptDigest='sha256:'+'2'*64, preparation=receipt)
    monkeypatch.setattr(operator, 'provision_run', provision)
    monkeypatch.setattr(yolo_bundle, 'verify_preparation', lambda *a, **k: receipt)
    def reference(*args):
        assert mode != 'negative-dependency'
        return (object(), tmp_path, tmp_path)
    monkeypatch.setattr(operator, '_normal_reference', reference)
    monkeypatch.setattr(yolo_graph_reference, 'read_request_reference', lambda *a, **k: {})
    def numerical(output, ref, **kw):
        if fault == 'request':
            raise ValueError('NUMERICAL_FAIL')
        accepted.append(kw['request_id'])
    monkeypatch.setattr(yolo_result, 'collect_request_result', numerical)
    from runtime import yolo_negative
    monkeypatch.setattr(yolo_negative, 'read_negative_user', lambda output, **kw: numerical(output, None, **kw))
    def rank(**kw):
        launched.append(kw)
        assert kw['probe_id'] == 'c'*32 and kw['mode'] == mode
        assert set(kw['homes']) == set(assigned_roles('two-node-gpu', kw['rank']))
        if kw['rank'] == 0 and fault != 'missing-request':
            for request in plan['requests']:
                kw['accept_request'](request, Path(request['output']))
        return {'rank': kw['rank']}
    monkeypatch.setattr(operator, 'run_rank', rank)
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(operator.execute_distributed_rank, prepared=prepared,
            profile=profile, resolved=resolved, allocation_expected=expected, rank=n) for n in (1,0)]
        errors = []
        for future in futures:
            try:
                future.result(timeout=5)
            except ValueError as exc:
                errors.append(str(exc))
    if fault == 'none':
        assert errors == [] and len(provisioned) == 1 and len(launched) == 2
        assert len(accepted) == len(plan['requests'])
        assert launched[0]['preparation_digest'] == launched[1]['preparation_digest']
    else:
        assert errors and not (root/'collection-input.json').exists()
        if fault in ('prepare', 'budget'):
            assert launched == []


@pytest.mark.parametrize('mode', ['two-node-gpu', 'negative-dependency'])
def test_final_join_consumes_both_actual_receipt_files_without_plan_candidate(tmp_path, monkeypatch, mode):
    from runtime import yolo_bundle
    plan, root, nodes = plan_fixture(tmp_path, case=mode)
    args = collection_kwargs(tmp_path, plan, root, nodes)
    plan['requests'] = [dict(index=n) for n in range(1 if mode == 'negative-dependency' else 4)]
    plan['identities'] = args['providers_by_role']
    prepared = dict(plan=plan, runId=plan['runId'], candidateDigest=args['runtime_candidate_digest'])
    expected = dict(job_id='123', submission_key='spec183-'+'b'*64, partition='bigTiger', gpu_type='rtx_6000')
    control = dict(runId=plan['runId'], candidateDigest=prepared['candidateDigest'], jobId='123', probeId='c'*32)
    (root/'distributed-control.json').write_text(json.dumps(control))
    receipt = dict(graphDigest=args['graph_digest'], catalogueDigest=args['catalogue_digest'],
        placementCandidateId=args['placement_candidate_id'], placementCandidateDigest=args['placement_candidate_digest'])
    (root/'distributed-preparation.json').write_text(json.dumps(dict(control=control,
        provision=dict(receiptDigest='sha256:'+'a'*64, preparation=receipt))))
    monkeypatch.setattr(yolo_bundle, 'verify_preparation', lambda *a, **k: receipt)
    monkeypatch.setattr(operator, '_normal_reference', lambda *a: (None,
        Path(args['references'][0]['package']), Path(args['references'][0]['repository'])))
    result = operator.finalize_distributed_run(prepared=prepared, profile={}, resolved={}, allocation_expected=expected)
    assert set(result['nodes']) == {'0','1'}
    assert len(result['references']) == (0 if mode == 'negative-dependency' else 4)
    assert result['kind'] == ('expected-rejection' if mode == 'negative-dependency' else 'normal')
    assert result['candidateDigest'] == prepared['candidateDigest']
    assert 'candidateDigest' not in plan


@pytest.mark.parametrize('fault', ['none','duplicate','loopback','ambiguous'])
def test_endpoints_use_only_bounded_scheduler_host_resolution(monkeypatch, fault):
    calls=[]
    def query(command, **kw):
        calls.append(command)
        assert 0 < kw['timeout'] <= 2
        host=command[-1]
        address='10.0.0.1' if host=='n0' or fault=='duplicate' else '10.0.0.2'
        if fault=='loopback': address='127.0.0.1'
        payload=address+' STREAM '+host+'\n'
        if fault=='ambiguous': payload+='10.0.0.3 STREAM '+host+'\n'
        return SimpleNamespace(stdout=payload.encode())
    monkeypatch.setattr(operator.subprocess, 'run', query)
    if fault=='none':
        assert len(operator._allocation_endpoints(['n0','n1'], 18363, 2))==2
        assert calls == [['/usr/bin/getent','ahostsv4','n0'], ['/usr/bin/getent','ahostsv4','n1']]
    else:
        with pytest.raises(ValueError): operator._allocation_endpoints(['n0','n1'],18363,2)
