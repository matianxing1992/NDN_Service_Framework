"""Bind a running Slurm task to the submitted Spec183 job before CUDA probes.

Slurm 24.05/data_parser v0.0.41 is the observed site interface. This checks
scheduler/task identity, not a physical GPU UUID or inference correctness.
"""
import hashlib
import json
import math
import os
import re
import socket
import subprocess
import time

from runtime.yolo_gpu_probe import SELECTOR


def _document(payload, collection):
    if not isinstance(payload, bytes) or len(payload) > 4*1024*1024:
        raise ValueError('SLURM_DOCUMENT_SIZE')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('SLURM_DOCUMENT_DUPLICATE_KEY')
            result[key] = value
        return result
    def constant(value):
        raise ValueError('SLURM_DOCUMENT_NONFINITE')
    document = json.loads(payload, object_pairs_hook=pairs, parse_constant=constant)
    if (not isinstance(document, dict) or document.get('errors') != []
            or document.get('warnings') != []
            or not isinstance(document.get(collection), list)
            or len(document[collection]) != 1):
        raise ValueError('SLURM_DOCUMENT_NOT_ONE_RECORD')
    meta = document.get('meta')
    if (not isinstance(meta, dict) or not isinstance(meta.get('plugin'), dict)
            or meta['plugin'].get('data_parser') != 'data_parser/v0.0.41'
            or not isinstance(meta.get('slurm'), dict) or meta['slurm'].get('cluster') != 'itiger'):
        raise ValueError('SLURM_DOCUMENT_PARSER')
    record = document[collection][0]
    if not isinstance(record, dict):
        raise ValueError('SLURM_DOCUMENT_RECORD')
    return record


def _expected(job_id, submission_key, rank, node_count, partition, gpu_type):
    if (not isinstance(job_id, str) or re.fullmatch(r'[1-9][0-9]{0,9}', job_id) is None
            or not isinstance(submission_key, str)
            or re.fullmatch(r'spec183-[0-9a-f]{64}', submission_key) is None
            or type(node_count) is not int or node_count not in (1, 2)
            or type(rank) is not int or not 0 <= rank < node_count
            or any(not isinstance(v, str) or re.fullmatch(r'[A-Za-z0-9_-]{1,64}', v) is None
                   for v in (partition, gpu_type))):
        raise ValueError('SLURM_EXPECTED_BINDING')


def validate_task_allocation(job_payload, step_payload, hostnames, *, job_id,
                             submission_key, rank, node_count, partition,
                             gpu_type, environment, hostname, uid, step_hostnames):
    """Validate outputs of trusted scontrol commands against external inputs.

    The expected job id/comment come from the durable submission journal,
    not from SLURM_JOB_ID. hostnames must be scontrol expansion of job.nodes.
    Do not call this with caller-authored JSON and label it a live allocation.
    """
    _expected(job_id, submission_key, rank, node_count, partition, gpu_type)
    job = _document(job_payload, 'jobs')
    step = _document(step_payload, 'steps')
    count = job.get('node_count')
    if (type(uid) is not int or uid < 0 or type(job.get('job_id')) is not int
            or job['job_id'] != int(job_id) or type(job.get('user_id')) is not int
            or job['user_id'] != uid or job.get('comment') != submission_key
            or job.get('job_state') != ['RUNNING'] or job.get('partition') != partition
            or count != {'set': True, 'infinite': False, 'number': node_count}
            or count.get('set') is not True or count.get('infinite') is not False
            or type(count.get('number')) is not int
            or job.get('tres_per_node') != 'gres/gpu:'+gpu_type+':1'):
        raise ValueError('SLURM_JOB_BINDING')
    if (not isinstance(hostnames, bytes) or len(hostnames) > 4096
            or not isinstance(step_hostnames, bytes) or len(step_hostnames) > 4096
            or not isinstance(hostname, str)):
        raise ValueError('SLURM_HOSTS')
    hosts = hostnames.decode('ascii').splitlines()
    if (len(hosts) != node_count or len(set(hosts)) != node_count
            or step_hostnames.decode('ascii').splitlines() != hosts
            or any(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]{0,252}', h) is None for h in hosts)
            or hostname != hosts[rank]):
        raise ValueError('SLURM_HOST_RANK')
    step_id = environment.get('SLURM_STEP_ID', '')
    if (not isinstance(step_id, str) or re.fullmatch(r'0|[1-9][0-9]{0,9}', step_id) is None
            or step.get('id') != job_id+'.'+step_id or step.get('state') != ['RUNNING']
            or type(step.get('number_tasks')) is not int or step['number_tasks'] != node_count):
        raise ValueError('SLURM_STEP_BINDING')
    expected = {'SLURM_JOB_ID': job_id, 'SLURM_NODEID': str(rank),
        'SLURM_PROCID': str(rank), 'SLURM_LOCALID': '0', 'SLURM_NTASKS': str(node_count),
        'SLURM_JOB_NUM_NODES': str(node_count), 'SLURM_GPUS_ON_NODE': '1',
        'SLURMD_NODENAME': hosts[rank]}
    if any(environment.get(k) != v for k, v in expected.items()):
        raise ValueError('SLURM_TASK_BINDING')
    gpu_id = environment.get('SLURM_STEP_GPUS', '')
    visible = environment.get('CUDA_VISIBLE_DEVICES', '')
    if (not isinstance(gpu_id, str) or re.fullmatch(r'0|[1-9][0-9]*', gpu_id) is None
            or not isinstance(visible, str) or re.fullmatch(SELECTOR, visible) is None):
        raise ValueError('SLURM_TASK_GPU_BINDING')
    digest = lambda b: 'sha256:'+hashlib.sha256(b).hexdigest()
    return dict(schema='tiger-slurm-task-v1', jobId=job_id, stepId=step_id,
        submissionKey=submission_key, rank=rank, hostname=hostname, hosts=hosts,
        globalStepGpuId=gpu_id, visible=visible, jobDigest=digest(job_payload),
        stepDigest=digest(step_payload), hostsDigest=digest(hostnames),
        stepHostsDigest=digest(step_hostnames),
        qualification='SLURM_TASK_IDENTITY_COMPONENT_ONLY')


