"""Own native children in one PID namespace; the allocation owns host-node audit."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import time

from . import cluster
from .allocation_topology import deployment_management_commands
from .container import (gpu_slots, load_frozen, load_gpu_allocation, management_role_id,
                        read_gpu_probe, step_visibility)
from .deployment_config import _check_installed_files, _name, _read_json
from .processes import write_json


_CANCEL_CHECK_INTERVAL = 1.0


def _receipt(state, target):
    payload = {key: state.get(key) for key in (
        'deployment_id', 'boot_id', 'configuration_sha256', 'release_sha256',
        'role_id', 'role_kind', 'node_id', 'host', 'job_id', 'step_id', 'pid_namespace',
        'status', 'readiness', 'execution', 'exit_code', 'failure', 'launches', 'cleanup',
        'started_at', 'wall_seconds', 'gpu_visibility', 'logical_devices', 'gpu_admission')}
    payload.update(schema='ndnsf-di-node-role-v1', cleanup_scope='PID_NAMESPACE',
                   host_audit='UNKNOWN')
    write_json(target, payload)
    return payload


def _finite(state, root, argv, env, guard):
    tag = 'management-' + str(len(state['launches']))
    owned_env = dict(env, NDNSF_DEPLOYMENT_ACTOR_ID=tag)
    child, row = cluster._spawn(state, root, tag, 'management', argv, owned_env, guard)
    while child.poll() is None:
        guard()
        time.sleep(0.05)
    row['exit_code'] = child.returncode
    write_json(root / 'state.json', state)
    if child.returncode:
        raise cluster.ClusterError('NATIVE_COMMAND_EXIT:' + str(child.returncode) + ':' + argv[0])
    with Path(row['log']).open('rb') as stream:
        data = stream.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise cluster.ClusterError('NATIVE_MANAGEMENT_LOG_TOO_LARGE')
    return data.decode('utf-8')


def _identity(config, actor, state, root, env, guard):
    executable = str(Path(config['release']['prefix']) / 'bin/ndnsf-identity-check')
    _finite(state, root, [executable, actor['credential_home'], actor['identity']], env, guard)


def _management(config, node, state, root, env, guard):
    executable = shutil.which('nfdc')
    if not executable:
        raise cluster.ClusterError('NATIVE_NFDC_UNAVAILABLE')
    faces = {}
    for command in deployment_management_commands(config, node['id']):
        output = _finite(state, root, [executable, *command['argv']], env, guard)
        check = command.get('check')
        if check == 'strategy':
            cluster._require_group_multicast(output, command['prefix'])
        elif check in ('face', 'route'):
            rows = [dict(token.split('=', 1) for token in line.split() if '=' in token)
                    for line in output.splitlines()]
            if check == 'face':
                identifiers = {row['faceid'] for row in rows
                               if row.get('remote') == command['remote'] and
                               row.get('faceid', '').isdigit()}
                if not identifiers:
                    raise cluster.ClusterError('NATIVE_ROUTE_FACE_READBACK_MISSING')
                faces[command['remote']] = identifiers
            else:
                expected = _name(command['prefix'], 'route.prefix')
                if not any(row.get('nexthop') in faces.get(command['remote'], set()) and
                           _name(row.get('prefix'), 'route.readback.prefix') == expected
                           for row in rows if 'prefix' in row):
                    raise cluster.ClusterError('NATIVE_ROUTE_READBACK_MISSING')


def run_role(args):
    config = load_frozen(args.deployment, args.configuration_sha256, args.boot_id,
                         args.release_sha256, private_parent=False)
    actor = next((item for item in config['actors'] if item['id'] == args.actor), None)
    if args.actor and (actor is None or actor['external']):
        raise cluster.ClusterError('NATIVE_OWNED_ACTOR_REQUIRED')
    node_id = actor['node'] if actor else args.node
    node = next((item for item in config['nodes'] if item['id'] == node_id), None)
    if node is None or node['host'] not in (socket.gethostname(), socket.gethostname().split('.')[0]):
        raise cluster.ClusterError('NATIVE_ALLOCATION_NODE_MISMATCH')
    role_id = actor['id'] if actor else management_role_id(args.management, node_id)
    kind = actor['kind'] if actor else args.management
    bindings = {'NDNSF_DEPLOYMENT_ACTOR_ID': role_id, 'NDNSF_DEPLOYMENT_BOOT_ID': args.boot_id,
                'NDNSF_DEPLOYMENT_CONFIG_SHA256': args.configuration_sha256}
    if (any(os.environ.get(key) != value for key, value in bindings.items()) or
            not os.environ.get('SLURM_JOB_ID') or not os.environ.get('SLURM_STEP_ID')):
        raise cluster.ClusterError('NATIVE_ROLE_ENVIRONMENT_BINDING_MISMATCH')
    evidence = (args.deployment.parent / 'actors' / role_id if actor else
                args.deployment.parent / 'nodes' / node_id / kind)
    root = evidence / 'runtime' / config['deployment_id']
    cluster._private_directory(root)
    receipt = evidence / 'receipt.json'
    with cluster._lock(root):
        if cluster._read_state(root) is not None:
            raise cluster.ClusterError('NATIVE_ROLE_ALREADY_ENTERED:' + str(root))
        state = {'schema': 'ndnsf-di-host-operation-v1', 'deployment_id': config['deployment_id'],
                 'boot_id': args.boot_id, 'configuration_sha256': args.configuration_sha256,
                 'release_sha256': args.release_sha256, 'evidence_root': str(root),
                 'role_id': role_id, 'role_kind': kind, 'node_id': node_id, 'host': socket.gethostname(),
                 'job_id': os.environ['SLURM_JOB_ID'], 'step_id': os.environ['SLURM_STEP_ID'],
                 'pid_namespace': os.readlink('/proc/self/ns/pid'), 'status': 'STARTING',
                 'readiness': 'UNVERIFIED', 'execution': 'STARTING', 'exit_code': None,
                 'failure': None, 'launches': [], 'diagnostic_logging': args.debug,
                 'started_at': time.time(), 'wall_seconds': 0}
        started = time.monotonic()
        deadline = started + args.timeout
        stopping = False
        ready = False
        next_cancel_check = started

        def terminate(signum, frame):
            nonlocal stopping
            stopping = True

        def guard():
            nonlocal stopping, next_cancel_check
            if stopping:
                raise KeyboardInterrupt('SIGTERM')
            now = time.monotonic()
            if now >= next_cancel_check:
                next_cancel_check = now + _CANCEL_CHECK_INTERVAL
                try:
                    cluster._cancelled(root, state)
                except cluster.ClusterError as error:
                    if str(error) != 'DEPLOYMENT_CANCELLED':
                        raise
                    stopping = True
                    raise KeyboardInterrupt('DEPLOYMENT_CANCELLED') from error
            if time.monotonic() >= deadline:
                raise cluster.ClusterError('NATIVE_ROLE_TIMEOUT:' + role_id)

        def save():
            state['wall_seconds'] = time.monotonic() - started
            write_json(root / 'state.json', state)
            return _receipt(state, receipt)

        handler = signal.signal(signal.SIGTERM, terminate)
        save()
        try:
            home = (Path(actor['credential_home']) if actor else
                    Path(config['storage']['state_root']) / config['deployment_id'] / (role_id + '-home'))
            cluster._private_directory(home)
            # The outer wrapper removes host loader overrides. These values
            # belong to the exact image, including its CUDA and NFD libraries.
            image_environment = {key: os.environ[key] for key in ('PATH', 'LD_LIBRARY_PATH')
                                 if key in os.environ}
            env = cluster._environment(home, node['nfd']['socket'], role_id, state)
            env.update(image_environment)
            state['gpu_visibility'] = step_visibility(actor)
            state['logical_devices'] = ['cuda:' + str(index) for index in
                                         range(len(actor['resources']['gpu_devices']))] if actor else []
            if actor:
                by_kind = {key: [] for key in ('controller', 'artifact-authority', 'requester', 'provider', 'repo')}
                by_kind[kind] = [actor]
                _check_installed_files(config['release'], _read_json(Path(config['release']['manifest'])), by_kind)
                argv, env = cluster._native_launch(config, actor, root, state,
                                                   visible_devices=state['gpu_visibility'])
                env.update(image_environment)
                if actor['resources']['gpu_devices']:
                    env.update({key: os.environ[key] for key in (
                        'SLURM_JOB_ID', 'SLURM_STEP_ID', 'SLURM_STEP_GPUS')})
                    allocation = load_gpu_allocation(args.deployment.parent / 'nodes' / node_id /
                                                     'gpu-allocation.json', args, config, node)
                    selected = gpu_slots(actor, allocation)
                    expected_uuids = [allocation['devices'][index]['uuid'] for index in selected]
                    query = str(Path(config['release']['prefix']) / 'libexec/ndnsf-di/ndnsf-di-device-query')
                    output = _finite(state, root, [query, '--count', str(len(selected))], env, guard)
                    observed = read_gpu_probe(output, args, node, state['launches'][-1]['id'],
                        len(selected), visibility=state['gpu_visibility'], step_id=os.environ['SLURM_STEP_ID'])
                    if [item['uuid'] for item in observed['devices']] != expected_uuids:
                        raise cluster.ClusterError('NATIVE_STEP_GPU_UUID_MISMATCH:' + role_id)
                    state['gpu_admission'] = observed
                    save()
                _identity(config, actor, state, root, env, guard)
            elif kind == 'nfdc':
                _management(config, node, state, root, env, guard)
                state.update(readiness='MANAGEMENT_VERIFIED', execution='COMPLETE', exit_code=0)
            else:
                endpoint = Path(node['nfd']['socket'])
                if node['nfd']['mode'] != 'managed' or endpoint.exists() or endpoint.is_symlink():
                    raise cluster.ClusterError('NATIVE_NFD_SOCKET_NOT_OWNED')
                executable = shutil.which('nfd')
                if not executable:
                    raise cluster.ClusterError('NATIVE_NFD_UNAVAILABLE')
                path = root / 'nfd.conf'
                path.write_text(cluster._nfd_config(node['nfd']['socket'], node['nfd']['port']))
                argv = [executable, '--config', str(path)]
            if kind != 'nfdc':
                child, row = cluster._spawn(state, root, role_id, kind, argv, env, guard)
            if kind == 'requester':
                state['requester'] = {'completion': 'RUNNING', 'exit_code': None}
                # Requester is a finite command, not a serving actor. Its native
                # contract reports completion by exit/statistics, without READY.
                state.update(status='REQUEST_RUNNING', readiness='FINITE_RUNNING', execution='RUNNING')
                save()
            while kind != 'nfdc':
                guard()
                code = child.poll()
                if kind == 'nfd' and not ready:
                    try:
                        info = Path(node['nfd']['socket']).lstat()
                    except FileNotFoundError:
                        info = None
                    if info and stat.S_ISSOCK(info.st_mode) and code is None:
                        row['socket'] = {'path': node['nfd']['socket'],
                                         'identity': [info.st_dev, info.st_ino, info.st_mtime_ns]}
                        ready = True
                        state['readiness'] = 'SOCKET_BOUND'
                elif actor and kind != 'requester' and not ready and code is None:
                    if cluster._ready(row, actor, state):
                        ready = True
                        state['readiness'] = 'NATIVE_READY'
                if ready and state['status'] == 'STARTING':
                    state.update(status='REQUEST_RUNNING' if kind == 'requester' else 'SERVICES_READY',
                                 execution='RUNNING')
                    save()
                if code is not None:
                    state['exit_code'] = code
                    if kind != 'requester' or code != 0:
                        raise cluster.ClusterError('NATIVE_CHILD_EXIT:' + role_id + ':' + str(code))
                    state.update(readiness='FINITE_EXITED', execution='COMPLETE',
                                 requester={'completion': 'COMPLETE', 'exit_code': code})
                    break
                time.sleep(0.05)
        except (Exception, KeyboardInterrupt) as error:
            state['execution'] = 'INTERRUPTED' if stopping else 'FAILED'
            if not (stopping and ready and kind not in ('requester', 'nfdc')):
                state['failure'] = str(error) or type(error).__name__
            else:
                state['execution'] = 'SERVICE_STOPPED'
            save()  # Publish the failure/stop before potentially lengthy group cleanup.
        finally:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                cluster._close(state, root)
                terminal = next((item for item in state['cleanup'] if item['id'] == role_id), {})
                if state['exit_code'] is None:
                    state['exit_code'] = terminal.get('exitCode')
                if kind == 'requester' and state['execution'] == 'COMPLETE' and (
                        terminal.get('status') != 'ZERO' or terminal.get('forced')):
                    state.update(execution='FAILED', failure='REQUESTER_GROUP_CLEANUP:' +
                                 terminal.get('status', 'UNKNOWN'))
                save()
            finally:
                signal.signal(signal.SIGTERM, handler)
        return _receipt(state, receipt)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deployment', required=True, type=Path)
    parser.add_argument('--boot-id', required=True)
    parser.add_argument('--configuration-sha256', required=True)
    parser.add_argument('--release-sha256', required=True)
    role = parser.add_mutually_exclusive_group(required=True)
    role.add_argument('--actor')
    role.add_argument('--management', choices=('nfd', 'nfdc'))
    parser.add_argument('--node')
    parser.add_argument('--timeout', type=float, required=True)
    parser.add_argument('--debug', action='store_true')
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or (args.management and not args.node):
        parser.error('finite positive timeout and management node are required')
    try:
        result = run_role(args)
    except (RuntimeError, ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(result, sort_keys=True))
    return 0 if result['status'] == 'STOPPED' and result['execution'] in ('COMPLETE', 'SERVICE_STOPPED') else 1


if __name__ == '__main__':
    raise SystemExit(main())
