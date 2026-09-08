"""Real receipt/log/profile readers; ownership and compute are fixtures."""
import hashlib
import json
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result
from test_yolo_node_receipt import worker
from test_yolo_native_observation import observation

UUID = 'GPU-12345678-1234-1234-1234-123456789abc'


@pytest.mark.parametrize('fault', ['none', 'missing-binding', 'wrong-uuid', 'wrong-selector', 'wrong-ordinal'])
def test_retained_gpu_role_compares_external_device_expectation(tmp_path, fault):
    """Real receipt/profile readers, source-shaped GPU log; no GPU execution."""
    state, rows = worker(tmp_path)
    state.mode = state._preparation_binding[0]['case'] = 'single-node-gpu'
    launch = next(r for r in state.launches if r['role'] == 'BackboneNeck')
    row = observation()
    row.update(processId=str(launch['pid']), profileRequestId='/app/request/1',
        providerProfilePath='/output/ort/profile.json', device={'kind': 'cuda', 'id': '0'},
        gpuUuid=UUID, gpuUuids=[UUID], cudaVisibleDevices='3',
        gpuIdentitySource='cuda-runtime-pci+driver-uuid')
    if fault == 'wrong-ordinal': row['device']['id'] = '3'
    row['nodeProviderAssignments'][0]['nodeName'] = 'conv_kernel_time'
    log = state.output/'logs/BackboneNeck.log'
    log.write_text(log.read_text()+'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED '+json.dumps(row)+'\n')
    profile = state.output/'BackboneNeck/ort/profile.json'
    profile.parent.mkdir(parents=True)
    profile.write_text(json.dumps([dict(cat='Node', name='conv_kernel_time',
        args={'provider': 'CUDAExecutionProvider'})]))
    result.write_worker_receipt(state, rows)
    digest = 'sha256:'+hashlib.sha256((state.output/'node-receipt.json').read_bytes()).hexdigest()
    plan, prep, candidate = state._preparation_binding
    binding = dict(uuid=UUID, visible='3')
    if fault == 'missing-binding': binding = None
    if fault == 'wrong-uuid': binding['uuid'] = UUID[:-1]+'d'
    if fault == 'wrong-selector': binding['visible'] = '0'
    def collect():
        return result.collect_retained_role_execution(state.output, receipt_digest=digest,
            plan=plan, preparation_digest=prep, candidate_digest=candidate, rank=0,
            role='BackboneNeck', provider='/app/worker-a', request_id='/app/request/1',
            attempt=1, execution_plan_digest='sha256:'+'1'*64, gpu_binding=binding)
    if fault == 'none':
        assert collect()['device']['gpuUuid'] == UUID
    else:
        with pytest.raises(ValueError): collect()


def _final_component(case='local-cpu', count=2, *, graph='sha256:'+'c'*64):
    role_names = ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')
    roles = {name: {'qualification': 'RETAINED_ROLE_COMPONENT_ONLY'} for name in role_names}
    devices = {} if case == 'local-cpu' else ({0} if case == 'single-node-gpu' else {0, 1})
    return [dict(requestIndex=i, qualification='RETAINED_REQUEST_COMPONENT_ONLY',
        request=dict(lifecycle={'qualification': 'LIFECYCLE_COMPONENT_ONLY'},
                     numerical={'matched': True, 'qualification': 'NUMERICAL_COMPONENT_ONLY'}),
        execution=dict(qualification='RETAINED_DEPENDENCY_COMPONENT_ONLY',
            dependencies={'qualification': 'DEPENDENCY_COMPONENT_ONLY'},
            certifiedGraph={'graphDigest': graph, 'qualification': 'CERTIFIED_GRAPH_COMPONENT_ONLY',
                            'roles': {name: roles[name] for name in role_names if name != 'Merge'}}, roles=roles,
            devices={rank: {} for rank in devices})) for i in range(count)]


