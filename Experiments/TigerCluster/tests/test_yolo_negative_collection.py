"""Retained-file rejection joins; native compute/allocation are explicit doubles."""
import hashlib
import json
from pathlib import Path

import pytest

from test_yolo_negative_user import fixture, D
from runtime import yolo_negative as negative


def sha(payload):
    return 'sha256:' + hashlib.sha256(payload).hexdigest()


@pytest.mark.parametrize('fault', ['none', 'candidate', 'deadline', 'fake-pass', 'late-success', 'no-shutdown'])
def test_negative_user_reader_binds_retained_observation(tmp_path, monkeypatch, fault):
    observer, binding, _, _, _ = fixture(tmp_path, monkeypatch,
        [TimeoutError('wait'), RuntimeError('shutdown')])
    observer.terminal(**binding)
    value = observer.finish_after_shutdown(0)
    if fault == 'candidate': value['candidateDigest'] = D
    if fault == 'deadline': value['elapsedMs'] = 10001
    if fault == 'fake-pass': value['qualification'] = 'EXPECTED_REJECTION_PASS'
    if fault == 'no-shutdown': value['observedAfterShutdown'] = False
    if fault == 'late-success':
        value['response'] = dict(present=True, success=True, bytes=0, sha256=sha(b''))
    path = tmp_path / 'negative-user.json'
    path.chmod(0o600)
    path.write_text(json.dumps(value))
    def read():
        return negative.read_negative_user(tmp_path, run_id=observer.run_id,
            request_id=observer.request_id, candidate_digest=observer.candidate_digest,
            placement_id=observer.placement_id, placement_digest=observer.placement_digest, deadline_ms=10000)
    if fault in ('none', 'late-success'):
        result = read()
        assert result['qualification'] == 'NEGATIVE_USER_COMPONENT_ONLY'
        assert result['observation']['response']['present'] == (fault == 'late-success')
    else:
        with pytest.raises(ValueError): read()


def cutpoint_fixture(tmp_path):
    names = ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')
    providers = {r: '/provider/' + r for r in names}
    logs = {r: tmp_path / (r + '.log') for r in names}
    for p in logs.values(): p.write_text('')
    edge = dict(scope='head0-merge', producer='DetectShard0', consumer='Merge', planned_name='/tensor/head0')
    contract = dict(edges=[edge], sessionId='run/request/1/attempt/1')
    row = dict(schema='ndnsf-di-withheld-output-v1', session=contract['sessionId'],
        requestId='/run/request/1', attempt='1', planDigest=D, producerRole='DetectShard0', consumerRole='Merge',
        plannedDataName=edge['planned_name'], manifestDataName=edge['planned_name'] + '/MANIFEST',
        endpointDigest=D, contentDigest=D, bytes='64', provider=providers['DetectShard0'],
        providerBootId='boot-fixture', round='3', microbatch='0', operationKind='PIPELINE',
        tensor='tensor/head0', atMs='100')
    logs['DetectShard0'].write_text('NDNSF_DI_OUTPUT_WITHHELD ' + json.dumps(row) + '\n')
    logs['Merge'].write_text('NDNSF_DI_NATIVE_FAILURE session=' + contract['sessionId'] +
        ' role=Merge reason=failed to fetch signed exact Data: ' + row['manifestDataName'] + '\n')
    return logs, contract, row, providers


@pytest.mark.parametrize('fault', ['none', 'timeout-only', 'missing-cutpoint', 'duplicate',
    'wrong-request', 'wrong-attempt', 'wrong-plan', 'wrong-provider', 'wrong-owner',
    'wrong-manifest', 'wrong-consumer', 'number-bool', 'unrelated-failure', 'transferred'])