def capture_task_allocation(*, job_id, submission_key, rank, node_count,
                            partition, gpu_type, seconds):
    """Read-only capture inside srun; never allocates or submits a job."""
    _expected(job_id, submission_key, rank, node_count, partition, gpu_type)
    if (isinstance(seconds, bool) or not isinstance(seconds, (float, int))
            or not math.isfinite(seconds) or seconds <= 0):
        raise ValueError('SLURM_QUERY_BUDGET')
    environment = dict(os.environ)
    hostname, uid = socket.gethostname(), os.getuid()
    step_id = environment.get('SLURM_STEP_ID', '')
    if (environment.get('SLURM_JOB_ID') != job_id or not isinstance(step_id, str)
            or re.fullmatch(r'0|[1-9][0-9]{0,9}', step_id) is None):
        raise ValueError('SLURM_QUERY_NOT_EXPECTED_TASK')
    deadline = time.monotonic()+seconds
    def query(arguments):
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            raise TimeoutError('SLURM_QUERY_DEADLINE')
        result = subprocess.run(['/usr/bin/scontrol', *arguments], check=True,
            capture_output=True, timeout=remaining,
            env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
        if len(result.stdout) > 4*1024*1024 or len(result.stderr) > 65536:
            raise ValueError('SLURM_QUERY_OUTPUT_SIZE')
        return result.stdout
    job = query(['--json', 'show', 'job', job_id])
    record = _document(job, 'jobs')
    def node_list(row):
        nodes = row.get('nodes')
        if (not isinstance(nodes, str) or len(nodes) > 4096
                or re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.,\[\]-]*', nodes) is None):
            raise ValueError('SLURM_QUERY_NODELIST')
        return nodes
    nodes = node_list(record)
    step = query(['--json', 'show', 'step', job_id+'.'+step_id])
    hosts = query(['show', 'hostnames', nodes])
    step_hosts = query(['show', 'hostnames', node_list(_document(step, 'steps'))])
    receipt = validate_task_allocation(job, step, hosts, job_id=job_id,
        submission_key=submission_key, rank=rank, node_count=node_count,
        partition=partition, gpu_type=gpu_type, environment=environment,
        hostname=hostname, uid=uid, step_hostnames=step_hosts)
    task_keys = ('SLURM_JOB_ID', 'SLURM_STEP_ID', 'SLURM_NODEID', 'SLURM_PROCID',
        'SLURM_LOCALID', 'SLURM_NTASKS', 'SLURM_JOB_NUM_NODES', 'SLURM_GPUS_ON_NODE',
        'SLURMD_NODENAME', 'SLURM_STEP_GPUS', 'CUDA_VISIBLE_DEVICES')
    return dict(receipt=receipt, job=job, step=step, hosts=hosts, stepHosts=step_hosts,
        task=dict(hostname=hostname, uid=uid, environment={k: environment[k] for k in task_keys}))
