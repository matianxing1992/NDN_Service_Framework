"""Source-shaped Slurm 24.05 JSON and mocked read-only command boundary.

Not a live allocation: the site presently has no running job for this user.
"""
import json
from pathlib import Path
import sys
from types import SimpleNamespace as NS

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_allocation as allocation

KEY = 'spec183-'+'a'*64


def fixture():
    def document(collection, record):
        return {collection: [record], 'errors': [], 'warnings': [],
            'meta': {'plugin': {'data_parser': 'data_parser/v0.0.41'}, 'slurm': {'cluster': 'itiger'}}}
    job = document('jobs', dict(job_id=123, user_id=1000, comment=KEY,
        job_state=['RUNNING'], partition='gpu', nodes='node[0-1]',
        node_count={'set': True, 'infinite': False, 'number': 2},
        tres_per_node='gres/gpu:rtx_5000:1'))
    step = document('steps', dict(id='123.0', state=['RUNNING'], nodes='node0,node1', number_tasks=2))
    env = dict(SLURM_JOB_ID='123', SLURM_STEP_ID='0', SLURM_NODEID='1', SLURM_PROCID='1',
        SLURM_LOCALID='0', SLURM_NTASKS='2', SLURM_JOB_NUM_NODES='2', SLURM_GPUS_ON_NODE='1',
        SLURMD_NODENAME='node1', SLURM_STEP_GPUS='3', CUDA_VISIBLE_DEVICES='0')
    expected = dict(job_id='123', submission_key=KEY, rank=1, node_count=2,
        partition='gpu', gpu_type='rtx_5000')
    return job, step, env, expected


@pytest.mark.parametrize('fault', ['none', 'empty-jobs', 'null-steps', 'wrong-job', 'wrong-comment',
    'wrong-user', 'not-running', 'pending-step', 'wrong-step', 'wrong-host', 'duplicate-host',
    'step-hosts', 'wrong-rank', 'extra-task', 'multiple-gpus', 'gpu-class', 'task-job',
    'batch', 'local-rank', 'numeric-bool', 'wrong-parser', 'wrong-cluster', 'warnings', 'global-gpus', 'selector'])
def test_scheduler_and_task_must_match_registered_job(fault):
    job, step, env, expected = fixture()
    hosts = step_hosts = b'node0\nnode1\n'
    if fault == 'empty-jobs': job['jobs'] = []
    if fault == 'null-steps': step['steps'] = None
    if fault == 'wrong-job': job['jobs'][0]['job_id'] = 124
    if fault == 'wrong-comment': job['jobs'][0]['comment'] = 'someone-else'
    if fault == 'wrong-user': job['jobs'][0]['user_id'] = 1001
    if fault == 'not-running': job['jobs'][0]['job_state'] = ['COMPLETED']
    if fault == 'pending-step': step['steps'][0]['state'] = ['PENDING']
    if fault == 'wrong-step': step['steps'][0]['id'] = '123.1'
    if fault == 'wrong-host': hosts = b'node1\nnode0\n'
    if fault == 'duplicate-host': hosts = b'node1\nnode1\n'
    if fault == 'step-hosts': step_hosts = b'node0\nnode2\n'
    if fault == 'wrong-rank': env['SLURM_NODEID'] = '0'
    if fault == 'extra-task': step['steps'][0]['number_tasks'] = 4
    if fault == 'multiple-gpus': env['SLURM_GPUS_ON_NODE'] = '2'
    if fault == 'gpu-class': job['jobs'][0]['tres_per_node'] = 'gres/gpu:a100:1'
    if fault == 'task-job': env['SLURM_JOB_ID'] = '124'
    if fault == 'batch': env['SLURM_STEP_ID'] = 'batch'
    if fault == 'local-rank': env['SLURM_LOCALID'] = '1'
    if fault == 'numeric-bool': job['jobs'][0]['node_count']['number'] = True
    if fault == 'wrong-parser': job['meta']['plugin']['data_parser'] = 'data_parser/v0.0.42'
    if fault == 'wrong-cluster': job['meta']['slurm']['cluster'] = 'another-cluster'
    if fault == 'warnings': step['warnings'] = [{'warning': 'incomplete'}]
    if fault == 'global-gpus': env['SLURM_STEP_GPUS'] = '0,1'
    if fault == 'selector': env['CUDA_VISIBLE_DEVICES'] = '0,1'
    def validate():
        return allocation.validate_task_allocation(json.dumps(job).encode(), json.dumps(step).encode(),
            hosts, **expected, environment=env, hostname='node1', uid=1000, step_hostnames=step_hosts)
    if fault == 'none':
        receipt = validate()
        assert receipt['globalStepGpuId'] == '3' and receipt['visible'] == '0'
        assert receipt['qualification'] == 'SLURM_TASK_IDENTITY_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError): validate()