def test_cutpoint_requires_exact_consumer_failure_and_bound_producer(tmp_path, fault):
    logs, contract, row, providers = cutpoint_fixture(tmp_path)
    if fault == 'wrong-request': row['requestId'] = '/other'
    if fault == 'wrong-attempt': row['attempt'] = '2'
    if fault == 'wrong-plan': row['planDigest'] = 'sha256:' + '3' * 64
    if fault == 'wrong-provider': row['provider'] = providers['Merge']
    if fault == 'wrong-manifest': row['manifestDataName'] = '/other/MANIFEST'
    if fault == 'wrong-consumer': row['consumerRole'] = 'DetectShard1'
    if fault == 'number-bool': row['bytes'] = True
    text = 'NDNSF_DI_OUTPUT_WITHHELD ' + json.dumps(row) + '\n'
    logs['DetectShard0'].write_text(text * (2 if fault == 'duplicate' else 1))
    if fault == 'missing-cutpoint': logs['DetectShard0'].write_text('')
    if fault == 'timeout-only': logs['Merge'].write_text('TimeoutError\n')
    if fault == 'wrong-owner':
        logs['DetectShard1'].write_text(text)
        logs['DetectShard0'].write_text('')
    if fault == 'unrelated-failure':
        logs['BackboneNeck'].write_text('NDNSF_DI_NATIVE_FAILURE session=other role=BackboneNeck reason=bad\n')
    if fault == 'transferred':
        logs['DetectShard0'].write_text(text + 'NDNSF_DI_DEPENDENCY_OBJECT session=' + contract['sessionId'] +
            ' producer=DetectShard0 consumer=Merge status=ok\n')
    def read():
        return negative.read_negative_cutpoint(logs, contract=contract,
            request_id='/run/request/1', plan_digest=D, providers_by_role=providers)
    if fault == 'none':
        assert read()['qualification'] == 'NEGATIVE_CUTPOINT_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError): read()


def test_cutpoint_accepts_native_nni_name_encoding(tmp_path):
    logs, contract, row, providers = cutpoint_fixture(tmp_path)
    native = '/tensor/ATTEMPT/%01/ROUND/%03/RANK/%00/MICROBATCH/%00'
    edge = contract['edges'][0]
    edge['planned_name'] = '/tensor/ATTEMPT/1/ROUND/3/RANK/0/MICROBATCH/0'
    row['plannedDataName'] = native
    row['manifestDataName'] = native + '/MANIFEST'
    logs['DetectShard0'].write_text('NDNSF_DI_OUTPUT_WITHHELD ' + json.dumps(row) + '\n')
    logs['Merge'].write_text('NDNSF_DI_NATIVE_FAILURE session=' + contract['sessionId'] +
        ' role=Merge reason=failed to fetch signed exact Data: ' + row['manifestDataName'] + '\n')
    result = negative.read_negative_cutpoint(logs, contract=contract,
        request_id='/run/request/1', plan_digest=D, providers_by_role=providers)
    assert result['qualification'] == 'NEGATIVE_CUTPOINT_COMPONENT_ONLY'


def test_cutpoint_accepts_native_request_session_without_attempt_suffix(tmp_path):
    logs, contract, row, providers = cutpoint_fixture(tmp_path)
    row['session'] = '/run/request/1'
    logs['DetectShard0'].write_text('NDNSF_DI_OUTPUT_WITHHELD ' + json.dumps(row) + '\n')
    logs['Merge'].write_text('NDNSF_DI_NATIVE_FAILURE session=' + row['session'] +
        ' role=Merge reason=failed to fetch signed exact Data: ' + row['manifestDataName'] + '\n')
    result = negative.read_negative_cutpoint(logs, contract=contract,
        request_id='/run/request/1', plan_digest=D, providers_by_role=providers)
    assert result['qualification'] == 'NEGATIVE_CUTPOINT_COMPONENT_ONLY'


def test_cutpoint_accepts_native_dependency_wait_reason_wrapper(tmp_path):
    logs, contract, row, providers = cutpoint_fixture(tmp_path)
    logs['Merge'].write_text('NDNSF_DI_NATIVE_FAILURE session=' + contract['sessionId'] +
        ' role=Merge reason=dependency wait failed: failed to fetch signed exact Data: ' +
        row['manifestDataName'] + '\n')
    result = negative.read_negative_cutpoint(logs, contract=contract,
        request_id='/run/request/1', plan_digest=D, providers_by_role=providers)
    assert result['qualification'] == 'NEGATIVE_CUTPOINT_COMPONENT_ONLY'


def test_cutpoint_binds_one_edge_when_role_pair_has_multiple_outputs(tmp_path):
    logs, contract, row, providers = cutpoint_fixture(tmp_path)
    second = dict(contract['edges'][0], scope='head0-merge-scale2',
                  planned_name='/tensor/head0-scale2')
    contract['edges'].append(second)
    logs['DetectShard0'].write_text(
        'NDNSF_DI_OUTPUT_WITHHELD ' + json.dumps(row) + '\n'
        + 'NDNSF_DI_DEPENDENCY_OBJECT session=' + contract['sessionId']
        + ' producer=DetectShard0 consumer=Merge planned_name=/tensor/head0-scale2 status=ok\n')
    result = negative.read_negative_cutpoint(logs, contract=contract,
        request_id='/run/request/1', plan_digest=D, providers_by_role=providers)
    assert result['edge']['planned_name'] == '/tensor/head0'
    assert result['cutpoint']['round'] == '3'


