"""Local deployment processes; native programs own authorization and inference."""
from __future__ import annotations

import contextlib
from collections import deque
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import time
import uuid

from .deployment_config import (_identifier, _object, _read_json, _string, _name,
                                load_deployment, provider_recipient_map)
from .processes import process_identity, owned_state, stop_owned, write_json


_PROCESS_IDENTITY_CAPTURE_SECONDS = 2


class ClusterError(RuntimeError):
    pass


def _locator(path):
    """Stopping needs the state location, even if runtime/model inputs disappeared."""
    source = Path(path).resolve(strict=True)
    raw = _object(_read_json(source), 'deployment')
    if raw.get('schema') != 'ndnsf-di-deployment-v1':
        raise ClusterError('unsupported deployment schema')
    name = _identifier(raw.get('deployment_id'), 'deployment_id')
    root = _string(_object(raw.get('storage'), 'storage').get('state_root'), 'storage.state_root')
    return source, (source.parent / root).resolve() / name


def _read_state(root):
    path = root / 'state.json'
    if not path.exists():
        return None
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o600):
        raise ClusterError('operation record must be an owned private regular file')
    state = _object(_read_json(path), 'operation')
    if (state.get('schema') not in ('ndnsf-di-host-operation-v1', 'ndnsf-di-slurm-operation-v1') or
            state.get('deployment_id') != root.name or not isinstance(state.get('launches'), list) or
            any(not isinstance(row, dict) for row in state['launches']) or
            not isinstance(state.get('status'), str) or not isinstance(state.get('boot_id'), str) or
            not isinstance(state.get('configuration_sha256'), str) or
            not isinstance(state.get('evidence_root'), str)):
        raise ClusterError('invalid operation record')
    return state


def _private_directory(path):
    missing = []
    cursor = path
    while not cursor.exists():
        if cursor.is_symlink():
            raise ClusterError('operation directory must not use symlinks')
        missing.append(cursor)
        cursor = cursor.parent
    for parent in (cursor, *cursor.parents):
        info = parent.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid not in (0, os.getuid()) or
                (info.st_mode & 0o022 and not info.st_mode & stat.S_ISVTX)):
            raise ClusterError('operation directory has an untrusted ancestor: ' + str(parent))
    for directory in reversed(missing):
        try:
            directory.mkdir(mode=0o700)
        except FileExistsError:
            info = directory.lstat()
            if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or
                    stat.S_IMODE(info.st_mode) != 0o700):
                raise ClusterError('operation directory changed during creation')
    if path.resolve() != path:
        raise ClusterError('operation directory must not use symlinks')
    info = path.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o700):
        raise ClusterError('operation directory must be owned and mode 0700: ' + str(path))
    for parent in path.parents:
        info = parent.lstat()
        if (info.st_uid not in (0, os.getuid()) or
                (info.st_mode & 0o022 and not info.st_mode & stat.S_ISVTX)):
            raise ClusterError('operation directory has an untrusted ancestor: ' + str(parent))


def _operation_active(root, state):
    proof = state.get('manager')
    if not isinstance(proof, dict):
        return False
    try:
        actual = process_identity(proof['pid'])
        if not actual or actual['state'] == 'Z' or any(
                actual[key] != proof[key] for key in ('pid', 'uid', 'start_ticks', 'exe')):
            return False
        descriptor = os.open(str(root / '.lock'), os.O_RDONLY | os.O_NOFOLLOW)
        try:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
            return False
        finally:
            os.close(descriptor)
    except (OSError, ValueError, KeyError):
        return False


def _snapshot(state, root=None):
    if state is None:
        return {'status': 'ABSENT', 'processes': []}
    if state['schema'] == 'ndnsf-di-slurm-operation-v1':
        from .slurm import snapshot
        return snapshot(state)
    rows = []
    for row in state['launches']:
        valid = _valid_intent(row, state)
        actual = owned_state(row.get('proof')) if valid and row.get('proof') else 'UNKNOWN'
        if valid and row.get('spawn_failed') and not row.get('proof'):
            actual = 'EXITED'
        rows.append({'id': row.get('id', 'UNKNOWN'), 'kind': row.get('kind', 'UNKNOWN'), 'state': actual})
    status = state['status']
    services = [row for row in rows if row['kind'] != 'requester']
    if state.get('needs_closeout') or any(row['state'] == 'UNKNOWN' for row in rows):
        status = 'UNKNOWN'
    elif status in ('STARTING', 'SERVICES_READY', 'REQUEST_RUNNING', 'STOPPING') and (
            root is None or not _operation_active(root, state)):
        status = 'UNKNOWN'
    elif status == 'COMPLETE' and any(row['state'] != 'RUNNING' for row in services):
        status = 'DEGRADED'
    return {'schema': state['schema'], 'deployment_id': state['deployment_id'],
            'boot_id': state['boot_id'], 'status': status, 'processes': rows,
            'operation_stage': state['status'],
            'requester': state.get('requester'), 'evidence_root': state['evidence_root'],
            'external_references': state.get('external_references', []),
            'diagnostic_logging': state.get('diagnostic_logging', False),
            'failure': state.get('failure'), 'failure_boundary': state.get('failure_boundary'),
            'cleanup': state.get('cleanup')}


