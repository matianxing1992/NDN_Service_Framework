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
            node['gpuBinding'] = dict(uuid=UUID[:-1]+str(rank), visible=str(rank+2))
    providers = {role: '/app/'+role for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')}
    calls = []
    def role(root, **args):
        owner = 1 if mode == 'two-node-gpu' and args['role'].startswith('Detect') else 0
        assert args['rank'] == owner and root == tmp_path/str(owner)
        assert args['receipt_digest'] == 'receipt-'+str(owner)
        assert args['preparation_digest'] == 'preparation-'+str(owner)
        assert args['provider'] == providers[args['role']]
        assert args['gpu_binding'] == (nodes[owner]['gpuBinding']
            if mode != 'local-cpu' and args['role'] != 'Merge' else None)
        calls.append(args['role'])
        return dict(logPath=str(root/(args['role']+'.log')), native={'logDigest': 'same'})
    monkeypatch.setattr(result, 'collect_retained_role_execution', role)
    def dependency(path, logs, **args):
        assert path == tmp_path/'public.json' and set(logs) == set(providers)
        return {'logDigests': {name: 'changed' if fault == 'changed-log' else 'same' for name in logs}}
    monkeypatch.setattr(result, 'collect_dependency_result', dependency)
    if fault == 'missing-node': nodes.pop(max(nodes))
    if fault == 'provider-cover': providers.pop('Merge')
    if fault == 'device-field':
        if mode == 'local-cpu': nodes[0]['gpuBinding'] = dict(uuid=UUID, visible='0')
        else: nodes[0].pop('gpuBinding')
    def collect():
        return result.collect_retained_dependencies(nodes, tmp_path/'public.json',
            plan={'case': mode}, candidate_digest='candidate', providers_by_role=providers,
            request_id='request', attempt=1, execution_plan_digest='execution')
    if fault == 'none':
        assert collect()['qualification'] == 'RETAINED_DEPENDENCY_COMPONENT_ONLY'
        assert set(calls) == set(providers)
    else:
        with pytest.raises(ValueError): collect()
