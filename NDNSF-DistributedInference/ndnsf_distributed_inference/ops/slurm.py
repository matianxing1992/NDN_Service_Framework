"""Owned scheduler control; native execution and node audits belong to the allocation."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sys
import time
import uuid
from types import SimpleNamespace

from . import cluster
from ._container.lib.adapters.slurm_apptainer import SlurmApptainerAdapter, SlurmAdapterError
from .processes import write_json


SCHEMA = 'ndnsf-di-slurm-operation-v1'


def _closed_groups(value, boot, digest):
    launches = value['launches']
    ids = [row['id'] for row in launches]
    if len(ids) != len(set(ids)):
        raise cluster.ClusterError('SLURM_RESULT_DUPLICATE_PROCESS')
    for row in launches:
        expected = {'NDNSF_DEPLOYMENT_ACTOR_ID': row['id'],
                    'NDNSF_DEPLOYMENT_BOOT_ID': boot,
                    'NDNSF_DEPLOYMENT_CONFIG_SHA256': digest}
        not_spawned = row.get('spawn_failed') is True and not row.get('proof')
        if row['bindings'] != expected or (not not_spawned and row['proof']['bindings'] != expected):
            raise cluster.ClusterError('SLURM_RESULT_PROCESS_BINDING')
        if not not_spawned:
            proof = row['proof']
            if (any(type(proof.get(key)) is not int for key in ('pid', 'start_ticks', 'uid', 'pgid')) or
                    proof['pid'] <= 1 or proof['start_ticks'] <= 0 or
                    proof['uid'] != os.getuid() or proof['pgid'] != proof['pid']):
                raise cluster.ClusterError('SLURM_RESULT_PROCESS_IDENTITY')
    cleanup = value['cleanup']
    not_spawned_ids = {row['id'] for row in launches
                       if row.get('spawn_failed') is True and not row.get('proof')}
    if (len(cleanup) != len(ids) or {row['id'] for row in cleanup} != set(ids) or
            any(row.get('status') != 'ZERO' or
                row.get('forced', False if row['id'] in not_spawned_ids else None) is not False
                for row in cleanup)):
        raise cluster.ClusterError('SLURM_RESULT_PROCESS_CLEANUP')


def _result_configuration(state, reference):
    from .allocation import _private_json

    root = Path(state['evidence_root'])
    config = _private_json(root / 'configuration.json')
    digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    boot = state['boot_id']
    if (digest != state['configuration_sha256'] or digest != reference['configuration_sha256'] or
            boot != reference['boot_id'] or config['deployment_id'] != state['deployment_id'] or
            config['platform']['kind'] != 'slurm-apptainer'):
        raise cluster.ClusterError('SLURM_RESULT_CONFIGURATION_BINDING')
    return root, config, digest, boot


def _allocation_result(state, result):
    """Consume original closeout receipts; no image/model access or native result inference."""
    from .allocation import _job_cgroup, _native_match, _private_json, _roles, read_role

    scheduler = result['scheduler']
    if not scheduler['terminal'] or scheduler['status'] != 'TERMINAL':
        return
    reference = scheduler['reference']
    root, config, digest, boot = _result_configuration(state, reference)
    value = _private_json(root / 'allocation.json')
    expected = dict(schema='ndnsf-di-host-operation-v1', deployment_id=state['deployment_id'],
                    boot_id=boot, configuration_sha256=digest,
                    release_sha256=reference['release_sha256'], job_id=reference['job_id'],
                    evidence_root=str(root / 'supervisor' / state['deployment_id']), status='STOPPED')
    if any(value.get(key) != item for key, item in expected.items()):
        raise cluster.ClusterError('SLURM_RESULT_ALLOCATION_BINDING')
    external = [dict(id=actor['id'], kind=actor['kind'], verification='NOT_PROBED')
                for actor in config['actors'] if actor['external']]
    if value['external_references'] != external:
        raise cluster.ClusterError('SLURM_RESULT_EXTERNAL_REFERENCE_BINDING')
    result.update(execution=value['execution'], failure=value['failure'] or state.get('failure'),
                  oracle='NOT_RUN', readiness=value.get('readiness', 'UNVERIFIED'))
    _closed_groups(value, boot, digest)
    if value['execution'] not in ('COMPLETE', 'FAILED', 'INTERRUPTED'):
        raise cluster.ClusterError('SLURM_RESULT_EXECUTION_UNKNOWN')
    finished = value['execution'] == 'COMPLETE'
    roles = _roles(config)
    expected_roles = {role['id']: role for role in roles}
    observed = value['roles']
    actual_ids = [role['id'] for role in observed]
    if (len(actual_ids) != len(set(actual_ids)) or
            not set(actual_ids).issubset(expected_roles) or
            (finished and set(actual_ids) != set(expected_roles)) or any(
                role['kind'] != expected_roles[role['id']]['kind'] or
                role['node'] != expected_roles[role['id']]['node'] for role in observed)):
        raise cluster.ClusterError('SLURM_RESULT_ROLE_COVERAGE')
    args = SimpleNamespace(deployment=root / 'configuration.json', boot_id=boot,
                           configuration_sha256=digest, release_sha256=reference['release_sha256'])
    requesters = []
    for role in value['roles']:
        receipt = read_role(args, role, job_id=reference['job_id'])
        if receipt is None and not finished:
            from .container import management_role_id
            attempt = next((row for row in value['launches']
                            if row['id'] == management_role_id('step', role['id'])), {})
            if attempt.get('proof') or attempt.get('spawn_failed') is True:
                continue
        if receipt is None or receipt['status'] != 'STOPPED':
            raise cluster.ClusterError('SLURM_RESULT_ROLE_NOT_CLOSED:' + role['id'])
        _closed_groups(receipt, boot, digest)
        if receipt['exit_code'] is not None and type(receipt['exit_code']) is not int:
            raise cluster.ClusterError('SLURM_RESULT_EXIT_CODE:' + role['id'])
        if finished and not receipt['launches']:
            raise cluster.ClusterError('SLURM_RESULT_ROLE_NOT_LAUNCHED:' + role['id'])
        expected_execution = 'COMPLETE' if role['kind'] in ('requester', 'nfdc') else 'SERVICE_STOPPED'
        if value['execution'] == 'COMPLETE' and (
                receipt['failure'] or receipt['execution'] != expected_execution):
            raise cluster.ClusterError('SLURM_RESULT_ROLE_EXECUTION:' + role['id'])
        observation_path = root / 'nodes' / role['node'] / ('host-audit-' + role['id'] + '.json')
        if role['kind'] != 'nfdc' and (finished or observation_path.exists()):
            observed = _private_json(observation_path)
            bound = dict(schema='ndnsf-di-host-node-audit-v1', boot_id=boot,
                         configuration_sha256=digest, release_sha256=reference['release_sha256'],
                         job_id=reference['job_id'], node_id=role['node'], role_id=role['id'],
                         status='OBSERVED')
            proof = observed.get('native_process', {})
            host = observed.get('host')
            node_host = next(node['host'] for node in config['nodes'] if node['id'] == role['node'])
            early_exit = (not finished and receipt['execution'] == 'FAILED' and
                          type(receipt['exit_code']) is int and receipt['exit_code'] != 0 and
                          observed.get('status') == 'UNKNOWN' and
                          observed.get('role_id', role['id']) == role['id'] and
                          observed.get('failure') ==
                          'ALLOCATION_NATIVE_HOST_PID_NOT_OBSERVED:' + role['id'] and
                          observed.get('errors') == [] and
                          observed.get('cleanup_scope') == 'HOST_UID_JOB' and
                          isinstance(host, str) and node_host in (host, host.split('.')[0]) and
                          isinstance(observed.get('auditor_cgroup'), str) and
                          _job_cgroup(observed['auditor_cgroup'], reference['job_id']))
            if early_exit:
                # This admits failed cleanup only, never a native execution proof.
                bound.pop('role_id')
                bound['status'] = 'UNKNOWN'
            if (any(observed.get(key) != item for key, item in bound.items()) or
                    (not early_exit and (not _native_match(proof, receipt, role) or
                     not _job_cgroup(proof.get('cgroup', ''), reference['job_id'])))):
                raise cluster.ClusterError('SLURM_RESULT_NATIVE_HOST_PROOF:' + role['id'])
        if role['kind'] == 'requester':
            requesters.append(receipt)
    if len(requesters) > 1 or (finished and len(requesters) != 1):
        raise cluster.ClusterError('SLURM_RESULT_REQUESTER_COVERAGE')
    for node in config['nodes']:
        audit = _private_json(root / 'nodes' / node['id'] / 'host-audit-final.json')
        binding = dict(schema='ndnsf-di-host-node-audit-v1', boot_id=boot,
                       configuration_sha256=digest, release_sha256=reference['release_sha256'],
                       job_id=reference['job_id'], node_id=node['id'], status='ZERO',
                       cleanup_scope='HOST_UID_JOB', processes=[], errors=[])
        host = audit.get('host', '')
        if (any(audit.get(key) != item for key, item in binding.items()) or
                node['host'] not in (host, host.split('.')[0])):
            raise cluster.ClusterError('SLURM_RESULT_NODE_CLEANUP:' + node['id'])
    result.update(actor_cleanup='ZERO', processes=[], requester=dict(
        completion=requesters[0]['execution'] if requesters else 'NOT_STARTED',
        exit_code=requesters[0]['exit_code'] if requesters else None))
    complete = (value['execution'] == 'COMPLETE' and not result['failure'] and
                scheduler['state'] == 'COMPLETED' and scheduler['exit_code'] == '0:0' and
                requesters[0]['execution'] == 'COMPLETE' and requesters[0]['exit_code'] == 0)
    result['status'] = ('COMPLETE' if complete and not value['external_references'] else
                        'EXTERNAL_UNVERIFIED' if complete else
                        'INTERRUPTED' if value['execution'] == 'INTERRUPTED' else 'FAILED')


def _recovery_result(state, result):
    """Admit independent host cleanup without inventing a lost execution result."""
    from .allocation import _audit_id, _job_cgroup, _private_json

    scheduler = result['scheduler']
    if (scheduler.get('terminal') is not True or scheduler.get('status') != 'TERMINAL' or
            scheduler.get('live_verified') is not False or scheduler.get('never_allocated') is True):
        raise cluster.ClusterError('SLURM_RECOVERY_JOB_NOT_TERMINAL')
    reference = scheduler['reference']
    root, config, digest, boot = _result_configuration(state, reference)
    allocation = root / 'allocation.json'
    nonce = state.get('recovery_id')
    if (allocation.exists() or allocation.is_symlink() or not isinstance(nonce, str) or
            not re.fullmatch('[0-9a-f]{32}', nonce)):
        raise cluster.ClusterError('SLURM_RECOVERY_NOT_ADMISSIBLE')
    for node in config['nodes']:
        owner_root = root / 'recovery' / nonce / node['id'] / config['deployment_id']
        owner = _private_json(owner_root / 'state.json')
        expected = dict(schema='ndnsf-di-host-operation-v1', deployment_id=config['deployment_id'],
            boot_id=boot, configuration_sha256=digest, evidence_root=str(owner_root),
            recovery_id=nonce, status='STOPPED')
        if (any(owner.get(key) != value for key, value in expected.items()) or
                type(owner.get('audit_exit_code')) is not int or owner['audit_exit_code'] != 0 or
                len(owner.get('launches', [])) != 1 or
                owner['launches'][0]['id'] != _audit_id(node['id'])):
            raise cluster.ClusterError('SLURM_RECOVERY_OWNER_BINDING')
        _closed_groups(owner, boot, digest)
        audit = _private_json(root / 'nodes' / node['id'] / 'host-audit-recovery.json')
        binding = dict(schema='ndnsf-di-host-node-audit-v1', boot_id=boot,
            configuration_sha256=digest, release_sha256=reference['release_sha256'],
            job_id=reference['job_id'], node_id=node['id'], recovery_id=nonce,
            status='ZERO', cleanup_scope='HOST_UID_JOB', processes=[], errors=[])
        host, auditor = audit.get('host'), audit.get('auditor_job_id')
        cgroup = audit.get('auditor_cgroup', '')
        step = (re.search(r'(?:^|/)step[_-]([0-9]+)(?:/|\.|$)', cgroup)
                if isinstance(cgroup, str) else None)
        if (any(audit.get(key) != value for key, value in binding.items()) or
                not isinstance(host, str) or node['host'] not in (host, host.split('.')[0]) or
                not isinstance(auditor, str) or not re.fullmatch('[1-9][0-9]*', auditor) or
                auditor == reference['job_id'] or
                not _job_cgroup(cgroup, auditor) or not step or
                audit.get('auditor_step_id') != step[1]):
            raise cluster.ClusterError('SLURM_RECOVERY_NODE_BINDING:' + node['id'])
    result.update(status='FAILED', execution='FAILED', actor_cleanup='ZERO', processes=[],
                  cleanup_scope='RECOVERED_HOST_UID_JOB',
                  failure=state.get('failure') or 'ALLOCATION_RESULT_MISSING')


def _recover_missing_closeout(state, operation_root, deadline):
    """Run bounded read-only host audits; never launch application roles here."""
    from .allocation import _audit_id, _bound_arguments

    scheduler = state['scheduler']
    if (scheduler.get('terminal') is not True or scheduler.get('status') != 'TERMINAL' or
            scheduler.get('live_verified') is not False or scheduler.get('never_allocated') is True):
        raise cluster.ClusterError('SLURM_RECOVERY_JOB_NOT_TERMINAL')
    reference = scheduler['reference']
    root, config, digest, boot = _result_configuration(state, reference)
    allocation = root / 'allocation.json'
    if allocation.exists() or allocation.is_symlink():
        raise cluster.ClusterError('SLURM_RECOVERY_ALLOCATION_PRESENT')
    srun = shutil.which('srun')
    if not srun:
        raise cluster.ClusterError('SLURM_RECOVERY_SRUN_UNAVAILABLE')
    nonce = uuid.uuid4().hex
    state['recovery_id'] = nonce
    _save(operation_root, state)
    for node in config['nodes']:
        owner_root = root / 'recovery' / nonce / node['id'] / config['deployment_id']
        cluster._private_directory(owner_root)
        owner = dict(schema='ndnsf-di-host-operation-v1', deployment_id=config['deployment_id'],
            boot_id=boot, configuration_sha256=digest, evidence_root=str(owner_root),
            recovery_id=nonce, status='STARTING', launches=[], audit_exit_code=None)
        slurm = config['platform']['slurm']
        command = [srun, '--job-name=ndnsf-audit-' + nonce[:12],
            '--partition=' + slurm['partition'], '--nodes=1', '--ntasks=1',
            '--cpus-per-task=1', '--mem=1G', '--gres=none', '--nodelist=' + node['host'],
            '--immediate=15', '--time=00:02:00', '--chdir=/tmp', '--export=NONE']
        for option in ('account', 'qos'):
            if slurm.get(option):
                command.append('--' + option + '=' + slurm[option])
        args = SimpleNamespace(deployment=root / 'configuration.json', boot_id=boot,
            configuration_sha256=digest, release_sha256=reference['release_sha256'], debug=False)
        command += [sys.executable, '-I', '-m', 'ndnsf_distributed_inference.ops.allocation']
        command += _bound_arguments(args, min(30, _remaining(deadline)))
        command += ['--audit-node', node['id'], '--audit-target-job', reference['job_id'],
                    '--recovery-id', nonce]
        env = {key: value for key, value in os.environ.items()
               if key in ('PATH', 'HOME', 'LANG', 'LC_ALL', 'TZ')}
        tag = _audit_id(node['id'])
        env.update(NDNSF_DEPLOYMENT_ACTOR_ID=tag, NDNSF_DEPLOYMENT_BOOT_ID=boot,
                   NDNSF_DEPLOYMENT_CONFIG_SHA256=digest)
        try:
            child, _ = cluster._spawn(owner, owner_root, tag, 'audit', command, env,
                                       lambda: _remaining(deadline))
            while child.poll() is None:
                _remaining(deadline)
                time.sleep(0.05)
            owner['audit_exit_code'] = child.returncode
            if child.returncode:
                raise cluster.ClusterError('SLURM_RECOVERY_AUDIT_EXIT:' + node['id'])
        finally:
            cluster._close(owner, owner_root)
        _closed_groups(owner, boot, digest)


def snapshot(state, *, timeout=30):
    """Scheduler completion alone never proves native success or node cleanup."""
    result = dict(schema=SCHEMA, deployment_id=state['deployment_id'],
                  boot_id=state['boot_id'], operation_stage=state['status'], status='UNKNOWN',
                  evidence_root=state['evidence_root'], processes=[],
                  readiness='UNVERIFIED', actor_cleanup='UNVERIFIED',
                  oracle='NOT_RUN',
                  failure=state.get('failure'), job_reference=state.get('job_reference'))
    reference = state.get('job_reference')
    if reference is None:
        return result
    try:
        result['scheduler'] = SlurmApptainerAdapter().deployment_status(reference, timeout=timeout)
        result['job_reference'] = result['scheduler']['reference']
    except (SlurmAdapterError, OSError) as error:
        result['scheduler_error'] = str(error)
        return result
    if result['scheduler'].get('never_allocated') is True:
        allocation = Path(state['evidence_root']) / 'allocation.json'
        scheduler = result['scheduler']
        if (scheduler.get('terminal') is not True or scheduler.get('state') != 'CANCELLED' or
                scheduler.get('live_verified') is not False or
                reference.get('boot_id') != state['boot_id'] or
                reference.get('configuration_sha256') != state['configuration_sha256'] or
                state.get('launches') != [] or allocation.exists() or allocation.is_symlink()):
            result['allocation_error'] = 'SCHEDULER_NEVER_ALLOCATED_CONFLICT'
            return result
        result.update(status='STOPPED', actor_cleanup='ZERO',
                      cleanup_scope='SCHEDULER_NEVER_ALLOCATED', readiness='NOT_STARTED',
                      execution='NOT_STARTED', failure=state.get('failure') or
                      'DEPLOYMENT_CANCELLED_BEFORE_ALLOCATION')
        return result
    try:
        _allocation_result(state, result)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        result['allocation_error'] = str(error) or type(error).__name__
        try:
            _recovery_result(state, result)
        except (OSError, ValueError, RuntimeError, KeyError, TypeError) as recovery_error:
            result['recovery_error'] = str(recovery_error) or type(recovery_error).__name__
    return result


def _save(root, state):
    write_json(root / 'state.json', state)
    write_json(Path(state['evidence_root']) / 'operation.json', state)


def _remaining(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise cluster.ClusterError('DEPLOYMENT_TIMEOUT')
    return remaining


def up(config, source, root, timeout, debug=False):
    """Persist a single submission intent before allowing any scheduler side effect."""
    cluster._private_directory(root)
    with cluster._lock(root):
        old = cluster._read_state(root)
        digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        if old and (old['status'] != 'STOPPED' or old.get('needs_closeout')):
            if old['configuration_sha256'] != digest:
                raise cluster.ClusterError('active deployment configuration differs; close it before a new up')
            return (snapshot(old, timeout=timeout) if old['schema'] == SCHEMA
                    else cluster._snapshot(old, root))
        boot = uuid.uuid4().hex
        directory = Path(config['storage']['evidence_root']) / config['deployment_id'] / boot
        cluster._private_directory(directory)
        frozen = directory / 'configuration.json'
        write_json(frozen, config)
        supervisor = (Path(__file__).parent / '_container/adapters/slurm-apptainer/scripts'
                      / 'run-allocation-topology.sh').resolve(strict=True)
        argv = ['bash', str(supervisor), '--deployment', str(frozen), '--control-python', sys.executable,
                '--timeout', str(timeout), '--boot-id', boot,
                '--configuration-sha256', digest, '--evidence', str(directory)]
        release_digest = hashlib.sha256(Path(config['release']['manifest']).read_bytes()).hexdigest()
        argv += ['--release-sha256', release_digest]
        if debug:
            argv.append('--debug')
        script = directory / 'allocation.sbatch'
        with script.open('x') as stream:
            stream.write('#!/bin/bash\nset -euo pipefail\nexec ' + shlex.join(argv) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        script.chmod(0o600)
        adapter = SlurmApptainerAdapter()
        reference = adapter.prepare_deployment(config, script, boot_id=boot,
                                               release_sha256=release_digest)
        state = dict(schema=SCHEMA, deployment_id=config['deployment_id'], boot_id=boot,
                     configuration_sha256=digest, evidence_root=str(directory), source=str(source),
                     status='SUBMITTING', launches=[], job_reference=reference,
                     needs_closeout=True, diagnostic_logging=debug, failure=None)
        # This is the authoritative intent, including the exact script digest.
        # If sbatch accepted the job but its reply is lost, up must reconcile it.
        _save(root, state)
        deadline = time.monotonic() + timeout
        try:
            state['job_reference'] = adapter.submit_deployment(
                config, reference, timeout=min(30, _remaining(deadline)))
            state['status'] = 'SUBMITTED'
            _save(root, state)
            while True:
                cluster._cancelled(root, state)
                result = adapter.deployment_status(state['job_reference'],
                                                   timeout=min(30, _remaining(deadline)))
                state['scheduler'] = result
                state['job_reference'] = result['reference']
                _save(root, state)
                if result['terminal']:
                    # The subsequent allocation/node result path must close this
                    # boundary before COMPLETE or STOPPED can be reported.
                    state['status'] = 'AWAITING_NODE_AUDIT'
                    _save(root, state)
                    return snapshot(state, timeout=min(30, _remaining(deadline)))
                time.sleep(min(0.25, _remaining(deadline)))
        except (Exception, KeyboardInterrupt) as error:
            state['failure'] = str(error) or type(error).__name__
            state['status'] = 'UNKNOWN'
            _save(root, state)
            # An unknown submission must be reconciled by the same reference;
            # never create a second intent or guess a numeric job to cancel.
            try:
                state['scheduler'] = adapter.cancel_deployment(state['job_reference'], timeout=30)
                state['job_reference'] = state['scheduler']['reference']
            except (SlurmAdapterError, OSError) as cleanup_error:
                state['cleanup_failure'] = str(cleanup_error)
            _save(root, state)
            return snapshot(state)


def run_slurm(action, source, root, timeout, debug=False):
    """Inspect or cancel the persisted job without requiring model/image inputs."""
    state = cluster._read_state(root)
    if state is None:
        return {'status': 'ABSENT', 'processes': []}
    if action == 'status':
        return snapshot(state, timeout=timeout)
    if action != 'down':
        raise cluster.ClusterError('unsupported Slurm action')
    write_json(root / 'cancel.json', {'boot_id': state['boot_id']})
    cancelled_boot = state['boot_id']
    deadline = time.monotonic() + timeout
    while True:
        try:
            with cluster._lock(root):
                state = cluster._read_state(root)
                if state['schema'] != SCHEMA:
                    raise cluster.ClusterError('operation platform changed during closeout')
                if state['boot_id'] != cancelled_boot:
                    raise cluster.ClusterError('operation boot changed during closeout')
                adapter = SlurmApptainerAdapter()
                try:
                    result = adapter.cancel_deployment(state['job_reference'],
                                                       timeout=min(30, _remaining(deadline)))
                    state['scheduler'] = result
                    state['job_reference'] = result['reference']
                except (SlurmAdapterError, OSError) as error:
                    state['cleanup_failure'] = str(error)
                state.update(status='AWAITING_NODE_AUDIT', needs_closeout=True)
                _save(root, state)
                closed = snapshot(state, timeout=min(30, _remaining(deadline)))
                allocation = Path(state['evidence_root']) / 'allocation.json'
                if (closed['actor_cleanup'] != 'ZERO' and not allocation.exists() and
                        not allocation.is_symlink() and state.get('scheduler', {}).get('terminal') is True):
                    try:
                        _recover_missing_closeout(state, root, deadline)
                    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
                        state['recovery_failure'] = str(error) or type(error).__name__
                        _save(root, state)
                    closed = snapshot(state, timeout=min(30, _remaining(deadline)))
                if closed['actor_cleanup'] == 'ZERO':
                    state.update(status='STOPPED', needs_closeout=False)
                    _save(root, state)
                    closed['operation_stage'] = 'STOPPED'
                return closed
        except cluster.ClusterError as error:
            if not str(error).startswith('DEPLOYMENT_BUSY'):
                raise
            time.sleep(min(0.1, _remaining(deadline)))