@contextlib.contextmanager
def _lock(root):
    descriptor = os.open(str(root / '.lock'), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(descriptor)
        if info.st_uid != os.getuid() or not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ClusterError('unsafe deployment lock')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ClusterError('DEPLOYMENT_BUSY: another up/down owns the operation')
        yield
    finally:
        os.close(descriptor)


def _environment(home, socket, actor_id, state):
    # Avoid development overrides or credentials from the invoking shell.
    env = {key: value for key, value in os.environ.items()
           if key in ('LANG', 'LC_ALL', 'TZ')}
    env.update(PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
               HOME=str(home), NDN_CLIENT_TRANSPORT='unix://' + str(socket),
               NDN_CLIENT_PIB='pib-sqlite3:' + str(Path(home) / '.ndn'),
               NDN_CLIENT_TPM='tpm-file:',
               NDN_LOG='*=DEBUG' if state.get('diagnostic_logging') else '*=NONE',
               NDNSF_DEPLOYMENT_ACTOR_ID=actor_id,
               NDNSF_DEPLOYMENT_BOOT_ID=state['boot_id'],
               NDNSF_DEPLOYMENT_CONFIG_SHA256=state['configuration_sha256'])
    if 'release_sha256' in state:
        env['NDNSF_DEPLOYMENT_RELEASE_SHA256'] = state['release_sha256']
    return env


def _valid_intent(row, state):
    try:
        expected = {'NDNSF_DEPLOYMENT_ACTOR_ID': _identifier(row['id'], 'actor.id'),
                    'NDNSF_DEPLOYMENT_BOOT_ID': state['boot_id'],
                    'NDNSF_DEPLOYMENT_CONFIG_SHA256': state['configuration_sha256']}
        if row['bindings'] != expected or not Path(row['expected_exe']).is_absolute():
            return False
        proof = row.get('proof')
        return not proof or (proof.get('bindings') == expected and proof.get('exe') == row['expected_exe'])
    except (ValueError, KeyError, TypeError, AttributeError):
        return False


def _native_launch(config, actor, directory, state, *, visible_devices=None):
    """Translate only maintained native CLI/config inputs, never model task placement."""
    prefix = Path(config['release']['prefix']) / 'bin'
    network, security = config['network'], config['security']
    controller = next(a['identity'] for a in config['actors'] if a['kind'] == 'controller')
    node = next(n for n in config['nodes'] if n['id'] == actor['node'])
    env = _environment(actor['credential_home'], node['nfd']['socket'], actor['id'], state)
    env['NDNSF_CONTROLLER_CERT_FILE'] = security['controller_certificate']
    cpus = str(actor['resources']['cpus'])
    env.update(OMP_NUM_THREADS=cpus, OPENBLAS_NUM_THREADS=cpus, MKL_NUM_THREADS=cpus,
               CUDA_DEVICE_ORDER='PCI_BUS_ID',
               CUDA_VISIBLE_DEVICES=(','.join(actor['resources']['gpu_devices'])
                                     if visible_devices is None else visible_devices))
    kind = actor['kind']
    if kind == 'controller':
        env['NDNSF_CONTROLLER_GENERATION_STATE'] = str(
            Path(actor['credential_home']) / '.ndn' / 'controller-generation.state')
        argv = [str(prefix / 'App_ServiceController'), '--controller-prefix', actor['identity'],
                '--no-demo-identities', '--policy-file', security['controller_policy'],
                '--trust-schema', security['trust_schema']]
    elif kind in ('requester', 'artifact-authority'):
        native = dict(config['requester' if kind == 'requester' else 'artifact_authority'])
        if kind == 'requester':
            native.setdefault('statistics_file', str(directory / (actor['id'] + '-statistics.jsonl')))
            for turn in native['turns']:
                for key in ('output_file', 'checkpoint_output_file'):
                    if key in turn:
                        Path(turn[key]).parent.mkdir(parents=True, exist_ok=True)
            checkpoint = native.get('conversation', {}).get('checkpoint_output_file')
            if checkpoint:
                Path(checkpoint).parent.mkdir(parents=True, exist_ok=True)
        path = directory / (actor['id'] + '.json')
        write_json(path, native)
        program = 'DI_NativeRequester' if kind == 'requester' else 'DI_NativeArtifactAuthority'
        argv = [str(prefix / program), '--config', str(path)]
    elif kind == 'repo':
        argv = [str(prefix / 'DistributedRepoServer'), '--repo-node', actor['identity'],
                '--database', str(Path(config['storage']['repo_root']) / (actor['id'] + '.sqlite')),
                '--group-prefix', network['group'], '--provider-identity', actor['identity'],
                '--authority-identity', controller, '--trust-schema', security['trust_schema'],
                '--service-prefix', network['repo_service']]
    else:
        params = dict(actor['parameters'])
        if not {'grant-authority-public-key', 'recipient-key-map'} <= params.keys():
            raise ClusterError('Provider requires grant-authority-public-key and recipient-key-map')
        env['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY'] = params.pop('grant-authority-public-key')
        recipient = provider_recipient_map(params.pop('recipient-key-map'), actor['identity'])
        path = directory / (actor['id'] + '-recipient.json')
        write_json(path, recipient)
        env['SPEC181_PROVIDER_RECIPIENT_KEY_MAP'] = str(path)
        params.setdefault('artifact-cache-dir', str(Path(config['storage']['cache_root']) / actor['id']))
        params.setdefault('statistics-file', str(directory / (actor['id'] + '-statistics.jsonl')))
        argv = [str(prefix / 'di-native-provider'), '--serve', '--provider', actor['identity'],
                '--group', network['group'], '--controller', controller,
                '--service', network['service'], '--trust-schema', security['trust_schema'],
                '--plan', config['model']['plan'], '--manifest', config['model']['manifest'],
                '--tokenizer-json', config['model']['tokenizer']]
        if 'repo_service' in network:
            argv += ['--repo-service', network['repo_service']]
        for key, value in sorted(params.items()):
            if type(value) is bool:
                if value:
                    argv.append('--' + key)
            else:
                argv += ['--' + key, str(value)]
        # CUDA renumbers the selected visible physical devices from zero.
        # The native V3 offer and runner contract uses this local namespace.
        for index in range(len(actor['resources']['gpu_devices'])):
            argv += ['--offer-device', 'cuda:' + str(index)]
    return argv, env


def _spawn(state, root, actor_id, kind, argv, env, guard):
    row = {'id': actor_id, 'kind': kind, 'argv': argv, 'expected_exe': str(Path(argv[0]).resolve()),
           'bindings': {key: env[key] for key in (
               'NDNSF_DEPLOYMENT_ACTOR_ID', 'NDNSF_DEPLOYMENT_BOOT_ID',
               'NDNSF_DEPLOYMENT_CONFIG_SHA256')},
           'log': str(Path(state['evidence_root']) / (actor_id + '.log'))}
    state['launches'].append(row)
    write_json(root / 'state.json', state)  # durable launch intent precedes Popen
    log = open(row['log'], 'wb')
    try:
        try:
            guard()
        except BaseException:
            row['spawn_failed'] = True  # Popen has not been called.
            write_json(root / 'state.json', state)
            raise
        try:
            child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, env=env, start_new_session=True)
        except OSError:
            row['spawn_failed'] = True
            write_json(root / 'state.json', state)
            raise
        capture_deadline = time.monotonic() + _PROCESS_IDENTITY_CAPTURE_SECONDS
        while True:
            guard()
            try:
                proof = process_identity(child.pid)
            except (OSError, ValueError, IndexError, UnicodeError):
                proof = None
            if proof and proof['state'] == 'Z':
                # Popen proved this child successfully exec'd this program. Keep its
                # kernel start identity while it is still our unreaped zombie.
                proof.update(exe=row['expected_exe'], bindings=row['bindings'])
            if (proof and proof.get('exe') == row['expected_exe'] and
                    proof.get('bindings') == row['bindings'] and
                    owned_state(proof) in ('RUNNING', 'EXITED')):
                row['proof'] = proof
                write_json(root / 'state.json', state)
                return child, row
            if time.monotonic() >= capture_deadline:
                raise ClusterError('cannot capture native process identity: ' + actor_id)
            time.sleep(0.01)
    finally:
        log.close()