@pytest.mark.parametrize('fault', ['none', 'missing-shard', 'fake-merge-ort', 'merge-failed'])
def test_four_role_join_uses_three_ort_graphs_and_retains_native_merge(monkeypatch, fault):
    """Real join/comparator; retained readers are explicit synthetic fixtures."""
    names = ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')
    binding = dict(modelManifestDigest='sha256:'+'a'*64, artifactDigest='sha256:'+'b'*64)
    graph = dict(schema='tiger-yolo-certified-graph-v1', graphDigest='sha256:'+'c'*64,
        roles={role: dict(binding, backend='CPUExecutionProvider',
                         optimizedNodeNames=['conv_kernel_time']) for role in names[:-1]},
        # Synthetic producer provenance: fixture values only; the real document
        # is produced by serialize_certified_graph from ORT reference records.
        referenceProvenance={role: dict(schema='tiger-yolo-role-reference-v1',
            qualification='ORT_GRAPH_PREPARATION_COMPONENT_ONLY',
            ortVersion='fixture-ort-version', optimizedModelDigest='sha256:'+'d'*64,
            assembledModelDigest='sha256:'+'e'*64,
            sessionOptions={'intraOpThreads': 1, 'graphOptimization': 'ORT_ENABLE_BASIC',
                            'allowCpuFallback': False, 'deviceId': 0},
            backend='CPUExecutionProvider') for role in names[:-1]})
    checked_roles = []

    def read_role(*args, role, **kwargs):
        checked_roles.append(role)
        if role == 'Merge' and fault == 'merge-failed':
            raise result.EvidenceError('NATIVE_EXECUTION_BINDING_OR_STATUS')
        assignments = [] if role == 'Merge' else [dict(role=role,
            nodeName='conv_kernel_time', provider='CPUExecutionProvider', modelNode=True)]
        return dict(logPath=role+'.log', native=dict(logDigest='sha256:'+'d'*64,
            observation=dict(nodeProviderAssignments=assignments)))

    monkeypatch.setattr(result, 'collect_retained_role_execution', read_role)
    monkeypatch.setattr(result, 'collect_dependency_result', lambda *args, **kwargs: dict(
        logDigests={role: 'sha256:'+'d'*64 for role in names},
        modelBindings={role: dict(binding) for role in names}))
    if fault == 'missing-shard':
        del graph['roles']['DetectShard1']
    elif fault == 'fake-merge-ort':
        graph['roles']['Merge'] = dict(graph['roles']['BackboneNeck'])

    def collect():
        return result.collect_retained_dependencies(
            {0: dict(root='/unused', receiptDigest='unused', preparationDigest='unused')},
            '/unused', plan={'case': 'local-cpu'}, candidate_digest='unused',
            providers_by_role={role: '/app/'+role for role in names}, request_id='/app/r1',
            attempt=1, execution_plan_digest='sha256:'+'e'*64, certified_graph=graph)

    if fault == 'none':
        value = collect()
        assert set(checked_roles) == set(names)
        assert set(value['roles']) == set(names)
        assert set(value['certifiedGraph']['roles']) == set(names[:-1])
        assert value['roles']['Merge']['native']['observation']['nodeProviderAssignments'] == []
    else:
        reason = ('NATIVE_EXECUTION_BINDING_OR_STATUS' if fault == 'merge-failed'
                  else 'CERTIFIED_GRAPH_ROLE_COVERAGE')
        with pytest.raises(result.EvidenceError, match=reason):
            collect()


@pytest.mark.parametrize('mutation', ['missing-merge', 'merge-as-onnx', 'missing-shard'])
def test_native_merge_is_required_but_is_not_an_ort_graph(mutation):
    rows = _final_component()
    execution = rows[0]['execution']
    if mutation == 'missing-merge':
        del execution['roles']['Merge']
        reason = 'FINAL_VERDICT_ROLE_COVERAGE'
    elif mutation == 'merge-as-onnx':
        execution['certifiedGraph']['roles']['Merge'] = execution['roles']['Merge']
        reason = 'FINAL_VERDICT_GRAPH_BINDING'
    else:
        del execution['certifiedGraph']['roles']['DetectShard1']
        reason = 'FINAL_VERDICT_GRAPH_BINDING'
    plan = {'case': 'local-cpu', 'requests': [dict(index=i, warmup=i == 0) for i in range(2)]}
    with pytest.raises(result.EvidenceError, match=reason):
        result.finalize_normal_verdict(rows, plan=plan, graph_digest='sha256:'+'c'*64)


@pytest.mark.parametrize('case,count', [('local-cpu', 2), ('single-node-gpu', 2),
                                        ('two-node-gpu', 4)])
def test_finalize_normal_verdict_requires_all_components(case, count):
    plan = {'case': case, 'requests': [dict(index=i, warmup=i == 0) for i in range(count)]}
    verdict = result.finalize_normal_verdict(_final_component(case, count), plan=plan,
                                             graph_digest='sha256:'+'c'*64)
    assert verdict['status'] == 'PASS' and verdict['requestCount'] == count


@pytest.mark.parametrize('mutation', ['missing', 'numerical', 'graph', 'roles', 'role-component',
                                      'dependencies', 'graph-qualification', 'devices'])
