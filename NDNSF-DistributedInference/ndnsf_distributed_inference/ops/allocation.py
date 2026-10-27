"""Host Slurm process management; native roles and object admission stay in SIF."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

from . import cluster
from .container import (SCRIPTS, gpu_selectors as _gpu_selectors, gpu_slots as _gpu_slots,
                        load_frozen, management_role_id, read_gpu_probe)
from .deployment_config import _read_json
from .processes import process_identity, write_json


_ROLE_STOP_GRACE_SECONDS = 10.0


def _roles(config):
    result = []
    for node in config['nodes']:
        for kind in ('nfd', 'nfdc'):
            if kind == 'nfd' and node['nfd']['mode'] != 'managed':
                continue
            result.append(dict(id=management_role_id(kind, node['id']), kind=kind, node=node['id']))
    result.extend(actor for actor in config['actors'] if not actor['external'])
    if len({role['id'] for role in result}) != len(result):
        raise cluster.ClusterError('ALLOCATION_MANAGEMENT_ROLE_ID_COLLISION')
    return result


def _audit_id(node_id):
    return 'host-audit.' + hashlib.sha256(node_id.encode()).hexdigest()[:32]


def _audit_tag(node_id, role_id=None):
    return _audit_id(node_id) + ('.' + hashlib.sha256(role_id.encode()).hexdigest()[:16]
                                if role_id else '')


def _receipt_path(args, role):
    root = args.deployment.parent
    if role['kind'] in ('nfd', 'nfdc'):
        return root / 'nodes' / role['node'] / role['kind'] / 'receipt.json'
    return root / 'actors' / role['id'] / 'receipt.json'


def _private_json(path):
    info = path.lstat()
    if (path.resolve() != path or not stat.S_ISREG(info.st_mode) or
            info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600):
        raise cluster.ClusterError('ALLOCATION_RECEIPT_NOT_PRIVATE:' + str(path))
    value = _read_json(path)
    if not isinstance(value, dict):
        raise cluster.ClusterError('ALLOCATION_RECEIPT_NOT_OBJECT:' + str(path))
    return value


def read_role(args, role, *, job_id=None):
    """Consume only private receipts bound to this allocation and frozen boot."""
    path = _receipt_path(args, role)
    try:
        value = _private_json(path)
    except FileNotFoundError:
        return None
    expected = dict(schema='ndnsf-di-node-role-v1', boot_id=args.boot_id,
                    configuration_sha256=args.configuration_sha256, release_sha256=args.release_sha256,
                    role_id=role['id'], role_kind=role['kind'], node_id=role['node'],
                    job_id=job_id if job_id is not None else os.environ['SLURM_JOB_ID'],
                    cleanup_scope='PID_NAMESPACE')
    if any(value.get(key) != item for key, item in expected.items()):
        raise cluster.ClusterError('ALLOCATION_ROLE_RECEIPT_MISMATCH:' + role['id'])
    if not isinstance(value.get('step_id'), str) or not re.fullmatch(r'[0-9]+', value['step_id']):
        raise cluster.ClusterError('ALLOCATION_ROLE_STEP_ID_MISSING:' + role['id'])
    return value


def _job_cgroup(value, job):
    if not isinstance(value, str) or not isinstance(job, str):
        return False
    return any(re.search(r'(?:^|/)job[_-]' + re.escape(job) + r'(?:/|\.|$)', line)
               for line in value.splitlines())


def _native_match(proof, receipt, role):
    native = next((row.get('proof') for row in receipt.get('launches', []) if row['id'] == role['id']), None)
    return bool(native and isinstance(proof, dict) and proof.get('nspid') and
                proof['nspid'][-1] == native['pid'] and proof['uid'] == os.getuid() and
                proof['start_ticks'] == native['start_ticks'] and proof['exe'] == native['exe'] and
                proof['bindings'] == native['bindings'] and
                proof['pid_namespace'] == receipt['pid_namespace'] and
                _job_cgroup(proof['cgroup'], receipt['job_id']) and re.search(
                    r'(?:^|/)step[_-]' + re.escape(receipt['step_id']) + r'(?:/|\.|$)', proof['cgroup']))


def audit_node(args, config):
    """Observe host processes without trusting namespace-local ZERO or signalling."""
    node = next(node for node in config['nodes'] if node['id'] == args.audit_node)
    if node['host'] not in (socket.gethostname(), socket.gethostname().split('.')[0]):
        raise cluster.ClusterError('ALLOCATION_AUDIT_HOST_MISMATCH')
    auditor_job = os.environ['SLURM_JOB_ID']
    target_job = getattr(args, 'audit_target_job', None)
    recovery_id = getattr(args, 'recovery_id', None)
    if target_job is not None and (not isinstance(target_job, str) or
            not re.fullmatch(r'[1-9][0-9]*', target_job) or
            target_job == auditor_job or args.observe_role or
            not isinstance(recovery_id, str) or not re.fullmatch('[0-9a-f]{32}', recovery_id)):
        raise cluster.ClusterError('ALLOCATION_RECOVERY_AUDIT_BINDING_INVALID')
    job = target_job or auditor_job
    result = dict(schema='ndnsf-di-host-node-audit-v1', node_id=node['id'],
                  boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                  release_sha256=args.release_sha256, job_id=job, host=socket.gethostname(),
                  observed_at=time.time(), status='UNKNOWN', cleanup_scope='HOST_UID_JOB',
                  processes=[], errors=[])
    if target_job is not None:
        result.update(auditor_job_id=auditor_job, recovery_id=recovery_id)
    deadline = time.monotonic() + args.timeout
    try:
        result['auditor_cgroup'] = Path('/proc/self/cgroup').read_text()
        if not _job_cgroup(result['auditor_cgroup'], auditor_job):
            raise cluster.ClusterError('ALLOCATION_AUDIT_JOB_CGROUP_MISMATCH')
        auditor_step = re.search(r'(?:^|/)step[_-]([0-9]+)(?:/|\.|$)', result['auditor_cgroup'])
        if not auditor_step:
            raise cluster.ClusterError('ALLOCATION_AUDIT_STEP_CGROUP_MISSING')
        result['auditor_step_id'] = auditor_step[1]
        roles = _roles(config)
        known = {}
        if not args.observe_role:
            for role in roles:
                if role['node'] != node['id']:
                    continue
                path = args.deployment.parent / 'nodes' / node['id'] / ('host-audit-' + role['id'] + '.json')
                if not path.exists():
                    continue
                prior = _private_json(path)
                if prior.get('status') != 'OBSERVED':
                    continue
                if any(prior.get(key) != result[key] for key in (
                        'boot_id', 'configuration_sha256', 'release_sha256', 'job_id', 'node_id')):
                    raise cluster.ClusterError('ALLOCATION_PRIOR_AUDIT_BINDING_MISMATCH')
                proof = prior['native_process']
                known[proof['pid']] = proof
        ignored = {_audit_id(item['id']) for item in config['nodes']} | {
            _audit_tag(role['node'], role['id']) for role in roles}
        if ignored & {role['id'] for role in roles}:
            raise cluster.ClusterError('ALLOCATION_AUDIT_ROLE_ID_COLLISION')
        for root in Path('/proc').iterdir():
            if time.monotonic() >= deadline:
                raise cluster.ClusterError('ALLOCATION_AUDIT_SCAN_TIMEOUT')
            if not root.name.isdigit():
                continue
            try:
                pid = int(root.name)
                uid = root.stat().st_uid
                known_pid = pid in known and int((root / 'stat').read_text().rsplit(')', 1)[1].split()[19]) == known[pid]['start_ticks']
                if uid != os.getuid():
                    if known_pid:
                        raise cluster.ClusterError('ALLOCATION_OBSERVED_PID_UID_CHANGED:' + root.name)
                    continue
                cgroup = (root / 'cgroup').read_text()
                in_job = _job_cgroup(cgroup, job)
                try:
                    proof = process_identity(pid)
                except PermissionError:
                    if not in_job and not known_pid:
                        continue  # Readable cgroup proves this is an unrelated protected process.
                    raise
                if proof is None or proof['state'] == 'Z':
                    continue
                bindings = proof['bindings']
                if (bindings.get('NDNSF_DEPLOYMENT_ACTOR_ID') in ignored and
                        (target_job is None or _job_cgroup(cgroup, auditor_job))):
                    continue
                bound = (bindings.get('NDNSF_DEPLOYMENT_BOOT_ID') == args.boot_id and
                         bindings.get('NDNSF_DEPLOYMENT_CONFIG_SHA256') == args.configuration_sha256)
                numeric_step = re.search(r'(?:^|/)step[_-]([0-9]+)(?:/|\.|$)', cgroup)
                job_task = (in_job and (job != auditor_job or
                            (numeric_step and numeric_step[1] != result['auditor_step_id'])))
                if not bound and not known_pid and not job_task:
                    continue
                status = (root / 'status').read_text()
                nspid = next(line.split()[1:] for line in status.splitlines() if line.startswith('NSpid:'))
                proof.update(nspid=[int(value) for value in nspid],
                             pid_namespace=os.readlink(root / 'ns/pid'),
                             cgroup=cgroup)
                if not _job_cgroup(proof['cgroup'], job):
                    raise cluster.ClusterError('ALLOCATION_BOUND_PROCESS_WRONG_CGROUP:' + root.name)
                result['processes'].append(proof)
            except (FileNotFoundError, ProcessLookupError):
                if root.exists():
                    result['errors'].append('UNREADABLE:' + root.name)
            except (OSError, ValueError, StopIteration, RuntimeError) as error:
                result['errors'].append(root.name + ':' + str(error))
            if len(result['errors']) >= 64 or len(result['processes']) >= 4096:
                raise cluster.ClusterError('ALLOCATION_AUDIT_CAPACITY_EXCEEDED')
        if result['errors']:
            raise cluster.ClusterError('ALLOCATION_AUDIT_UNREADABLE')
        if args.observe_role:
            role = next(role for role in _roles(config) if role['id'] == args.observe_role)
            if role['node'] != node['id']:
                raise cluster.ClusterError('ALLOCATION_AUDIT_ROLE_NODE_MISMATCH')
            receipt = read_role(args, role)
            matches = [proof for proof in result['processes'] if receipt and _native_match(proof, receipt, role)]
            if len(matches) != 1:
                raise cluster.ClusterError('ALLOCATION_NATIVE_HOST_PID_NOT_OBSERVED:' + role['id'])
            result.update(status='OBSERVED', role_id=role['id'], native_process=matches[0])
        else:
            result['status'] = 'SURVIVORS' if result['processes'] else 'ZERO'
    except (OSError, ValueError, RuntimeError, KeyError, StopIteration) as error:
        result.update(status='UNKNOWN', failure=str(error) or type(error).__name__)
    suffix = 'recovery' if target_job is not None else args.observe_role or 'final'
    target = args.deployment.parent / 'nodes' / node['id'] / ('host-audit-' + suffix + '.json')
    cluster._private_directory(target.parent)
    write_json(target, result)
    return result




def step_arguments(config, role, gpu_allocation=None):
    node = next(node for node in config['nodes'] if node['id'] == role['node'])
    resources = role.get('resources', {'cpus': 1, 'gpu_devices': []})
    command = ['srun', '--overlap', '--exact', '--nodes=1', '--ntasks=1',
               '--nodelist=' + node['host'], '--cpus-per-task=' + str(resources['cpus'])]
    slots = _gpu_slots(role, gpu_allocation)
    if slots:
        # The container scopes declared UUIDs; Slurm/NVML mask numbering may differ.
        command += ['--gpus-per-node=' + str(len(gpu_allocation['devices'])), '--gpu-bind=none']
    else:
        command.append('--gres=none')
    return command




def _executable(command):
    executable = shutil.which(command[0])
    if not executable:
        raise cluster.ClusterError('ALLOCATION_EXECUTABLE_MISSING:' + command[0])
    return [executable, *command[1:]]


def _bound_arguments(args, timeout):
    command = ['--deployment', str(args.deployment), '--boot-id', args.boot_id,
               '--configuration-sha256', args.configuration_sha256,
               '--release-sha256', args.release_sha256, '--timeout', str(timeout)]
    if args.debug:
        command.append('--debug')
    return command


def _stop_role_owners(config, args, state, children):
    """Drain entered owners in reverse startup order before terminating srun."""
    deadline = time.monotonic() + _ROLE_STOP_GRACE_SECONDS
    errors = []
    requests = state.setdefault('role_stop_requests', [])
    for role in reversed(state['roles']):
        child = children.get(role['id'])
        if child is None or child.poll() is not None:
            continue
        try:
            receipt = read_role(args, role)
            if receipt is None or receipt['status'] == 'STOPPED':
                continue
            root = _receipt_path(args, role).parent / 'runtime' / config['deployment_id']
            if root.resolve() != root or not root.is_dir():
                raise cluster.ClusterError('ALLOCATION_ROLE_STOP_ROOT_INVALID:' + role['id'])
            cluster._private_directory(root)
            row = dict(role_id=role['id'], status='REQUESTED')
            requests.append(row)
            write_json(root / 'cancel.json', {'boot_id': args.boot_id})
            # This cleanup deadline is independent of the expired execution
            # deadline and does not stop dependencies while a child drains.
            while child.poll() is None and time.monotonic() < deadline:
                time.sleep(min(0.05, max(0.001, deadline - time.monotonic())))
            if child.poll() is None:
                row['status'] = 'GRACE_EXPIRED'
                errors.append('ALLOCATION_ROLE_STOP_GRACE_EXPIRED:' + role['id'])
                break  # Original reverse-order TERM/KILL remains the fallback.
            row['status'] = 'EXITED'
        except (OSError, ValueError, RuntimeError, KeyError) as error:
            errors.append(str(error) or type(error).__name__)
            break  # Do not stop a dependency before an unverified owner drains.
    return errors


def run_allocation(args, config):
    """Own ordered native role steps, preserve failures, and require host audits."""
    roles = _roles(config)
    if not any(role['kind'] == 'requester' for role in roles):
        raise cluster.ClusterError('ALLOCATION_OWNED_FINITE_REQUESTER_REQUIRED')
    if len(roles) > 256:
        raise cluster.ClusterError('ALLOCATION_ROLE_CAPACITY_EXCEEDED')
    for role in roles:
        _gpu_selectors(role)
    root = args.deployment.parent / ('routes' if args.routes_only else 'supervisor') / config['deployment_id']
    cluster._private_directory(root)
    with cluster._lock(root):
        if cluster._read_state(root) is not None:
            raise cluster.ClusterError('ALLOCATION_BOOT_ALREADY_ENTERED')
        state = dict(schema='ndnsf-di-host-operation-v1', deployment_id=config['deployment_id'],
                     boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                     release_sha256=args.release_sha256, evidence_root=str(root), status='STARTING',
                     job_id=os.environ['SLURM_JOB_ID'], launches=[], roles=[], host_audits=[],
                     failure=None, execution='STARTING', oracle='NOT_RUN', started_at=time.time())
        deadline = time.monotonic() + args.timeout
        started = time.monotonic()
        stopping = False
        services = []
        role_children = {}
        gpu_allocations = {}
        state['external_references'] = [dict(id=actor['id'], kind=actor['kind'], verification='NOT_PROBED')
                                        for actor in config['actors'] if actor['external']]

        def terminate(signum, frame):
            nonlocal stopping
            stopping = True

        def guard():
            if stopping:
                raise KeyboardInterrupt('ALLOCATION_SIGTERM')
            if time.monotonic() >= deadline:
                raise cluster.ClusterError('ALLOCATION_TIMEOUT')
            for child, role in services:
                if child.poll() is not None:
                    raise cluster.ClusterError('ALLOCATION_SERVICE_EXIT:' + role['id'])
                value = read_role(args, role)
                if not value or value['failure'] or value['execution'] != 'RUNNING':
                    raise cluster.ClusterError('ALLOCATION_SERVICE_NOT_RUNNING:' + role['id'])

        def remaining():
            guard()
            return max(0.001, deadline - time.monotonic())

        def launch(tag, kind, command):
            env = {key: value for key, value in os.environ.items() if key != 'PYTHONPATH'}
            env.update(NDNSF_DEPLOYMENT_BOOT_ID=args.boot_id,
                       NDNSF_DEPLOYMENT_CONFIG_SHA256=args.configuration_sha256,
                       NDNSF_DEPLOYMENT_ACTOR_ID=tag)
            return cluster._spawn(state, root, tag, kind, _executable(command), env, guard)[0]

        def wait(child, tag):
            while child.poll() is None:
                guard()
                time.sleep(0.05)
            guard()
            if child.returncode:
                raise cluster.ClusterError('ALLOCATION_STEP_EXIT:' + tag + ':' + str(child.returncode))

        def role_step(role):
            command = step_arguments(config, role, gpu_allocations.get(role['node']))
            command += ['/bin/sh', str(SCRIPTS / 'run-container.sh'), '--control-python', sys.executable]
            command += _bound_arguments(args, remaining())
            command += (['--management', role['kind'], '--node', role['node']]
                        if role['kind'] in ('nfd', 'nfdc') else ['--actor', role['id']])
            state['roles'].append(dict(role))
            child = launch(management_role_id('step', role['id']), role['kind'], command)
            role_children[role['id']] = child
            return child

        def audit(node_id, role_id=None, *, closeout=False):
            role = dict(id=_audit_id(node_id), kind='audit', node=node_id)
            command = step_arguments(config, role) + [sys.executable, '-I', '-m',
                       'ndnsf_distributed_inference.ops.allocation']
            command += _bound_arguments(args, 30 if closeout else remaining())
            command += ['--audit-node', node_id]
            if role_id:
                command += ['--observe-role', role_id]
            tag = _audit_tag(node_id, role_id)
            # Ignore only the explicit host-audit identity during its own scan.
            env = dict(os.environ, NDNSF_DEPLOYMENT_BOOT_ID=args.boot_id,
                       NDNSF_DEPLOYMENT_CONFIG_SHA256=args.configuration_sha256,
                       NDNSF_DEPLOYMENT_ACTOR_ID=role['id'])
            if closeout:
                audit_root = root / 'final-audits' / node_id / config['deployment_id']
                cluster._private_directory(audit_root)
                audit_state = dict(schema='ndnsf-di-host-operation-v1',
                    deployment_id=config['deployment_id'], boot_id=args.boot_id,
                    configuration_sha256=args.configuration_sha256,
                    evidence_root=str(audit_root), status='STARTING', launches=[])
                audit_deadline = time.monotonic() + 30

                def audit_guard():
                    if time.monotonic() >= audit_deadline:
                        raise cluster.ClusterError('ALLOCATION_FINAL_AUDIT_TIMEOUT:' + node_id)

                try:
                    child, _ = cluster._spawn(audit_state, audit_root, role['id'], 'audit',
                                              _executable(command), env, audit_guard)
                    while child.poll() is None:
                        audit_guard()
                        time.sleep(0.05)
                    if child.returncode:
                        raise cluster.ClusterError('ALLOCATION_FINAL_AUDIT_EXIT:' + node_id)
                finally:
                    cluster._close(audit_state, audit_root)
                if audit_state['status'] != 'STOPPED' or any(
                        row.get('forced') for row in audit_state['cleanup']):
                    raise cluster.ClusterError('ALLOCATION_FINAL_AUDIT_CLEANUP_UNKNOWN:' + node_id)
            else:
                child = launch(tag, 'audit', command)
                wait(child, tag)
            target = args.deployment.parent / 'nodes' / node_id / ('host-audit-' + (role_id or 'final') + '.json')
            value = _private_json(target)
            expected = dict(schema='ndnsf-di-host-node-audit-v1',
                            boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                            release_sha256=args.release_sha256, job_id=state['job_id'], node_id=node_id,
                            status='OBSERVED' if role_id else 'ZERO')
            if any(value.get(key) != item for key, item in expected.items()):
                raise cluster.ClusterError('ALLOCATION_HOST_AUDIT_FAILED:' + node_id)
            node = next(node for node in config['nodes'] if node['id'] == node_id)
            host = value.get('host', '')
            if node['host'] not in (host, host.split('.')[0]):
                raise cluster.ClusterError('ALLOCATION_HOST_AUDIT_NODE_MISMATCH:' + node_id)
            if role_id:
                observed = next(role for role in roles if role['id'] == role_id)
                if (value.get('role_id') != role_id or not _native_match(
                        value.get('native_process'), read_role(args, observed) or {}, observed)):
                    raise cluster.ClusterError('ALLOCATION_HOST_AUDIT_PROOF_MISSING:' + role_id)
            elif value.get('processes') != [] or value.get('errors') != []:
                raise cluster.ClusterError('ALLOCATION_HOST_AUDIT_ZERO_NOT_PROVEN:' + node_id)
            state['host_audits'].append(str(target))

        def await_ready(child, role):
            wanted = 'SOCKET_BOUND' if role['kind'] == 'nfd' else 'NATIVE_READY'
            while True:
                guard()
                if child.poll() is not None:
                    raise cluster.ClusterError('ALLOCATION_ROLE_EXIT_BEFORE_READY:' + role['id'])
                value = read_role(args, role)
                if (value and value['readiness'] == wanted and not value['failure'] and
                        value['execution'] == 'RUNNING' and value['status'] == 'SERVICES_READY'):
                    services.append((child, role))
                    audit(role['node'], role['id'])
                    return
                time.sleep(0.05)

        handler = signal.signal(signal.SIGTERM, terminate)
        write_json(root / 'state.json', state)
        try:
            child = launch('allocation-hosts', 'scheduler',
                           ['scontrol', 'show', 'hostnames', os.environ['SLURM_JOB_NODELIST']])
            wait(child, 'allocation-hosts')
            with Path(state['launches'][-1]['log']).open('rb') as stream:
                raw = stream.read(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024:
                raise cluster.ClusterError('ALLOCATION_NODELIST_TOO_LARGE')
            hosts = raw.decode('utf-8').splitlines()
            if set(hosts) != {node['host'] for node in config['nodes']} or len(hosts) != len(config['nodes']):
                raise cluster.ClusterError('ALLOCATION_NODE_MEMBERSHIP_MISMATCH')
            if not args.routes_only:
                for node in config['nodes']:
                    if not any(role['node'] == node['id'] and _gpu_selectors(role) for role in roles):
                        continue
                    probe = dict(id=management_role_id('gpu-probe', node['id']),
                                 kind='gpu-probe', node=node['id'])
                    command = [item for item in step_arguments(config, probe) if item != '--gres=none']
                    command += ['--gpus-per-node=' + str(config['platform']['slurm']['gpu']['count']),
                                '--gpu-bind=none', '/bin/sh', str(SCRIPTS / 'run-container.sh'),
                                '--control-python', sys.executable]
                    command += _bound_arguments(args, remaining()) + ['--management', 'gpu-probe', '--node', node['id']]
                    child = launch(probe['id'], 'gpu-probe', command)
                    wait(child, probe['id'])
                    path = args.deployment.parent / 'nodes' / node['id'] / 'gpu-allocation.json'
                    with Path(state['launches'][-1]['log']).open('rb') as stream:
                        output = stream.read(1024 * 1024 + 1)
                    if len(output) > 1024 * 1024:
                        raise cluster.ClusterError('ALLOCATION_GPU_QUERY_TOO_LARGE')
                    value = read_gpu_probe(output.decode('utf-8'), args, node, probe['id'],
                                           config['platform']['slurm']['gpu']['count'])
                    write_json(path, value)
                    gpu_allocations[node['id']] = value
                for role in roles:
                    step_arguments(config, role, gpu_allocations.get(role['node']))
                owners = {}
                for role in roles:
                    for slot in _gpu_slots(role, gpu_allocations.get(role['node'])):
                        owners.setdefault((role['node'], slot), []).append(role)
                if any(len(group) > 1 and not all(role['resources']['allow_sharing'] for role in group)
                       for group in owners.values()):
                    raise cluster.ClusterError('ALLOCATION_PHYSICAL_GPU_SHARING_NOT_ALLOWED')
                state['gpu_allocations'] = gpu_allocations
            if args.routes_only:
                for role in roles:
                    if role['kind'] == 'nfdc':
                        child = role_step(role)
                        wait(child, role['id'])
                        value = read_role(args, role)
                        if not value or value['execution'] != 'COMPLETE' or value['status'] != 'STOPPED':
                            raise cluster.ClusterError('ALLOCATION_NFDC_NOT_VERIFIED:' + role['id'])
            else:
                for role in roles:
                    if role['kind'] == 'nfd':
                        await_ready(role_step(role), role)
                # The maintained routes script dispatches to this same owner.
                # Start its stable Python process directly for durable PID proof.
                command = [sys.executable, '-I', '-m',
                           'ndnsf_distributed_inference.ops.allocation', '--routes-only']
                command += _bound_arguments(args, remaining())
                child = launch('route-config', 'routes', command)
                wait(child, 'route-config')
                for kind in ('controller', 'artifact-authority', 'repo', 'provider'):
                    for role in roles:
                        if role['kind'] == kind:
                            await_ready(role_step(role), role)
                state['readiness'] = ('OWNED_SERVICES_READY' if state['external_references'] else 'SERVICES_READY')
                role = next(role for role in roles if role['kind'] == 'requester')
                child = role_step(role)
                while child.poll() is None:
                    guard()
                    value = read_role(args, role)
                    if value and value['readiness'] == 'FINITE_RUNNING':
                        audit(role['node'], role['id'])
                        break
                    time.sleep(0.05)
                wait(child, role['id'])
                value = read_role(args, role)
                if not value or value['execution'] != 'COMPLETE' or value['status'] != 'STOPPED':
                    raise cluster.ClusterError('ALLOCATION_REQUESTER_NOT_COMPLETE')
                if not any('host-audit-' + role['id'] + '.json' in item for item in state['host_audits']):
                    raise cluster.ClusterError('ALLOCATION_REQUESTER_HOST_PID_UNOBSERVED')
            state['execution'] = 'COMPLETE'
        except (Exception, KeyboardInterrupt) as error:
            state.update(execution='INTERRUPTED' if stopping else 'FAILED',
                         failure=str(error) or type(error).__name__)
        finally:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                stop_errors = (_stop_role_owners(config, args, state, role_children)
                               if not args.routes_only else [])
                cluster._close(state, root)
                if stop_errors:
                    state.update(execution='FAILED', failure=state['failure'] or stop_errors[0],
                                 role_stop_errors=stop_errors)
                if any(row.get('forced') for row in state['cleanup']):
                    state.update(execution='FAILED', failure=state['failure'] or 'ALLOCATION_FORCED_CLEANUP')
                if not args.routes_only:
                    route_state = args.deployment.parent / 'routes.json'
                    if not route_state.exists() and any(
                            row['id'] == 'route-config' for row in state['launches']):
                        state.update(status='UNKNOWN', failure=state['failure'] or
                                     'ALLOCATION_ROUTES_CLEANUP_UNKNOWN')
                    if route_state.exists():
                        try:
                            routes = _private_json(route_state)
                            if (routes['boot_id'] != args.boot_id or
                                    routes['configuration_sha256'] != args.configuration_sha256):
                                raise cluster.ClusterError('ALLOCATION_ROUTES_RECEIPT_MISMATCH')
                            state['roles'].extend(routes['roles'])
                        except (OSError, ValueError, RuntimeError, KeyError) as error:
                            state.update(status='UNKNOWN', failure=state['failure'] or str(error))
                    for role in state['roles']:
                        try:
                            value = read_role(args, role)
                            if value is None and state['execution'] in ('FAILED', 'INTERRUPTED'):
                                attempt = next((row for row in state['launches'] if row['id'] ==
                                                management_role_id('step', role['id'])), {})
                                if attempt.get('proof') or attempt.get('spawn_failed') is True:
                                    # Exact group cleanup above and final host audits below
                                    # prove closeout even if the container entry never ran.
                                    continue
                            if not value or value['status'] != 'STOPPED' or any(
                                    row['status'] != 'ZERO' for row in value['cleanup']):
                                raise cluster.ClusterError('ALLOCATION_ROLE_CLEANUP_UNKNOWN:' + role['id'])
                        except (OSError, ValueError, RuntimeError, KeyError) as error:
                            state.update(status='UNKNOWN', failure=state['failure'] or str(error))
                    for node in config['nodes']:
                        try:
                            audit(node['id'], closeout=True)
                        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                            state.update(status='UNKNOWN', failure=state['failure'] or str(error))
                state['wall_seconds'] = time.monotonic() - started
                write_json(root / 'state.json', state)
                target = args.deployment.parent / ('routes.json' if args.routes_only else 'allocation.json')
                write_json(target, state)
            finally:
                signal.signal(signal.SIGTERM, handler)
        return state


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deployment', type=Path, required=True)
    parser.add_argument('--boot-id', required=True)
    parser.add_argument('--configuration-sha256', required=True)
    parser.add_argument('--release-sha256', required=True)
    parser.add_argument('--timeout', type=float, required=True)
    parser.add_argument('--evidence', type=Path)
    purpose = parser.add_mutually_exclusive_group()
    purpose.add_argument('--routes-only', action='store_true')
    purpose.add_argument('--audit-node')
    parser.add_argument('--observe-role')
    parser.add_argument('--audit-target-job')
    parser.add_argument('--recovery-id')
    parser.add_argument('--debug', action='store_true')
    args = parser.parse_args(argv)
    if (not math.isfinite(args.timeout) or not 0 < args.timeout <= 3600 or
            not os.environ.get('SLURM_JOB_ID') or not os.environ.get('SLURM_JOB_NODELIST') or
            (args.evidence and args.evidence != args.deployment.parent) or
            (args.observe_role and not args.audit_node) or
            (args.audit_target_job is not None and not args.audit_node) or
            ((args.audit_target_job is None) != (args.recovery_id is None))):
        parser.error('allocation, finite bounded timeout and bound evidence path are required')
    try:
        config = load_frozen(args.deployment, args.configuration_sha256, args.boot_id, args.release_sha256)
        result = audit_node(args, config) if args.audit_node else run_allocation(args, config)
    except (OSError, ValueError, RuntimeError, KeyError, StopIteration) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(result, sort_keys=True))
    return 0 if result['status'] in ('ZERO', 'OBSERVED') or (
        result['status'] == 'STOPPED' and result['execution'] == 'COMPLETE' and not result['failure']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