def _recover(row):
    """Recover an interrupted launch only from its exact executable/launch binding."""
    if row.get('proof') or row.get('spawn_failed'):
        return
    matches = []
    for path in Path('/proc').iterdir():
        try:
            if not path.name.isdigit() or path.stat().st_uid != os.getuid():
                continue
            actual = process_identity(int(path.name))
        except (OSError, ValueError, UnicodeError):
            continue
        if (actual and actual['exe'] == row['expected_exe'] and
                actual['bindings'] == row['bindings'] and actual['pgid'] == actual['pid']):
            matches.append(actual)
    if len(matches) == 1:
        row['proof'] = matches[0]


def _close(state, root):
    actor_ids = [row.get('id', 'UNKNOWN') for row in reversed(state['launches'])]
    previous = {row.get('id'): row for row in (state.get('cleanup') or [])
                if isinstance(row, dict) and row.get('id') in actor_ids}
    cleanup_by_id = dict(previous)
    state.update(status='STOPPING', manager=process_identity(os.getpid()),
                 cleanup=[cleanup_by_id[key] for key in actor_ids if key in cleanup_by_id])
    write_json(root / 'state.json', state)
    for row in reversed(state['launches']):
        try:
            if not _valid_intent(row, state):
                result = {'status': 'UNKNOWN', 'error': 'launch binding does not match this operation'}
            else:
                _recover(row)
                result = ({'status': 'ZERO'} if row.get('spawn_failed') and not row.get('proof')
                          else stop_owned(row.get('proof')))
        except (OSError, ValueError, KeyError) as error:
            result = {'status': 'UNKNOWN', 'error': str(error)}
        actor_id = row.get('id', 'UNKNOWN')
        old = previous.get(actor_id, {})
        if old.get('forced'):
            result['forced'] = True
        for key in ('exitCode', 'reaped'):
            if key not in result and key in old:
                result[key] = old[key]
        record = dict(result, id=actor_id)
        cleanup_by_id[actor_id] = record
        state['cleanup'] = [cleanup_by_id[key] for key in actor_ids if key in cleanup_by_id]
        requester = state.get('requester')
        if (row.get('kind') == 'requester' and isinstance(requester, dict) and
                requester.get('completion') == 'RUNNING'):
            requester.update(completion='INTERRUPTED', cleanup=dict(record),
                             exit_code=record.get('exitCode'))
        if row.get('kind') == 'nfd' and result['status'] == 'ZERO' and row.get('socket'):
            path = Path(row['socket']['path'])
            try:
                info = path.lstat()
                if stat.S_ISSOCK(info.st_mode) and [info.st_dev, info.st_ino, info.st_mtime_ns] == row['socket']['identity']:
                    path.unlink()
            except FileNotFoundError:
                pass
            except OSError as error:
                record.update(status='UNKNOWN', error=str(error))
        write_json(root / 'state.json', state)
    state['status'] = 'STOPPED' if all(r['status'] == 'ZERO' for r in state['cleanup']) else 'UNKNOWN'
    write_json(root / 'state.json', state)
    return _snapshot(state, root)


