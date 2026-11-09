"""Own native children in one PID namespace; the allocation owns host-node audit."""
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
from .allocation_topology import deployment_management_commands
from .container import (gpu_slots, load_frozen, load_gpu_allocation, management_role_id,
                        read_gpu_probe, step_visibility)
from .deployment_config import _check_installed_files, _name, _read_json
from .processes import write_json


_CANCEL_CHECK_INTERVAL = 1.0
_ROUTE_READBACK_ATTEMPTS = 20
_ROUTE_READBACK_INTERVAL = 0.1


def _receipt(state, target):
    payload = {key: state.get(key) for key in (
        'deployment_id', 'boot_id', 'configuration_sha256', 'release_sha256',
        'role_id', 'role_kind', 'node_id', 'host', 'job_id', 'step_id', 'pid_namespace',
        'status', 'readiness', 'execution', 'exit_code', 'failure', 'launches', 'cleanup',
        'started_at', 'wall_seconds', 'gpu_visibility', 'logical_devices', 'gpu_admission',
        'runtime_timing', 'image_inspection')}
    payload.update(schema='ndnsf-di-node-role-v1',
                   cleanup_scope=('HOST_AND_NATIVE_PROCESS_GROUPS' if state.get('host_container')
                                  else 'PID_NAMESPACE'),
                   host_audit='UNKNOWN')
    write_json(target, payload)
    return payload


def _container_command(args, argv, *, separate_session=True):
    command = [sys.executable, '-I', '-m', 'ndnsf_distributed_inference.ops.container',
               '--deployment', str(args.deployment), '--boot-id', args.boot_id,
               '--configuration-sha256', args.configuration_sha256,
               '--release-sha256', args.release_sha256, '--timeout', str(args.timeout)]
    command += (['--actor', args.actor] if args.actor else
                ['--management', args.management, '--node', args.node])
    if args.debug:
        command.append('--debug')
    if getattr(args, 'timing', False):
        command.append('--timing')
    return [*command, '--', *(['/usr/bin/setsid'] if separate_session else []), *argv]


def _stop_container_launcher(child):
    # Popen owns this unreaped direct child and its newly created session.
    # Do not use this transient ownership proof for persisted recovery.
    result = {'forced': False}
    if child.poll() is not None:
        return dict(result, exitCode=child.returncode)
    try:
        os.killpg(child.pid, signal.SIGTERM)
        child.wait(timeout=5)
    except ProcessLookupError:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            result['forced'] = True
        child.wait(timeout=5)
    return dict(result, exitCode=child.returncode)


def _native_container_process(child, expected_exe, bindings):
    """Find one owned native descendant, never another matching deployment."""
    from .allocation import _job_cgroup
    matches = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            proof = cluster.process_identity(int(entry.name))
            if (not proof or proof['uid'] != os.getuid() or proof.get('exe') != expected_exe or
                    proof.get('bindings') != bindings or proof['pgid'] != proof['pid']):
                continue
            current = proof['pid']
            for _ in range(64):
                if current == child.pid:
                    break
                fields = (Path('/proc') / str(current) / 'stat').read_text().rsplit(')', 1)[1].split()
                current = int(fields[1])
                if current <= 1:
                    break
            if current != child.pid:
                continue
            cgroup = (entry / 'cgroup').read_text()
            step = os.environ['SLURM_STEP_ID']
            if (not _job_cgroup(cgroup, os.environ['SLURM_JOB_ID']) or not re.search(
                    r'(?:^|/)step[_-]' + re.escape(step) + r'(?:/|\.|$)', cgroup)):
                continue
            status = (entry / 'status').read_text().splitlines()
            pids = [int(value) for line in status if line.startswith('NSpid:')
                    for value in line.split()[1:]]
            if not pids or pids[0] != proof['pid']:
                continue
            namespace = os.readlink(entry / 'ns/pid')
            matches.append((proof, pids[-1], namespace))
        except (OSError, ValueError, KeyError, IndexError, UnicodeError):
            continue
    if len(matches) > 1:
        raise cluster.ClusterError('NATIVE_CONTAINER_PROCESS_AMBIGUOUS')
    return matches[0] if matches else None


