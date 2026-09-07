"""Real receipt/log readers with synthetic Slurm/CUDA input, not a GPU run."""
import hashlib
import json
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result
from runtime.yolo_allocation import validate_task_allocation
from runtime.yolo_gpu_probe import MARKER
from test_yolo_allocation import fixture
from test_yolo_node_receipt import worker

UUID = 'GPU-12345678-1234-1234-1234-123456789abc'


def digest(content):
    return 'sha256:'+hashlib.sha256(content).hexdigest()


def retained(tmp_path, fault='none'):
    state, rows = worker(tmp_path)
    plan, prep, candidate = state._preparation_binding
    state.mode = plan['case'] = 'single-node-gpu'
    job, step, env, expected = fixture()
    expected.update(rank=0, node_count=1)
    job['jobs'][0]['node_count']['number'] = 1
    job['jobs'][0]['nodes'] = step['steps'][0]['nodes'] = 'node0'
    step['steps'][0]['number_tasks'] = 1
    env.update(SLURM_NODEID='0', SLURM_PROCID='0', SLURM_NTASKS='1',
        SLURM_JOB_NUM_NODES='1', SLURMD_NODENAME='node0')
    sources = dict(job=json.dumps(job), step=json.dumps(step), hosts='node0\n', stepHosts='node0\n')
    observed = validate_task_allocation(sources['job'].encode(), sources['step'].encode(), b'node0\n',
        **expected, environment=env, hostname='node0', uid=1000, step_hostnames=b'node0\n')
    expected = {k: v for k,v in expected.items() if k not in ('rank', 'node_count')}
    allocation = dict(schema='tiger-yolo-allocation-v1', runId=plan['runId'],
        preparationDigest=prep, candidateDigest=candidate, expected=expected,
        receipt=observed, sources=sources, task=dict(hostname='node0', uid=1000, environment=env))
    if fault == 'wrong-candidate': allocation['candidateDigest'] = 'sha256:'+'f'*64
    if fault == 'wrong-run': allocation['runId'] = 'another-run'
    if fault == 'wrong-summary': allocation['receipt']['stepId'] = '7'
    if fault == 'extra-environment': allocation['task']['environment']['UNRELATED'] = 'not-retained'
    if fault == 'non-running-job':
        job['jobs'][0]['job_state'] = ['PENDING']
        allocation['sources']['job'] = json.dumps(job)
    path = state.output/'slurm-allocation.json'
    path.write_text(json.dumps(allocation))
    allocation_hash = digest(path.read_bytes())
    nonce = 'e'*64
    binding = dict(uuid=UUID, visible='0')
    payload = (MARKER+json.dumps(dict(schema='tiger-cuda-device-v1', nonce=nonce,
        **binding, source='cuda-runtime-pci+driver-uuid'))+'\n').encode()
    relative = 'logs/BackboneNeck-gpu-device.log'
    (state.output/relative).write_bytes(payload)
    probe = dict(schema='tiger-yolo-gpu-probe-v1', rank=0, role='BackboneNeck', nonce=nonce,
        binding=dict(binding), logPath=relative, logDigest=digest(payload),
        allocationDigest=allocation_hash, qualification='CUDA_VISIBILITY_COMPONENT_ONLY')
    if fault == 'wrong-probe-rank': probe['rank'] = 1
    if fault == 'wrong-nonce': probe['nonce'] = 'd'*64
    if fault == 'wrong-uuid': probe['binding']['uuid'] = UUID[:-1]+'d'
    if fault == 'wrong-visible': probe['binding']['visible'] = '3'
    if fault == 'wrong-allocation-link': probe['allocationDigest'] = 'sha256:'+'f'*64
    path = state.output/'gpu-probe.json'
    path.write_text(json.dumps(probe))
    probe_hash = digest(path.read_bytes())
    launch = dict(role='BackboneNeck', invocation='gpu-device', pid=300, argv=['fixture', 'probe'])
    if fault == 'late-probe': state.launches.append(launch)
    else: state.launches.insert(0, launch)
    rows.append(dict(name='BackboneNeck-gpu-device', pid=300, kind='finite', exitCode=0,
        reaped=True, forced=False, leaseReleased=True, exitedBeforeCleanup=False))
    result.write_worker_receipt(state, rows)
    receipt_hash = digest((state.output/'node-receipt.json').read_bytes())
    if fault == 'changed-log': (state.output/relative).write_text('changed')
    if fault == 'symlink':
        original = state.output/'original-allocation.json'
        (state.output/'slurm-allocation.json').rename(original)
        (state.output/'slurm-allocation.json').symlink_to(original)
    arguments = dict(receipt_digest=receipt_hash, allocation_digest=allocation_hash,
        gpu_probe_digest=probe_hash, plan=plan, preparation_digest=prep, candidate_digest=candidate,
        rank=0, expected=expected)
    if fault == 'wrong-trusted-hash': arguments['gpu_probe_digest'] = 'sha256:'+'0'*64
    if fault == 'missing-journal': arguments['expected'] = None
    return state.output, arguments


@pytest.mark.parametrize('fault', ['none', 'wrong-candidate', 'wrong-run', 'wrong-summary',
    'extra-environment', 'non-running-job', 'wrong-probe-rank', 'wrong-nonce', 'wrong-uuid',
    'wrong-visible', 'wrong-allocation-link', 'late-probe', 'changed-log', 'symlink',
    'wrong-trusted-hash', 'missing-journal'])
def test_retained_device_join_recomputes_instead_of_trusting_summaries(tmp_path, fault):
    root, args = retained(tmp_path, fault)
    if fault == 'none':
        evidence = result.read_retained_device_binding(root, **args)
        assert evidence['gpuBinding'] == dict(uuid=UUID, visible='0')
        assert evidence['allocation']['globalStepGpuId'] == '3'
        assert evidence['qualification'] == 'RETAINED_DEVICE_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError): result.read_retained_device_binding(root, **args)


@pytest.mark.parametrize('fault', ['step', 'hostname', 'gpu', 'uid'])
def test_cross_node_allocation_conflicts_stop_before_role_collection(tmp_path, monkeypatch, fault):
    nodes = {rank: dict(root=tmp_path/str(rank), receiptDigest='fixture', preparationDigest='fixture',
        allocationDigest='fixture', gpuProbeDigest='fixture') for rank in (0,1)}
    def device(root, **args):
        rank = args['rank']
        evidence = dict(uid=1000, gpuBinding=dict(uuid=UUID[:-1]+str(rank), visible='0'),
            allocation=dict(jobId='123', stepId='0', submissionKey='key',
                hosts=['node0','node1'], hostname='node'+str(rank)))
        if rank == 1:
            if fault == 'step': evidence['allocation']['stepId'] = '1'
            if fault == 'hostname': evidence['allocation']['hostname'] = 'node0'
            if fault == 'gpu': evidence['gpuBinding']['uuid'] = UUID[:-1]+'0'
            if fault == 'uid': evidence['uid'] = 1001
        return evidence
    monkeypatch.setattr(result, 'read_retained_device_binding', device)
    monkeypatch.setattr(result, 'collect_retained_role_execution',
        lambda *a, **k: pytest.fail('must reject before role evidence is consumed'))
    with pytest.raises(ValueError, match='RETAINED_ALLOCATION_CROSS_NODE'):
        result.collect_retained_dependencies(nodes, tmp_path/'public', plan={'case':'two-node-gpu'},
            candidate_digest='fixture', request_id='request', attempt=1, execution_plan_digest='fixture',
            providers_by_role={r:'/'+r for r in ('BackboneNeck','DetectShard0','DetectShard1','Merge')})