def _ready(row, actor, state):
    path = Path(row['log'])
    with path.open('rb') as stream:
        raw = stream.read(1024 * 1024 + 1)
    if len(raw) > 1024 * 1024:
        raise ClusterError('readiness log exceeds 1 MiB: ' + actor['id'])
    for line in raw.splitlines():
        if actor['kind'] in ('controller', 'repo'):
            marker = (b'NDNSF_CONTROLLER_READY identity=' if actor['kind'] == 'controller'
                      else b'NDNSF_REPO_READY identity=')
            if line.startswith(marker):
                return _name(line[len(marker):].decode(), 'readiness.identity') == _name(
                    actor['identity'], 'actor.identity')
        elif line.startswith(b'{') and len(line) <= 4096:
            try:
                item = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            if item.get('event') != 'NDNSF_DEPLOYMENT_READY':
                continue
            return (item.get('actor_id') == actor['id'] and item.get('boot_id') == state['boot_id'] and
                    item.get('config_sha256') == state['configuration_sha256'] and
                    type(item.get('pid')) is int and item['pid'] == row['proof']['pid'] and
                    bool(item.get('native_boot_id')) and
                    _name(item.get('identity'), 'readiness.identity') == _name(actor['identity'], 'actor.identity'))
    return False


def _check_running(children):
    for actor, child, row in children:
        if actor['kind'] != 'requester' and child.poll() is not None:
            raise ClusterError('CHILD_EXIT:' + actor['id'] + ':' + str(child.returncode))