class _ContainerChild:
    """Expose the native host PID while retaining the owned launcher wait handle."""

    def __init__(self, child, proof):
        self.child = child
        self.pid = proof['pid']

    @property
    def returncode(self):
        return self.child.returncode

    def poll(self):
        return self.child.poll()

    def wait(self, timeout=None):
        return self.child.wait(timeout=timeout)


def _inspect_container_install(args, config, by_kind, state, root, env, guard):
    # This existing C++ worker waits for a complete input frame before doing
    # any assembly. Keep its pipe empty and open while inspecting the image.
    relative = 'libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker'
    executable = str(Path(config['release']['prefix']) / relative)
    inspection = dict(status='STARTING', purpose='IMAGE_ROOT_INSPECTION', expectedExitCode=2,
                      exitCheck='CODE_ONLY')
    state['image_inspection'] = inspection
    bindings = {key: env[key] for key in ('NDNSF_DEPLOYMENT_ACTOR_ID',
                'NDNSF_DEPLOYMENT_BOOT_ID', 'NDNSF_DEPLOYMENT_CONFIG_SHA256')}
    child = subprocess.Popen(_container_command(args, [executable, '--stdio-v1',
        '--metadata-bytes', '0']), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, env=env, start_new_session=True)
    proof = None
    failed = False
    try:
        while True:
            guard()
            match = _native_container_process(child, executable, bindings)
            if match:
                proof, _, namespace = match
                inspection.update(native_proof=proof, pid_namespace=namespace)
                break
            if child.poll() is not None:
                raise cluster.ClusterError('IMAGE_INSPECTION_HOLDER_EXIT:' + str(child.returncode))
            time.sleep(0.02)
        write_json(root / 'state.json', state)
        _check_installed_files(config['release'],
            _read_json(Path(config['release']['manifest'])), by_kind,
            image_root=Path('/proc') / str(proof['pid']) / 'root', extra_programs=(relative,))
        guard()
        child.stdin.close()
        child.stdin = None
        child.wait(timeout=5)
        data = child.stdout.read(4097)
        inspection.update(exitCode=child.returncode, outputBytes=len(data),
                           outputHex=data.hex() if len(data) <= 4096 else None)
        if child.returncode != 2 or len(data) > 4096:
            raise cluster.ClusterError('IMAGE_INSPECTION_HOLDER_EXIT')
        inspection['status'] = 'VERIFIED'
    except BaseException:
        failed = True
        inspection['status'] = 'FAILED'
        raise
    finally:
        errors = []
        try:
            if proof is not None:
                inspection['cleanup'] = cluster.stop_owned(proof)
                if (inspection['cleanup']['status'] != 'ZERO' or
                        inspection['cleanup'].get('forced')):
                    errors.append('native holder cleanup unverified or forced')
        except Exception as error:
            errors.append(str(error))
        try:
            inspection['launcher_cleanup'] = _stop_container_launcher(child)
            if inspection['launcher_cleanup']['forced']:
                errors.append('holder launcher cleanup forced')
        except Exception as error:
            errors.append(str(error))
        for stream in (child.stdin, child.stdout):
            if stream is not None:
                stream.close()
        inspection['cleanup_errors'] = errors
        if errors:
            print('IMAGE_INSPECTION_CLOSEOUT_ERRORS ' + json.dumps(errors), file=sys.stderr)
        if errors and not failed:
            inspection['status'] = 'FAILED'
            raise cluster.ClusterError('IMAGE_INSPECTION_CLEANUP:' + ';'.join(errors))
    write_json(root / 'state.json', state)