@pytest.mark.parametrize('record', ['user', 'cutpoint'])
def test_deeply_nested_record_is_a_retained_collection_rejection(tmp_path, record):
    payload = '[' * 2000 + '0' + ']' * 2000
    if record == 'user':
        path = tmp_path / 'negative-user.json'
        path.write_text(payload)
        with pytest.raises(ValueError, match='NEGATIVE_RECORD_JSON'):
            negative._read_json(path)
    else:
        logs, contract, _, providers = cutpoint_fixture(tmp_path)
        logs['DetectShard0'].write_text('NDNSF_DI_OUTPUT_WITHHELD ' + payload + '\n')
        with pytest.raises(ValueError, match='NEGATIVE_CUTPOINT_JSON'):
            negative.read_negative_cutpoint(logs, contract=contract,
                request_id='/run/request/1', plan_digest=D, providers_by_role=providers)


@pytest.mark.parametrize('rank', [0, 1])
def test_negative_node_receipt_requires_exact_single_request_schedule(tmp_path, rank):
    from test_yolo_node_receipt import worker
    from runtime import yolo_result as result
    from runtime.yolo_worker import assigned_roles
    state, rows = worker(tmp_path)
    state.mode = 'negative-dependency'
    state.rank = rank
    state.roles = assigned_roles(state.mode, rank)
    plan, prep, candidate = state._preparation_binding
    plan['case'] = state.mode
    plan['requests'] = [dict(index=0, requestId='/request/0', warmup=False)]
    if rank == 1:
        next(v for v in state.launches if v['role'] == 'nfd0')['role'] = 'nfd1'
        next(v for v in rows if v['name'] == 'nfd0')['name'] = 'nfd1'
        (state.output / 'logs/nfd0.log').rename(state.output / 'logs/nfd1.log')
    state.launches = [v for v in state.launches if v['role'] in state.roles and
                      (v['role'] != 'user' or v.get('invocation') == '0')]
    names = {v['role'] + ('-' + v['invocation'] if 'invocation' in v else '') for v in state.launches}
    rows = [v for v in rows if v['name'] in names]
    if rank == 1:
        state.output.rename(tmp_path / 'node1')
        state.output = tmp_path / 'node1'
    result.write_worker_receipt(state, rows)
    read = result.read_node_log_receipt(state.output, receipt_digest=sha((state.output / 'node-receipt.json').read_bytes()),
        plan=plan, preparation_digest=prep, candidate_digest=candidate, rank=rank)
    assert ('user-0' in read['logs']) == (rank == 0)


def test_legacy_operator_authored_rejection_is_not_public_evidence(tmp_path):
    from test_yolo_submit import submit_module, _negative_collection_input
    module = submit_module()
    prepared = dict(runId='negative-run', case='negative-dependency', candidateDigest=D,
                    plan=dict(requests=[dict(requestId='/run/request/1')]))
    path = tmp_path / 'collection-input.json'
    path.write_text(json.dumps(_negative_collection_input(prepared)))
    with pytest.raises(ValueError, match='COLLECTION_INPUT_SCHEMA'):
        module._load_collection_input(path, root=tmp_path, prepared=prepared)


@pytest.mark.parametrize('fault', ['none', 'node-missing', 'cleanup', 'gpu-alias', 'placement',
    'selection', 'response', 'failed-response', 'cutpoint', 'compute-model', 'log-changed'])