def _cancelled(root, state):
    path = root / 'cancel.json'
    if path.exists() and _read_json(path).get('boot_id') == state['boot_id']:
        raise ClusterError('DEPLOYMENT_CANCELLED')


def _nfd_config(socket, port):
    if any(char in str(socket) for char in ('"', '\\', '\n', '\r')):
        raise ClusterError('NFD socket contains unsupported configuration characters')
    return (f'general {{}}\nlog {{ default_level NONE }}\n'
            f'tables {{ cs_max_packets 4096 }}\n'
            f'face_system {{ unix {{ path "{socket}" }} '
            f'tcp {{ listen yes port {port} enable_v4 yes enable_v6 no }} '
            f'udp {{ listen yes port {port} enable_v4 yes enable_v6 no mcast no }} }}\n'
            'authorizations { authorize { certfile any privileges { faces fib strategy-choice } } }\n'
            'rib { localhost_security { trust-anchor { type any } } }\n')


def _configure_group_strategy(nfdc, env, config, directory, deadline, guard):
    """Configure our NFD's shared group; external forwarders are only inspected."""
    group = config['network']['group']
    multicast = '/localhost/nfd/strategy/multicast'
    commands = []
    if config['nodes'][0]['nfd']['mode'] == 'managed':
        commands.append(['strategy', 'set', 'prefix', group, 'strategy', multicast])
    commands.append(['strategy', 'list'])
    records = []
    for command in commands:
        guard()
        result = subprocess.run([nfdc, *command], env=env, stdin=subprocess.DEVNULL,
                                capture_output=True,
                                timeout=min(5, max(0.01, deadline - time.monotonic())))
        records.append({'arguments': command, 'exit_code': result.returncode,
                        'stdout': result.stdout.decode('utf-8', errors='replace'),
                        'stderr': result.stderr.decode('utf-8', errors='replace')})
        write_json(directory / 'group-forwarding.json', {'commands': records})
        guard()
        if result.returncode:
            raise ClusterError('NFD_GROUP_STRATEGY_COMMAND_FAILED')
    _require_group_multicast(records[-1]['stdout'], group)


def _require_group_multicast(stdout, group):
    from ndn.encoding import Component, Name

    group_name = Name.from_str(group)
    multicast = '/localhost/nfd/strategy/multicast'
    multicast_name = Name.from_str(multicast)
    choices = []
    try:
        for line in stdout.splitlines():
            fields = dict(token.split('=', 1) for token in line.split())
            if set(fields) != {'prefix', 'strategy'}:
                raise ValueError('invalid strategy choice row')
            prefix = Name.from_str(fields['prefix'])
            strategy = Name.from_str(fields['strategy'])
            is_multicast = (strategy == multicast_name or
                            (len(strategy) == len(multicast_name) + 1 and
                             strategy[:-1] == multicast_name and
                             Component.get_type(strategy[-1]) == Component.TYPE_VERSION))
            choices.append((prefix, is_multicast))
        ancestors = [(prefix, valid) for prefix, valid in choices
                     if Name.is_prefix(prefix, group_name)]
        effective = max(ancestors, key=lambda row: len(row[0]))[1] if ancestors else False
        descendants_valid = all(valid for prefix, valid in choices
                                if Name.is_prefix(group_name, prefix))
    except (ValueError, IndexError, TypeError, KeyError):
        raise ClusterError('NFD_GROUP_STRATEGY_READBACK_INVALID')
    if not effective or not descendants_valid:
        raise ClusterError('NFD_GROUP_MULTICAST_REQUIRED:' + group)