def _finite(state, root, argv, env, guard, *, allow_route_not_found=False,
            container_args=None):
    tag = 'management-' + str(len(state['launches']))
    owned_env = dict(env, NDNSF_DEPLOYMENT_ACTOR_ID=tag)
    launched = []
    def launch(command, **kwargs):
        child = subprocess.Popen(command, **kwargs)
        launched.append(child)
        return child
    command = (_container_command(container_args, argv, separate_session=False)
               if container_args else argv)
    try:
        child, row = cluster._spawn(state, root, tag, 'management', command, owned_env,
                                    guard, **({'launcher': launch} if container_args else {}))
        if container_args:
            row['native_argv'] = argv
        while child.poll() is None:
            guard()
            time.sleep(0.05)
    finally:
        for pending in launched:
            _stop_container_launcher(pending)
    row['exit_code'] = child.returncode
    write_json(root / 'state.json', state)
    with Path(row['log']).open('rb') as stream:
        data = stream.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise cluster.ClusterError('NATIVE_MANAGEMENT_LOG_TOO_LARGE')
    output = data.decode('utf-8')
    missing = allow_route_not_found and child.returncode == 6 and output.strip() == 'Route not found'
    if child.returncode and not missing:
        raise cluster.ClusterError('NATIVE_COMMAND_EXIT:' + str(child.returncode) + ':' + argv[0])
    return output


def _identity(config, actor, state, root, env, guard, container_args=None):
    executable = str(Path(config['release']['prefix']) / 'bin/ndnsf-identity-check')
    _finite(state, root, [executable, actor['credential_home'], actor['identity']], env, guard,
            container_args=container_args)