def test_single_node_gpu_case_has_one_task_and_rank_zero():
    job, step, env, expected = fixture()
    expected.update(rank=0, node_count=1)
    job['jobs'][0]['node_count']['number'] = 1
    job['jobs'][0]['nodes'] = step['steps'][0]['nodes'] = 'node0'
    step['steps'][0]['number_tasks'] = 1
    env.update(SLURM_NODEID='0', SLURM_PROCID='0', SLURM_NTASKS='1',
        SLURM_JOB_NUM_NODES='1', SLURMD_NODENAME='node0')
    receipt = allocation.validate_task_allocation(json.dumps(job).encode(), json.dumps(step).encode(),
        b'node0\n', **expected, environment=env, hostname='node0', uid=1000, step_hostnames=b'node0\n')
    assert receipt['hosts'] == ['node0'] and receipt['rank'] == 0


@pytest.mark.parametrize('fault', ['none', 'wrong-env-job', 'empty-success-job'])
def test_capture_runs_only_bounded_read_only_scontrol(monkeypatch, fault):
    job, step, env, expected = fixture()
    if fault == 'wrong-env-job': env['SLURM_JOB_ID'] = '124'
    if fault == 'empty-success-job': job['jobs'] = []
    monkeypatch.setattr(allocation.os, 'environ', env)
    monkeypatch.setattr(allocation.os, 'getuid', lambda: 1000)
    monkeypatch.setattr(allocation.socket, 'gethostname', lambda: 'node1')
    calls = []
    def run(argv, **options):
        calls.append(argv)
        assert options['check'] is True and 0 < options['timeout'] <= 5
        assert options['env'] == {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}
        assert argv[0] == '/usr/bin/scontrol'
        if argv[1:] == ['--json', 'show', 'job', '123']: output = json.dumps(job).encode()
        elif argv[1:] == ['--json', 'show', 'step', '123.0']: output = json.dumps(step).encode()
        elif argv[1:3] == ['show', 'hostnames']: output = b'node0\nnode1\n'
        else: pytest.fail('unexpected side effect command')
        return NS(stdout=output, stderr=b'', returncode=0)
    monkeypatch.setattr(allocation.subprocess, 'run', run)
    if fault == 'none':
        result = allocation.capture_task_allocation(**expected, seconds=5)
        assert result['receipt']['hostname'] == 'node1' and len(calls) == 4
        assert result['task']['environment'] == env
    else:
        with pytest.raises(ValueError): allocation.capture_task_allocation(**expected, seconds=5)
        assert len(calls) == (0 if fault == 'wrong-env-job' else 1)


@pytest.mark.parametrize('fault', ['none', 'missing-expected', 'wrong-selector', 'scheduler-reject'])
def test_worker_binds_allocation_before_gpu_or_service_commands(tmp_path, monkeypatch, fault):
    from runtime.yolo_worker import NodeRuntime
    from test_yolo_worker import prepared
    state = NodeRuntime(**prepared(tmp_path, rank=1))
    state._preparation_binding = ({'runId': 'run'}, 'sha256:'+'1'*64, 'sha256:'+'2'*64)
    state._verify_prepared_boundary = lambda: None  # Preparation is a separate tested boundary.
    job, step, env, expected = fixture()
    calls = []
    def capture(**kwargs):
        calls.append(kwargs)
        assert kwargs == dict(expected, seconds=5)
        if fault == 'scheduler-reject': raise ValueError('scheduler rejected')
        return dict(receipt={'visible': '3' if fault == 'wrong-selector' else '0'},
            task={'environment': env, 'uid': 1000, 'hostname': 'node1'},
            job=json.dumps(job).encode(), step=json.dumps(step).encode(),
            hosts=b'node0\nnode1\n', stepHosts=b'node0\nnode1\n')
    monkeypatch.setattr(allocation, 'capture_task_allocation', capture)
    argument = {k: v for k, v in expected.items() if k not in ('rank', 'node_count')}
    if fault == 'missing-expected': argument = None
    try:
        if fault == 'none':
            record = state.verify_allocation(argument, seconds=5)
            assert record['receipt']['visible'] == '0'
            saved = json.loads((state.output/'slurm-allocation.json').read_text())
            assert saved['candidateDigest'] == 'sha256:'+'2'*64
            assert saved['task']['environment'] == env
            assert json.loads(saved['sources']['job']) == job
        else:
            with pytest.raises(ValueError): state.verify_allocation(argument, seconds=5)
            assert state.allocation is None and not (state.output/'slurm-allocation.json').exists()
        assert state.launches == []  # No container, GPU probe or Provider launch.
    finally:
        state.close()