def _check_budget(config):
    """Monitor declared storage roots; never remove data to satisfy the budget."""
    used, seen = 0, set()
    for key, value in config['storage'].items():
        if key == 'max_bytes':
            continue
        root = Path(value)
        if not root.exists():
            continue

        def failed(error):
            raise error

        for parent, _, files in os.walk(root, followlinks=False, onerror=failed):
            for name in files:
                try:
                    info = (Path(parent) / name).lstat()
                except FileNotFoundError:
                    continue
                identity = info.st_dev, info.st_ino
                if stat.S_ISREG(info.st_mode) and identity not in seen:
                    used += info.st_size
                    seen.add(identity)
    if used > config['storage']['max_bytes']:
        raise ClusterError('STORAGE_BUDGET_EXCEEDED:' + str(used))
    return used


def _host_preflight(config):
    node = config['nodes'][0]
    if node['cpus'] > len(os.sched_getaffinity(0)):
        raise ClusterError('configured node CPUs exceed this process allocation')
    if not shutil.which('nfdc') or (node['nfd']['mode'] == 'managed' and not shutil.which('nfd')):
        raise ClusterError('required nfd/nfdc executable is unavailable')
    if any(actor['id'] == 'nfd.' + node['id'] for actor in config['actors']):
        raise ClusterError('actor ID conflicts with the managed NFD ID')
    for actor in config['actors']:
        if actor['external'] or actor['kind'] != 'provider':
            continue
        if not {'grant-authority-public-key', 'recipient-key-map'} <= actor['parameters'].keys():
            raise ClusterError('Provider requires grant-authority-public-key and recipient-key-map')
        if ('cuda' in actor['parameters'].get('offer-backend', '').lower() and
                not actor['resources']['gpu_devices']):
            raise ClusterError('CUDA Provider has no declared GPU device')
    if node['gpu_devices']:
        probe = subprocess.run(['nvidia-smi', '--query-gpu=index,uuid', '--format=csv,noheader'],
                               stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
        if probe.returncode:
            raise ClusterError('host GPU visibility probe failed')
        devices = {value.strip() for line in probe.stdout.splitlines() for value in line.split(',')}
        if not set(node['gpu_devices']) <= devices:
            raise ClusterError('configured host GPU device is unavailable')
    used = _check_budget(config)
    for key, value in config['storage'].items():
        if key == 'max_bytes':
            continue
        parent = Path(value)
        while not parent.exists():
            parent = parent.parent
        if shutil.disk_usage(parent).free < config['storage']['max_bytes'] - used:
            raise ClusterError('insufficient free capacity for the declared storage budget')


def _up(source, root, timeout, debug=False):
    config = load_deployment(source)
    if config['platform']['kind'] != 'host':
        from .slurm import up
        return up(config, source, root, timeout, debug)
    _private_directory(root)
    with _lock(root):
        old = _read_state(root)
        digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        if old and (old['status'] != 'STOPPED' or old.get('needs_closeout')):
            if old['configuration_sha256'] != digest:
                raise ClusterError('active deployment configuration differs; close it before a new up')
            # Acquiring this lock proves no previous up still owns it. A saved
            # nonterminal record is interrupted even when called in the same PID.
            return _snapshot(old)  # never resubmit a finite Requester
        _host_preflight(config)
        state = {'schema': 'ndnsf-di-host-operation-v1', 'deployment_id': config['deployment_id'],
                 'boot_id': uuid.uuid4().hex, 'configuration_sha256': digest,
                 'manager': process_identity(os.getpid()),
                 'diagnostic_logging': debug,
                 'status': 'STARTING', 'launches': [], 'source': str(source)}
        state['external_references'] = [dict(id=a['id'], kind=a['kind'], identity=a['identity'],
                                              verification='NOT_PROBED')
                                        for a in config['actors'] if a['external']]
        directory = Path(config['storage']['evidence_root']) / config['deployment_id'] / state['boot_id']
        _private_directory(directory)
        state['evidence_root'] = str(directory)
        write_json(root / 'state.json', state)
        write_json(directory / 'configuration.json', config)
        children = []
        started = time.monotonic()
        deadline = started + timeout
        last_budget = started
        request_start = None
        request_child = None
        request_actor = None

        def guard():
            nonlocal last_budget
            _cancelled(root, state)
            _check_running(children)
            if time.monotonic() >= deadline:
                raise ClusterError('DEPLOYMENT_TIMEOUT:' + state['status'])
            if time.monotonic() - last_budget >= 1:
                state['storage_bytes'] = _check_budget(config)
                last_budget = time.monotonic()
        try:
            node = config['nodes'][0]
            nfd = node['nfd']
            nfdc = shutil.which('nfdc')
            if not nfdc:
                raise ClusterError('nfdc executable is unavailable')
            launches = []
            for actor in config['actors']:
                if actor['external']:
                    continue
                guard()
                argv, env = _native_launch(config, actor, directory, state)
                checked = subprocess.run([str(Path(config['release']['prefix']) / 'bin/ndnsf-identity-check'),
                                          actor['credential_home'], actor['identity']],
                                         env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                         timeout=min(15, max(0.01, deadline - time.monotonic())))
                (directory / (actor['id'] + '-identity.log')).write_bytes(checked.stdout + checked.stderr)
                if checked.returncode:
                    raise ClusterError('IDENTITY_PREFLIGHT_FAILED:' + actor['id'])
                guard()
                launches.append((actor, argv, env))
            nfd_env = _environment(root / 'nfd-home', nfd['socket'], 'nfd.' + node['id'], state)
            # Native KeyChain initialization must not race the forwarder's fresh PIB.
            nfdc_env = _environment(root / 'nfdc-home', nfd['socket'], 'nfdc.' + node['id'], state)
            if nfd['mode'] == 'managed':
                if Path(nfd['socket']).exists():
                    raise ClusterError('managed NFD socket already exists; refusing to replace it')
                executable = shutil.which('nfd')
                if not executable:
                    raise ClusterError('nfd executable is unavailable')
                (root / 'nfd-home').mkdir(exist_ok=True, mode=0o700)
                Path(nfd['socket']).parent.mkdir(parents=True, exist_ok=True)
                path = directory / 'nfd.conf'
                path.write_text(_nfd_config(nfd['socket'], nfd['port']))
                child, row = _spawn(state, root, 'nfd.' + node['id'], 'nfd',
                                    [executable, '--config', str(path)], nfd_env, guard)
                children.append(({'id': row['id'], 'kind': 'nfd'}, child, row))
            while True:
                guard()
                _check_running(children)
                # A full report caches strategy/list before our strategy set.
                result = subprocess.run([nfdc, 'status', 'show'], env=nfdc_env,
                                        stdin=subprocess.DEVNULL, capture_output=True,
                                        timeout=min(5, max(0.01, deadline - time.monotonic())))
                if result.returncode == 0:
                    (directory / 'nfd-status.txt').write_bytes(result.stdout)
                    if nfd['mode'] == 'managed':
                        info = Path(nfd['socket']).lstat()
                        if not stat.S_ISSOCK(info.st_mode):
                            raise ClusterError('managed NFD did not create its configured socket')
                        children[0][2]['socket'] = {'path': nfd['socket'],
                            'identity': [info.st_dev, info.st_ino, info.st_mtime_ns]}
                        write_json(root / 'state.json', state)
                    break
                if time.monotonic() >= deadline:
                    raise ClusterError('NFD_READINESS_TIMEOUT')
                time.sleep(0.1)
            _configure_group_strategy(nfdc, nfdc_env, config, directory, deadline, guard)
            # Controller.start() proves its own PUBPARAMS path; NAC clients depend on it.
            for controller_phase in (True, False):
                pending = []
                for actor, argv, env in launches:
                    if actor['kind'] == 'requester' or (actor['kind'] == 'controller') != controller_phase:
                        continue
                    guard()
                    child, row = _spawn(state, root, actor['id'], actor['kind'], argv, env, guard)
                    children.append((actor, child, row))
                    pending.append((actor, child, row))
                while pending:
                    guard()
                    _check_running(children)
                    pending = [item for item in pending if not _ready(item[2], item[0], state)]
                    if pending and time.monotonic() >= deadline:
                        raise ClusterError('NATIVE_READINESS_TIMEOUT:' + ','.join(a['id'] for a, _, _ in pending))
                    if pending:
                        time.sleep(0.1)
            guard()
            state['status'] = 'SERVICES_READY'
            if pending == [] and any(a['kind'] in ('provider', 'artifact-authority', 'repo')
                                     for a, _, _ in children):
                for reference in state['external_references']:
                    if reference['kind'] == 'controller':
                        reference['verification'] = 'NATIVE_PERMISSION_BOOTSTRAP'
            write_json(root / 'state.json', state)
            for actor, argv, env in launches:
                if actor['kind'] != 'requester':
                    continue
                guard()
                request_start = time.monotonic()
                request_actor = actor['id']
                state['requester'] = {'exit_code': None, 'completion': 'RUNNING',
                                      'started_unix_seconds': time.time(), 'oracle': 'NOT_RUN'}
                child, row = _spawn(state, root, actor['id'], actor['kind'], argv, env, guard)
                request_child = child
                children.append((actor, child, row))
                state['status'] = 'REQUEST_RUNNING'
                write_json(root / 'state.json', state)
                while child.poll() is None:
                    guard()
                    _check_running(children)
                    if time.monotonic() >= deadline:
                        raise ClusterError('REQUESTER_TIMEOUT')
                    time.sleep(0.1)
                state['requester'].update(exit_code=child.returncode, completion='EXITED',
                                          wall_seconds=time.monotonic() - request_start)
                if child.returncode:
                    raise ClusterError('REQUESTER_EXIT:' + str(child.returncode))
                state['requester']['cleanup'] = stop_owned(row['proof'])
                terminal = state['requester']['cleanup']
                if terminal['status'] != 'ZERO' or terminal['forced']:
                    raise ClusterError('REQUESTER_GROUP_CLEANUP:' + terminal['status'])
            state['status'] = ('EXTERNAL_UNVERIFIED' if any(
                r['verification'] == 'NOT_PROBED' for r in state['external_references']) else 'COMPLETE')
            state['storage_bytes'] = _check_budget(config)
            state['wall_seconds'] = time.monotonic() - started
            write_json(root / 'state.json', state)
            write_json(directory / 'operation.json', state)
            return _snapshot(state, root)
        except BaseException as error:
            state['failure'] = str(error)
            frames = deque(maxlen=8)
            trace = error.__traceback__
            while trace is not None:
                frames.append({'file': trace.tb_frame.f_code.co_filename,
                               'function': trace.tb_frame.f_code.co_name,
                               'line': trace.tb_lineno})
                trace = trace.tb_next
            # Record the failure location without locals, payloads or debug logging.
            state['failure_boundary'] = {'type': type(error).__name__, 'frames': list(frames)}
            state['needs_closeout'] = any(row['kind'] == 'requester' and not row.get('spawn_failed')
                                         for row in state['launches'])
            state['wall_seconds'] = time.monotonic() - started
            if request_start is not None:
                state['requester'].update(completion='INTERRUPTED',
                    active_wall_seconds=time.monotonic() - request_start,
                    exit_code=request_child.poll() if request_child else None)
            handler = signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                result = _close(state, root)
            finally:
                signal.signal(signal.SIGTERM, handler)
            if request_start is not None:
                terminal = next((r for r in state['cleanup'] if r['id'] == request_actor), {})
                if state['requester']['exit_code'] is None:
                    state['requester']['exit_code'] = terminal.get('exitCode')
                state['requester']['wall_seconds'] = time.monotonic() - request_start
                write_json(root / 'state.json', state)
                result = _snapshot(state, root)
            write_json(directory / 'operation.json', state)
            if isinstance(error, (KeyboardInterrupt, SystemExit)):
                raise
            return result


def run_cluster(action, path, timeout=1800, *, debug=False):
    """Run the host operation or inspect/close its persisted owned processes."""
    if type(timeout) not in (int, float) or not 0 < timeout <= 3600:
        raise ClusterError('timeout must be in (0,3600] seconds')
    if type(debug) is not bool or (debug and action != 'up'):
        raise ClusterError('debug must be a boolean and only applies to up')
    source, root = _locator(path)
    if action == 'up':
        def terminate(signum, frame):
            raise KeyboardInterrupt('SIGTERM')
        handler = signal.signal(signal.SIGTERM, terminate)
        try:
            return _up(source, root, timeout, debug)
        finally:
            signal.signal(signal.SIGTERM, handler)
    if root.exists():
        _private_directory(root)
    state = _read_state(root)
    if state and state['schema'] == 'ndnsf-di-slurm-operation-v1':
        from .slurm import run_slurm
        return run_slurm(action, source, root, timeout, debug)
    if action == 'status' or state is None:
        return _snapshot(state, root)
    if action != 'down':
        raise ClusterError('unsupported cluster action')
    write_json(root / 'cancel.json', {'boot_id': state['boot_id']})
    deadline = time.monotonic() + min(timeout, 60)
    while True:
        try:
            with _lock(root):
                state = _read_state(root)
                state['needs_closeout'] = False
                result = _close(state, root)
                write_json(Path(state['evidence_root']) / 'operation.json', state)
                return result
        except ClusterError as error:
            if not str(error).startswith('DEPLOYMENT_BUSY') or time.monotonic() >= deadline:
                raise
            time.sleep(0.1)