def _management(config, node, state, root, env, guard, container_args=None):
    executable = (str(Path(config['release']['prefix']) / 'bin/nfdc') if container_args else
                  shutil.which('nfdc'))
    if not executable:
        raise cluster.ClusterError('NATIVE_NFDC_UNAVAILABLE')
    faces = {}
    for command in deployment_management_commands(config, node['id']):
        check = command.get('check')
        for attempt in range(_ROUTE_READBACK_ATTEMPTS if check == 'route' else 1):
            guard()
            options = {'allow_route_not_found': True} if check == 'route' else {}
            output = _finite(state, root, [executable, *command['argv']], env, guard,
                              container_args=container_args, **options)
            if check != 'route' or output.strip() != 'Route not found':
                break
            if attempt + 1 == _ROUTE_READBACK_ATTEMPTS:
                raise cluster.ClusterError('NATIVE_ROUTE_READBACK_TIMEOUT')
            guard()
            time.sleep(_ROUTE_READBACK_INTERVAL)
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
    host_container = getattr(args, 'host_container', False)
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
    expected_bindings = dict(bindings)
    if host_container:
        expected_bindings['NDNSF_DEPLOYMENT_ACTOR_ID'] = management_role_id('step', role_id)
    mismatches = {key: dict(expected=value, observed=os.environ.get(key))
                  for key, value in expected_bindings.items() if os.environ.get(key) != value}
    for key in ('SLURM_JOB_ID', 'SLURM_STEP_ID'):
        if not os.environ.get(key):
            mismatches[key] = dict(expected='allocated job/step', observed=os.environ.get(key))
    if mismatches:
        raise cluster.ClusterError('NATIVE_ROLE_ENVIRONMENT_BINDING_MISMATCH:' +
                                   json.dumps(mismatches, sort_keys=True))
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
                 'runtime_timing': getattr(args, 'timing', False),
                 'host_container': host_container,
                 'started_at': time.time(), 'wall_seconds': 0}
        started = time.monotonic()
        container_children = []
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

        def launch_native(argv, **kwargs):
            child = subprocess.Popen(_container_command(args, argv), **kwargs)
            container_children.append(child)
            expected = str(Path(argv[0]).resolve())
            while True:
                guard()
                match = _native_container_process(child, expected, bindings)
                if match:
                    proof, namespace_pid, namespace = match
                    row = state['launches'][-1]
                    row['proof'] = proof
                    row['namespace_proof'] = dict(proof, pid=namespace_pid, pgid=namespace_pid)
                    state['pid_namespace'] = namespace
                    return _ContainerChild(child, proof)
                if child.poll() is not None:
                    raise cluster.ClusterError('NATIVE_CONTAINER_CHILD_EXIT:' + role_id + ':' +
                                               str(child.returncode))
                time.sleep(0.02)

        handler = signal.signal(signal.SIGTERM, terminate)
        save()
        try:
            home = (Path(actor['credential_home']) if actor else
                    Path(config['storage']['state_root']) / config['deployment_id'] / (role_id + '-home'))
            cluster._private_directory(home)
            # The outer wrapper removes host loader overrides. These values
            # belong to the exact image, including its CUDA and NFD libraries.
            image_environment = ({key: os.environ[key] for key in ('PATH', 'LD_LIBRARY_PATH')
                                  if key in os.environ} if not host_container else {})
            env = cluster._environment(home, node['nfd']['socket'], role_id, state)
            if host_container:
                env.update({key: value for key, value in os.environ.items()
                            if key.startswith('SLURM_')})
            env.update(image_environment)
            visibility_owner = actor
            if host_container and actor and actor['resources']['gpu_devices']:
                visibility_owner = {'resources': {'gpu_devices':
                    [None] * config['platform']['slurm']['gpu']['count']}}
            state['gpu_visibility'] = step_visibility(visibility_owner)
            state['logical_devices'] = ['cuda:' + str(index) for index in
                                         range(len(actor['resources']['gpu_devices']))] if actor else []
            if actor:
                by_kind = {key: [] for key in ('controller', 'artifact-authority', 'requester', 'provider', 'repo')}
                by_kind[kind] = [actor]
                if not host_container:
                    _check_installed_files(config['release'],
                                           _read_json(Path(config['release']['manifest'])), by_kind)
                argv, env = cluster._native_launch(config, actor, root, state,
                                                   visible_devices=state['gpu_visibility'])
                env.update(image_environment)
                if host_container:
                    env.update({key: value for key, value in os.environ.items()
                                if key.startswith('SLURM_')})
                    _inspect_container_install(args, config, by_kind, state, root, env, guard)
                if actor['resources']['gpu_devices']:
                    env.update({key: os.environ[key] for key in (
                        'SLURM_JOB_ID', 'SLURM_STEP_ID', 'SLURM_STEP_GPUS')})
                    allocation = load_gpu_allocation(args.deployment.parent / 'nodes' / node_id /
                                                     'gpu-allocation.json', args, config, node)
                    selected = gpu_slots(actor, allocation)
                    expected_uuids = [allocation['devices'][index]['uuid'] for index in selected]
                    if host_container:
                        state['gpu_visibility'] = ','.join(expected_uuids)
                    query = str(Path(config['release']['prefix']) / 'libexec/ndnsf-di/ndnsf-di-device-query')
                    output = _finite(state, root, [query, '--count', str(len(selected))], env, guard,
                                     container_args=args if host_container else None)
                    observed = read_gpu_probe(output, args, node,
                        role_id if host_container else state['launches'][-1]['id'],
                        len(selected), visibility=state['gpu_visibility'], step_id=os.environ['SLURM_STEP_ID'])
                    if [item['uuid'] for item in observed['devices']] != expected_uuids:
                        raise cluster.ClusterError('NATIVE_STEP_GPU_UUID_MISMATCH:' + role_id)
                    if host_container:
                        observed['query_launch_id'] = state['launches'][-1]['id']
                    state['gpu_admission'] = observed
                    save()
                _identity(config, actor, state, root, env, guard,
                           container_args=args if host_container else None)
            elif kind == 'nfdc':
                _management(config, node, state, root, env, guard,
                             container_args=args if host_container else None)
                state.update(readiness='MANAGEMENT_VERIFIED', execution='COMPLETE', exit_code=0)
            else:
                endpoint = Path(node['nfd']['socket'])
                if node['nfd']['mode'] != 'managed' or endpoint.exists() or endpoint.is_symlink():
                    raise cluster.ClusterError('NATIVE_NFD_SOCKET_NOT_OWNED')
                executable = (str(Path(config['release']['prefix']) / 'bin/nfd') if host_container
                              else shutil.which('nfd'))
                if not executable:
                    raise cluster.ClusterError('NATIVE_NFD_UNAVAILABLE')
                path = root / 'nfd.conf'
                path.write_text(cluster._nfd_config(node['nfd']['socket'], node['nfd']['port']))
                argv = [executable, '--config', str(path)]
            if kind != 'nfdc':
                child, row = cluster._spawn(state, root, role_id, kind, argv, env, guard,
                                            **({'launcher': launch_native} if host_container else {}))
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
                    if (actor and code == 0 and
                            cluster._provider_withdrawal_complete(actor, row)):
                        state.update(readiness='WITHDRAWN', execution='SERVICE_STOPPED')
                        save()
                        break
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
                close_error = None
                cleanup_errors = []
                try:
                    cluster._close(state, root)
                except BaseException as error:
                    close_error = error
                    # Receipt I/O may fail before _close reaches the native
                    # process. Drain its verified group without another write.
                    if host_container:
                        for owned in state['launches']:
                            if owned['id'] == role_id and cluster._valid_intent(owned, state):
                                try:
                                    cluster._recover(owned)
                                    closed = cluster._stop_launch(owned)
                                    if closed['status'] != 'ZERO' or closed.get('forced'):
                                        cleanup_errors.append(closed)
                                except Exception as cleanup_error:
                                    cleanup_errors.append(str(cleanup_error))
                finally:
                    for launcher in container_children:
                        try:
                            closed = _stop_container_launcher(launcher)
                            terminal = next((item for item in state.get('cleanup', [])
                                             if item['id'] == role_id), None)
                            if terminal is not None:
                                terminal.setdefault('exitCode', closed['exitCode'])
                                if closed['forced']:
                                    terminal['forced'] = True
                        except Exception as cleanup_error:
                            cleanup_errors.append(str(cleanup_error))
                if close_error is not None or cleanup_errors:
                    print('NATIVE_ROLE_CLOSEOUT_ERRORS ' + json.dumps({
                        'state_error': str(close_error) if close_error else None,
                        'cleanup_errors': cleanup_errors}), file=sys.stderr)
                    if close_error is not None:
                        raise close_error
                    raise cluster.ClusterError('NATIVE_CONTAINER_LAUNCHER_CLEANUP_FAILED')
                terminal = next((item for item in state['cleanup'] if item['id'] == role_id), {})
                if state['exit_code'] is None:
                    state['exit_code'] = terminal.get('exitCode')
                if (state['execution'] == 'SERVICE_STOPPED' and
                        state['exit_code'] not in (0, -signal.SIGTERM, 128 + signal.SIGTERM)):
                    state.update(execution='FAILED', failure='NATIVE_ROLE_SHUTDOWN_EXIT:' +
                                 role_id + ':' + str(state['exit_code']))
                if ((kind == 'requester' and state['execution'] == 'COMPLETE') or
                        state['readiness'] == 'WITHDRAWN') and (
                        terminal.get('status') != 'ZERO' or terminal.get('forced')):
                    boundary = ('PROVIDER_WITHDRAWAL_GROUP_CLEANUP:' if kind == 'provider'
                                else 'REQUESTER_GROUP_CLEANUP:')
                    state.update(execution='FAILED', failure=boundary +
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
    parser.add_argument('--timing', action='store_true')
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