def test_finalize_normal_verdict_rejects_partial_component(mutation):
    graph = 'sha256:'+'c'*64
    plan = {'case': 'two-node-gpu',
            'requests': [dict(index=i, warmup=i == 0) for i in range(4)]}
    rows = _final_component('two-node-gpu', 4, graph=graph)
    if mutation == 'missing': rows.pop()
    elif mutation == 'numerical': rows[0]['request']['numerical']['matched'] = False
    elif mutation == 'graph': rows[0]['execution']['certifiedGraph']['graphDigest'] = 'sha256:'+'d'*64
    elif mutation == 'roles': rows[0]['execution']['roles'].pop('Merge')
    elif mutation == 'role-component': rows[0]['execution']['roles']['Merge'] = {}
    elif mutation == 'dependencies': rows[0]['execution']['dependencies']['qualification'] = 'BAD'
    elif mutation == 'graph-qualification': rows[0]['execution']['certifiedGraph']['qualification'] = 'BAD'
    else: rows[0]['execution']['devices'] = {0: {}}
    with pytest.raises(ValueError):
        result.finalize_normal_verdict(rows, plan=plan, graph_digest=graph)


def test_collect_normal_verdict_owns_complete_request_loop(monkeypatch):
    graph = 'sha256:'+'c'*64
    plan = {'runId': 'run', 'case': 'local-cpu',
            'requests': [dict(index=i, warmup=i == 0, requestId='/run/' + str(i)) for i in range(2)]}
    providers = {name: '/app/' + name for name in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')}
    calls = []

    def retained(nodes, reference, **kwargs):
        assert kwargs['certified_graph']['requestIndex'] == kwargs['request_index']
        calls.append((reference, kwargs['request_index']))
        return _final_component('local-cpu', 2, graph=graph)[kwargs['request_index']]

    monkeypatch.setattr(result, 'collect_retained_request', retained)
    import runtime.yolo_graph_reference as producer
    monkeypatch.setattr(producer, 'read_request_reference',
        lambda path, **kwargs: {'graphDigest': graph, 'requestIndex': int(path.parent.name)})
    verdict = result.collect_normal_verdict(
        {0: {'root': '/node0'}}, ['warmup-reference', 'measured-reference'],
        plan=plan, runtime_candidate_digest='sha256:'+'a'*64,
        placement_candidate_id='yolo-v1', placement_candidate_digest='sha256:'+'b'*64,
        graph_digest=graph, catalogue_digest='sha256:'+'d'*64,
        providers_by_role=providers,
        certified_graph={'graphDigest': graph})
    assert verdict['qualification'] == 'NORMAL_EXPERIMENT_PASS'
    assert calls == [('warmup-reference', 0), ('measured-reference', 1)]


@pytest.mark.parametrize('references,certified_graph,reason', [
    (['only-one'], {'graphDigest': 'sha256:'+'c'*64}, 'NORMAL_VERDICT_REFERENCE_COVERAGE'),
    (['warmup', 'measured'], None, 'NORMAL_VERDICT_EXPECTED_GRAPH'),
])
def test_collect_normal_verdict_rejects_incomplete_inputs(references, certified_graph, reason):
    plan = {'case': 'local-cpu',
            'requests': [dict(index=0, warmup=True), dict(index=1, warmup=False)]}
    with pytest.raises(ValueError, match=reason):
        result.collect_normal_verdict(
            {0: {'root': '/node0'}}, references, plan=plan,
            runtime_candidate_digest='sha256:'+'a'*64,
            placement_candidate_id='yolo-v1', placement_candidate_digest='sha256:'+'b'*64,
            graph_digest='sha256:'+'c'*64, catalogue_digest='sha256:'+'d'*64,
            providers_by_role={}, certified_graph=certified_graph)


def test_normal_verdict_rejects_shared_graph_without_user_record(tmp_path):
    plan = {'runId': 'run', 'case': 'local-cpu',
            'requests': [dict(index=i, warmup=i == 0, requestId='/run/' + str(i)) for i in range(2)]}
    with pytest.raises(ValueError, match='NORMAL_VERDICT_REQUEST_REFERENCE'):
        result.collect_normal_verdict(
            {0: {'root': str(tmp_path)}}, ['warmup', 'measured'], plan=plan,
            runtime_candidate_digest='sha256:'+'a'*64, placement_candidate_id='yolo',
            placement_candidate_digest='sha256:'+'b'*64, graph_digest='sha256:'+'c'*64,
            catalogue_digest='sha256:'+'d'*64, providers_by_role={},
            certified_graph={'graphDigest': 'sha256:'+'c'*64, 'roles': {'callerSupplied': 'not authority'}})


def _expected_rejection():
    candidate = 'sha256:' + 'a' * 64
    request = '/run/negative/requests/0'
    return dict(
        schema='tiger-yolo-expected-rejection-v1', status='REJECTED',
        qualification='EXPECTED_REJECTION_COMPONENT_ONLY',
        case='negative-dependency', runId='run-1', requestId=request,
        attempt=1, candidateDigest=candidate,
        selection=dict(status='COMMITTED', selectedProvider='/run/provider/BackboneNeck',
                       selectionCount=1, reselectionCount=0),
        failure=dict(boundary='DEPENDENCY_DATA_MISSING',
                     edge=dict(producer='BackboneNeck', consumer='DetectShard0',
                               plannedName='/run/data/backbone-to-head0'),
                     observedAfterSelection=True, reselected=False),
        response=dict(present=False, success=False), cleanup=dict(
            qualification='CLEANUP_COMPONENT_ONLY', allChildrenReaped=True,
            forced=False, remainingChildren=0, deadlineSatisfied=True),
        elapsedMs=812, deadlineMs=60000)


@pytest.mark.parametrize('failed_response', [False, True])
def test_finalize_expected_rejection_requires_selection_failure_and_cleanup(failed_response):
    rejection = _expected_rejection()
    rejection['response']['present'] = failed_response
    value = result.finalize_expected_rejection(
        rejection,
        plan={'case': 'negative-dependency', 'runId': 'run-1', 'requests': [
            {'index': 0, 'warmup': False, 'requestId': '/run/negative/requests/0',
             'output': '/run/output/0'}]},
        request_id='/run/negative/requests/0', attempt=1,
        candidate_digest='sha256:' + 'a' * 64, request_deadline_ms=60000)
    assert value['qualification'] == 'EXPECTED_REJECTION_PASS'
    assert value['failureBoundary'] == 'DEPENDENCY_DATA_MISSING'


@pytest.mark.parametrize('mutation', [
    'wrong-case', 'no-selection', 'reselection', 'early-failure',
    'response', 'forced-cleanup', 'unbounded', 'wrong-candidate',
])
def test_finalize_expected_rejection_rejects_false_negative_pass(mutation):
    value = _expected_rejection()
    if mutation == 'wrong-case':
        value['case'] = 'two-node-gpu'
    elif mutation == 'no-selection':
        value['selection']['status'] = 'MISSING'
    elif mutation == 'reselection':
        value['selection']['reselectionCount'] = 1
    elif mutation == 'early-failure':
        value['failure']['observedAfterSelection'] = False
    elif mutation == 'response':
        value['response']['present'] = True
        value['response']['success'] = True
    elif mutation == 'forced-cleanup':
        value['cleanup']['forced'] = True
    elif mutation == 'unbounded':
        value['elapsedMs'] = 60001
    else:
        value['candidateDigest'] = 'sha256:' + 'b' * 64
    with pytest.raises(ValueError):
        result.finalize_expected_rejection(
            value,
            plan={'case': 'negative-dependency', 'runId': 'run-1', 'requests': [
                {'index': 0, 'warmup': False, 'requestId': '/run/negative/requests/0',
                 'output': '/run/output/0'}]},
            request_id='/run/negative/requests/0', attempt=1,
            candidate_digest='sha256:' + 'a' * 64, request_deadline_ms=60000)


@pytest.mark.parametrize('fault', ['none', 'pid', 'request', 'execution-plan', 'old-profile',
    'cpu-assignment', 'profile-symlink', 'changed-log', 'wrong-role', 'cpu-gpu-exposure'])
def test_retained_execution_uses_receipt_pid_and_scoped_profile(tmp_path, fault):
    state, rows = worker(tmp_path)
    row = observation()
    launch = next(r for r in state.launches if r['role'] == 'BackboneNeck')
    row.update(processId=str(launch['pid']), roles=['BackboneNeck'], runnerKind='onnxruntime-cpu',
        profileRequestId='/app/request/1', providerProfilePath='/output/ort/profile.json',
        gpuUuid='', gpuUuids='', cudaVisibleDevices='', gpuIdentitySource='')
    if fault == 'cpu-gpu-exposure': row['cudaVisibleDevices'] = '0'
    row['nodeProviderAssignments'][0].update(provider='CPUExecutionProvider', nodeName='conv_kernel_time')
    if fault == 'pid': row['processId'] = str(launch['pid']+1)
    if fault == 'old-profile': row['profileRequestId'] = 'old-request'
    log = state.output/'logs/BackboneNeck.log'
    log.write_text(log.read_text()+'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED '+json.dumps(row)+'\n')
    profile = state.output/'BackboneNeck/ort/profile.json'
    profile.parent.mkdir(parents=True)
    profile.write_text(json.dumps([dict(cat='Node', name='conv_kernel_time',
        args={'provider': 'CUDAExecutionProvider' if fault == 'cpu-assignment' else 'CPUExecutionProvider'})]))
    if fault == 'profile-symlink':
        other = profile.parent/'other.json'
        profile.rename(other)
        profile.symlink_to(other)
    result.write_worker_receipt(state, rows)
    digest = 'sha256:'+hashlib.sha256((state.output/'node-receipt.json').read_bytes()).hexdigest()
    if fault == 'changed-log': log.write_text('changed')
    plan, prep, candidate = state._preparation_binding
    def collect():
        return result.collect_retained_role_execution(state.output,
            receipt_digest=digest, plan=plan, preparation_digest=prep,
            candidate_digest=candidate, rank=0,
            role='controller' if fault == 'wrong-role' else 'BackboneNeck',
            provider='/app/worker-a', request_id='other' if fault == 'request' else '/app/request/1',
            attempt=1, execution_plan_digest='sha256:'+('2' if fault == 'execution-plan' else '1')*64)
    if fault == 'none':
        evidence = collect()
        assert evidence['native']['observation']['processId'] == launch['pid']
        assert evidence['profile'] is not None
        assert evidence['qualification'] == 'RETAINED_ROLE_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError): collect()


@pytest.mark.parametrize('mode', ['local-cpu', 'single-node-gpu', 'two-node-gpu'])
@pytest.mark.parametrize('fault', ['none', 'missing-node', 'provider-cover', 'changed-log', 'device-field'])
def test_cross_process_collection_dispatches_frozen_role_owners(tmp_path, monkeypatch, mode, fault):
    """Dispatch boundary doubles; actual readers exercised above."""
    nodes = {rank: dict(root=tmp_path/str(rank), receiptDigest='receipt-'+str(rank),
        preparationDigest='preparation-'+str(rank))
        for rank in ([0, 1] if mode == 'two-node-gpu' else [0])}
    if mode != 'local-cpu':
        for rank, node in nodes.items():
            node.update(allocationDigest='allocation-'+str(rank), gpuProbeDigest='probe-'+str(rank))
    providers = {role: '/app/'+role for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')}
    calls = []
    def role(root, **args):
        owner = 1 if mode == 'two-node-gpu' and args['role'].startswith('Detect') else 0
        assert args['rank'] == owner and root == tmp_path/str(owner)
        assert args['receipt_digest'] == 'receipt-'+str(owner)
        assert args['preparation_digest'] == 'preparation-'+str(owner)
        assert args['provider'] == providers[args['role']]
        assert args['gpu_binding'] == (dict(uuid=UUID[:-1]+str(owner), visible=str(owner+2))
            if mode != 'local-cpu' and args['role'] != 'Merge' else None)
        calls.append(args['role'])
        return dict(logPath=str(root/(args['role']+'.log')), native={'logDigest': 'same'})
    monkeypatch.setattr(result, 'collect_retained_role_execution', role)
    def device(root, **args):
        rank = args['rank']
        assert root == tmp_path/str(rank)
        assert args['allocation_digest'] == 'allocation-'+str(rank)
        assert args['gpu_probe_digest'] == 'probe-'+str(rank)
        return dict(gpuBinding=dict(uuid=UUID[:-1]+str(rank), visible=str(rank+2)), uid=1000,
            allocation=dict(jobId='123', stepId='0', submissionKey='key',
                hosts=['node0','node1'], hostname='node'+str(rank)))
    monkeypatch.setattr(result, 'read_retained_device_binding', device)
    def dependency(path, logs, **args):
        assert path == tmp_path/'public.json' and set(logs) == set(providers)
        return {'logDigests': {name: 'changed' if fault == 'changed-log' else 'same' for name in logs}}
    monkeypatch.setattr(result, 'collect_dependency_result', dependency)
    if fault == 'missing-node': nodes.pop(max(nodes))
    if fault == 'provider-cover': providers.pop('Merge')
    if fault == 'device-field':
        if mode == 'local-cpu': nodes[0]['gpuBinding'] = dict(uuid=UUID, visible='0')
        else: nodes[0].pop('allocationDigest')
    def collect():
        return result.collect_retained_dependencies(nodes, tmp_path/'public.json',
            plan={'case': mode}, candidate_digest='candidate', providers_by_role=providers,
            request_id='request', attempt=1, execution_plan_digest='execution')
    if fault == 'none':
        assert collect()['qualification'] == 'RETAINED_DEPENDENCY_COMPONENT_ONLY'
        assert set(calls) == set(providers)
    else:
        with pytest.raises(ValueError): collect()