def test_retained_join_requires_all_independent_boundaries(tmp_path, monkeypatch, fault):
    from runtime import yolo_bundle, yolo_result as result
    root = tmp_path / 'run'
    output = root / 'node0/user/requests/0'
    output.mkdir(parents=True)
    observer, binding, _, _, rows = fixture(output, monkeypatch,
        [TimeoutError('wait'), RuntimeError('stopped')])
    log_root = tmp_path / 'logs'
    log_root.mkdir()
    logs, contract, _, providers = cutpoint_fixture(log_root)
    def canonical(value):
        return sha(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode())
    rows[8]['roleDigest'] = canonical(providers)
    rows[7]['selectionDigest'] = canonical({'plan': D, 'ack': rows[2]['ackSnapshotDigest']})
    (output / 'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows) + '\n')
    observer.terminal(**binding)
    user = observer.finish_after_shutdown(0)
    plan = dict(case='negative-dependency', runId=observer.run_id, output=str(root), identities=providers,
        requests=[dict(index=0, warmup=False, requestId=observer.request_id, output=str(output))])
    assignments = []
    for role in providers:
        assignments.append(dict(schema='tiger-yolo-public-assignment-v2', requestId=observer.request_id,
            attempt=1, planDigest=D, sessionId=contract['sessionId'], provider=providers[role], role=role,
            inputs=[e for e in contract['edges'] if e['consumer'] == role],
            outputs=[e for e in contract['edges'] if e['producer'] == role],
            model=dict(modelManifestDigest=D, artifactDigest=D)))
    (output / 'yolo-public-assignments.json').write_text(json.dumps(
        dict(schema='yolo-public-assignments-v2', assignments=assignments)))
    nodes = {rank: dict(root=root / ('node' + str(rank)), receiptDigest=D, preparationDigest=D,
                       allocationDigest=D, gpuProbeDigest=D) for rank in (0, 1)}
    seen = []
    def closed(path, **kw):
        seen.append(('node', kw['rank']))
        if fault == 'cleanup': raise ValueError('CLEANUP_NOT_CLEAN')
        roles = ('BackboneNeck', 'Merge') if kw['rank'] == 0 else ('DetectShard0', 'DetectShard1')
        return dict(logs={role: dict(path=str(logs[role]), logDigest=sha(logs[role].read_bytes())) for role in roles})
    monkeypatch.setattr(result, 'read_node_log_receipt', closed)
    def device(path, **kw):
        rank = kw['rank']
        seen.append(('device', rank))
        return dict(allocation=dict(jobId='1', stepId='0', submissionKey='key', hosts=['n0', 'n1'],
            hostname='n' + str(rank)), gpuBinding=dict(uuid='GPU-' + str(0 if fault == 'gpu-alias' else rank), visible='0'), uid=1000)
    monkeypatch.setattr(result, 'read_retained_device_binding', device)
    preparation = dict(placementCandidateId=observer.placement_id, placementCandidateDigest=D,
                       graphDigest=D, catalogueDigest=D)
    def verify(public, received_plan, **kw):
        assert public == root / 'public' and received_plan == plan
        assert kw == dict(expected_receipt_digest=D, candidate_digest=observer.candidate_digest)
        seen.append(('preparation', 0))
        return preparation
    monkeypatch.setattr(yolo_bundle, 'verify_preparation', verify)
    def compute(path, **kw):
        role = kw['role']
        seen.append(('compute', role))
        return dict(native=dict(observation=dict(modelDigest='bad' if fault == 'compute-model' else D,
            artifactDigests={role: D}), logDigest='bad' if fault == 'log-changed' else sha(logs[role].read_bytes())))
    monkeypatch.setattr(result, 'collect_retained_role_execution', compute)
    if fault == 'node-missing': del nodes[1]
    if fault == 'placement': preparation['placementCandidateDigest'] = 'sha256:' + '4' * 64
    if fault == 'selection':
        rows[7]['selectionDigest'] = 'sha256:' + '4' * 64
        (output / 'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows) + '\n')
    if fault in ('response', 'failed-response'):
        user['response'] = dict(present=True, success=fault == 'response', bytes=0, sha256=sha(b''))
        path = output / 'negative-user.json'
        path.chmod(0o600)
        path.write_text(json.dumps(user))
    if fault == 'cutpoint': logs['DetectShard0'].write_text('')
    def collect():
        return negative.collect_negative_verdict(nodes, plan=plan, runtime_candidate_digest=observer.candidate_digest,
            placement_candidate_id=observer.placement_id, placement_candidate_digest=D,
            graph_digest=D, catalogue_digest=D, providers_by_role=providers,
            allocation_expected={}, request_deadline_ms=10000)
    if fault in ('none', 'failed-response'):
        verdict = collect()
        assert verdict['qualification'] == 'EXPECTED_REJECTION_PASS'
        assert {('node', 0), ('node', 1), ('device', 0), ('device', 1), ('preparation', 0),
                ('compute', 'BackboneNeck'), ('compute', 'DetectShard0')} <= set(seen)
    else:
        with pytest.raises(ValueError): collect()
