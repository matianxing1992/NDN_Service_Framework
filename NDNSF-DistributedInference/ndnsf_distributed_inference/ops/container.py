"""Node-side exact-image execution; application behavior stays in native roles."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import runpy
import signal
import socket
import stat
import subprocess
import tempfile
import time
from types import SimpleNamespace

from . import cluster
from .deployment_config import _digest_file, _name, _read_json, provider_recipient_map


SCRIPTS = Path(__file__).parent / '_container/adapters/slurm-apptainer/scripts'


def load_frozen(path, digest, boot_id, release_digest, *, private_parent=True):
    """Check the frozen control input without probing every remote role's files."""
    source = Path(path)
    if (not source.is_absolute() or source.resolve() != source or
            not re.fullmatch('[0-9a-f]{64}', digest) or
            not re.fullmatch('[0-9a-f]{32}', boot_id) or
            not re.fullmatch('[0-9a-f]{64}', release_digest)):
        raise cluster.ClusterError('CONTAINER_FROZEN_BINDING_INVALID')
    info = source.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o600):
        raise cluster.ClusterError('CONTAINER_FROZEN_FILE_NOT_PRIVATE')
    if private_parent:
        cluster._private_directory(source.parent)
    config = _read_json(source)
    actual = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if (actual != digest or config.get('schema') != 'ndnsf-di-deployment-v1' or
            config.get('platform', {}).get('kind') != 'slurm-apptainer'):
        raise cluster.ClusterError('CONTAINER_FROZEN_CONFIGURATION_MISMATCH')
    if _digest_file(Path(config['release']['manifest'])) != release_digest:
        raise cluster.ClusterError('CONTAINER_RELEASE_MANIFEST_MISMATCH')
    return config


def _canonical(path):
    value = Path(path)
    if (not value.is_absolute() or str(value).startswith('//') or
            value.resolve() != value or '..' in value.parts or
            any(character in str(value) for character in (':', ',', '\n', '\r'))):
        raise cluster.ClusterError('CONTAINER_BIND_PATH_INVALID:' + str(value))
    return value


def _contains(parent, child):
    return parent == child or parent in child.parents


def _admit_runtime_mount(value, config):
    forbidden = [Path(item) for item in ('/usr', '/lib', '/lib64', '/bin', '/sbin',
                                         '/proc', '/sys', '/dev')]
    forbidden.append(Path(config['release']['prefix']))
    if value == Path('/') or any(_contains(root, value) or _contains(value, root)
                                 for root in forbidden):
        raise cluster.ClusterError('CONTAINER_RUNTIME_OVERLAY_FORBIDDEN:' + str(value))


def _large_data_mount(config, actor, evidence, mounts):
    """Put Core's default file-publication directory on owned, budgeted disk."""
    if actor is None:
        return None
    target = Path('/tmp/ndnsf-large-data')
    _admit_runtime_mount(target, config)
    if mounts.get(evidence) != 'rw' or any(
            _contains(target, path) or _contains(path, target) for path in mounts):
        raise cluster.ClusterError('CONTAINER_LARGE_DATA_TARGET_CONFLICT')
    source = _canonical(evidence / 'large-data')
    cluster._private_directory(source)
    return source, target


def _journal_root_mount(config, actor, evidence, mounts):
    """Expose a private parent without binding other identities' journal data."""
    if actor is None or actor['kind'] != 'requester':
        return None
    journal = config['requester'].get('conversation', {}).get('journal')
    if not journal:
        return None
    target = _canonical(journal['state_root'])
    _admit_runtime_mount(target, config)
    identity = journal['identity']
    if not isinstance(identity, str) or not identity or identity in ('.', '..') or any(
            character in identity for character in '/\\'):
        raise cluster.ClusterError('CONTAINER_JOURNAL_IDENTITY_INVALID')
    child = _canonical(target / identity)
    if mounts.get(child) != 'rw' or any(
            path != child and (_contains(target, path) or _contains(path, target))
            for path in mounts):
        raise cluster.ClusterError('CONTAINER_JOURNAL_PARENT_CONFLICT:' + str(target))
    source = _canonical(evidence / 'journal-parent')
    cluster._private_directory(source)
    if any(path.name != identity for path in source.iterdir()):
        raise cluster.ClusterError('CONTAINER_JOURNAL_PARENT_NOT_EMPTY')
    placeholder = _canonical(source / identity)
    cluster._private_directory(placeholder)
    if any(placeholder.iterdir()):
        raise cluster.ClusterError('CONTAINER_JOURNAL_PLACEHOLDER_NOT_EMPTY')
    return source, target


def step_visibility(actor):
    """Validate the current process visibility without translating device numbers."""
    if actor is None or not actor['resources']['gpu_devices']:
        return ''
    visibility = os.environ.get('CUDA_VISIBLE_DEVICES', '')
    devices = visibility.split(',')
    if (not os.environ.get('SLURM_STEP_GPUS') or not os.environ.get('SLURM_STEP_ID') or
            not visibility or any(not re.fullmatch(
                '(?:[0-9]+|GPU-[0-9a-fA-F-]+|MIG-[A-Za-z0-9_./-]+)', token) for token in devices) or
            len(devices) != len(actor['resources']['gpu_devices']) or len(set(devices)) != len(devices)):
        raise cluster.ClusterError('CONTAINER_STEP_GPU_VISIBILITY_INVALID')
    return visibility


def management_role_id(kind, node_id):
    value = kind + '.' + node_id
    return value if len(value) <= 128 else kind + '.' + hashlib.sha256(node_id.encode()).hexdigest()


def gpu_selectors(role):
    values = role.get('resources', {}).get('gpu_devices', [])
    if any(not re.fullmatch(
            r'(?:0|[1-9][0-9]*|GPU-[0-9a-fA-F-]+|allocated:(?:[0-9]|[1-5][0-9]|6[0-3]))',
            value) for value in values):
        raise cluster.ClusterError('ALLOCATION_GPU_SELECTOR_INVALID:' + role['id'])
    return values


def gpu_slots(role, allocation):
    """Resolve physical identity or allocation-relative index through native UUID order."""
    devices = gpu_selectors(role)
    if not devices:
        return []
    if allocation is None:
        raise cluster.ClusterError('ALLOCATION_GPU_OBSERVATION_REQUIRED:' + role['id'])
    allocated = allocation['devices']
    if not 0 < len(allocated) <= 64:
        raise cluster.ClusterError('ALLOCATION_GPU_CAPACITY_INVALID')
    slots = []
    for device in devices:
        if device.startswith('allocated:'):
            slot = int(device[len('allocated:'):])
            matches = [slot] if slot < len(allocated) else []
        else:
            matches = [index for index, item in enumerate(allocated)
                       if device in (item['physical_id'], item['uuid'])]
        if len(matches) != 1 or matches[0] in slots:
            raise cluster.ClusterError('ALLOCATION_GPU_NOT_UNIQUELY_ALLOCATED:' + role['id'])
        slots.append(matches[0])
    return slots


def read_gpu_probe(output, args, node, role_id, count, *, visibility=None, step_id=None):
    """Validate a native SIF platform observation, without inferring inference success."""
    records = []
    for line in output.splitlines():
        try:
            item = json.loads(line)
        except (ValueError, UnicodeError):
            continue
        if isinstance(item, dict) and item.get('event') == 'NDNSF_DI_GPU_PROBE':
            records.append(item)
    if len(records) != 1:
        raise cluster.ClusterError('ALLOCATION_GPU_NATIVE_PROBE_MISSING_OR_DUPLICATE')
    value = records[0]
    expected = dict(schema='ndnsf-di-gpu-probe-v1', boot_id=args.boot_id,
                    configuration_sha256=args.configuration_sha256, release_sha256=args.release_sha256,
                    role_id=role_id, job_id=os.environ['SLURM_JOB_ID'], device_order='PCI_BUS_ID',
                    visibility=visibility if visibility is not None else ','.join(map(str, range(count))))
    if (any(value.get(key) != item for key, item in expected.items()) or
            not isinstance(value.get('step_id'), str) or not re.fullmatch(r'[0-9]+', value['step_id']) or
            (step_id is not None and value['step_id'] != step_id) or
            not isinstance(value.get('host'), str) or node['host'] not in (
                value['host'], value['host'].split('.')[0]) or not value.get('step_gpus')):
        raise cluster.ClusterError('ALLOCATION_GPU_OBSERVATION_MISMATCH:' + node['id'])
    visible, inventory = value.get('visible_uuids'), value.get('inventory')
    if (not isinstance(visible, list) or not 0 < count <= 64 or len(visible) != count or
            any(not isinstance(uuid, str) or not re.fullmatch(r'GPU-[0-9a-fA-F-]+', uuid) for uuid in visible) or
            len(set(visible)) != count or not isinstance(inventory, list) or not 0 < len(inventory) <= 256):
        raise cluster.ClusterError('ALLOCATION_GPU_NATIVE_INVENTORY_INVALID')
    if any(not isinstance(item, dict) or not isinstance(item.get('physical_id'), str) or
           not re.fullmatch(r'0|[1-9][0-9]*', item['physical_id']) or
           not isinstance(item.get('uuid'), str) for item in inventory):
        raise cluster.ClusterError('ALLOCATION_GPU_NATIVE_INVENTORY_INVALID')
    if (len({item['physical_id'] for item in inventory}) != len(inventory) or
            len({item['uuid'] for item in inventory}) != len(inventory)):
        raise cluster.ClusterError('ALLOCATION_GPU_NATIVE_INVENTORY_NOT_UNIQUE')
    devices = []
    for uuid in visible:
        matches = [item for item in inventory if item['uuid'] == uuid]
        if len(matches) != 1:
            raise cluster.ClusterError('ALLOCATION_GPU_UUID_NOT_IN_NATIVE_INVENTORY')
        devices.append(dict(matches[0]))
    return dict(schema='ndnsf-di-gpu-allocation-v1', node_id=node['id'], status='OBSERVED',
                boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                release_sha256=args.release_sha256, job_id=value['job_id'], step_id=value['step_id'],
                host=value['host'], visibility=value['visibility'], devices=devices, native_probe=value)


def load_gpu_allocation(path, args, config, node):
    info = path.lstat()
    if (path.resolve() != path or not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o600):
        raise cluster.ClusterError('CONTAINER_GPU_ALLOCATION_NOT_PRIVATE')
    value = _read_json(path)
    if not isinstance(value, dict) or 'native_probe' not in value:
        raise cluster.ClusterError('CONTAINER_GPU_ALLOCATION_INVALID')
    admitted = read_gpu_probe(json.dumps(value['native_probe']), args, node,
        management_role_id('gpu-probe', node['id']), config['platform']['slurm']['gpu']['count'])
    if admitted != value:
        raise cluster.ClusterError('CONTAINER_GPU_ALLOCATION_MISMATCH')
    return admitted


def _prepare_repository_locks(paths):
    """Prepare exact native sidecars without acquiring or replacing their leases."""
    def validate(path, info):
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1):
            raise cluster.ClusterError('CONTAINER_REPO_LOCK_INVALID:' + str(path))

    # Validate every existing sidecar before creating any absent one.
    existing = {}
    for path in sorted(paths):
        _canonical(path)
        try:
            info = path.lstat()
        except FileNotFoundError:
            existing[path] = None
        else:
            validate(path, info)
            existing[path] = (info.st_dev, info.st_ino)
    for path, previous in existing.items():
        _canonical(path)
        flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC
        if previous is None:
            flags |= os.O_CREAT | os.O_EXCL
        try:
            descriptor = os.open(path, flags, 0o600)
        except OSError as error:
            raise cluster.ClusterError('CONTAINER_REPO_LOCK_OPEN:' + str(path) + ':' + str(error)) from error
        try:
            info = os.fstat(descriptor)
            validate(path, info)
            _canonical(path)
            current = path.lstat()
            validate(path, current)
            identity = (info.st_dev, info.st_ino)
            if identity != (current.st_dev, current.st_ino) or (
                    previous is not None and identity != previous):
                raise cluster.ClusterError('CONTAINER_REPO_LOCK_CHANGED:' + str(path))
        finally:
            os.close(descriptor)


def role_mounts(config, source, actor=None, *, node=None, management=None):
    """Preserve absolute runtime paths while exposing only one role's inputs."""
    source = _canonical(source)
    node = node or next(item for item in config['nodes'] if item['id'] == actor['node'])
    if management:
        role_id = management_role_id(management, node['id'])
        evidence = source.parent / 'nodes' / node['id'] / management
        home = Path(config['storage']['state_root']) / config['deployment_id'] / (role_id + '-home')
        kind = management
        if kind == 'nfd' and node['nfd']['mode'] != 'managed':
            raise cluster.ClusterError('CONTAINER_EXTERNAL_NFD_NOT_OWNED')
    else:
        role_id, kind = actor['id'], actor['kind']
        evidence = source.parent / 'actors' / role_id
        home = Path(actor['credential_home'])
    # Only private output directories may be created. Credentials are never copied
    # or synthesized; native identity admission checks the provisioned HOME.
    if not management and not home.is_dir():
        raise cluster.ClusterError('CONTAINER_CREDENTIAL_HOME_MISSING')
    cluster._private_directory(home)
    cluster._private_directory(evidence)
    protected = [Path(item['credential_home']) for item in config['actors']
                 if not item['external'] and (kind == 'oracle' or item['node'] == node['id']) and
                 (management or item['id'] != role_id)]
    requester = config['requester']
    authority = config['artifact_authority'].get('authority', {})
    if kind != 'requester':
        if 'requester_private_key_file' in requester.get('grant', {}):
            protected.append(Path(requester['grant']['requester_private_key_file']))
        journal = requester.get('conversation', {}).get('journal', {})
        protected.extend(Path(item['file']) for item in journal.get('keys', []))
    if kind != 'artifact-authority':
        protected.extend(Path(authority[key]) for key in ('authority_private_key_file', 'content_key_file')
                         if key in authority)
    # Mapping values are path references; inspect no foreign private-key bytes.
    for provider in config['actors']:
        if (provider['external'] or provider['kind'] != 'provider' or
                (kind != 'oracle' and provider['node'] != node['id'])):
            continue
        mapping = provider['parameters'].get('recipient-key-map')
        if provider['id'] != role_id and 'selection-offer-key-file' in provider['parameters']:
            protected.append(Path(provider['parameters']['selection-offer-key-file']))
        if mapping:
            mapping_path = _canonical(mapping)
            for identity, key_path in _read_json(mapping_path).items():
                selected = _canonical(mapping_path.parent / key_path)
                if (management or provider['id'] != role_id or
                        _name(identity, 'recipient.identity') != _name(actor['identity'], 'actor.identity')):
                    protected.append(selected)
    protected = [_canonical(path) for path in protected]
    mounts = {}
    repository_locks = set()
    def add(path, *, writable=False, directory=False, credential=False, create_file=False):
        value = _canonical(path)
        _admit_runtime_mount(value, config)
        if any(_contains(value, secret) or _contains(secret, value) for secret in protected):
            raise cluster.ClusterError('CONTAINER_FOREIGN_CREDENTIAL_EXPOSURE:' + str(value))
        if directory:
            if writable:
                cluster._private_directory(value)
            elif not value.is_dir():
                raise cluster.ClusterError('CONTAINER_INPUT_DIRECTORY_MISSING:' + str(value))
        elif create_file:
            repository_locks.add(value)
        elif not value.is_file() or not os.access(value, os.R_OK):
            # Unix sockets are admitted separately below, never as model files.
            raise cluster.ClusterError('CONTAINER_INPUT_FILE_MISSING:' + str(value))
        if writable and not credential and any(_contains(value, path) for path in (home, source)):
            raise cluster.ClusterError('CONTAINER_WRITABLE_INPUT_OVERLAP:' + str(value))
        mode = 'rw' if writable else 'ro'
        if value in mounts and mounts[value] != mode:
            raise cluster.ClusterError('CONTAINER_BIND_MODE_CONFLICT:' + str(value))
        mounts[value] = mode

    def output(path):
        add(Path(path).parent, writable=True, directory=True)

    add(home, writable=True, directory=True, credential=True)
    add(evidence, writable=True, directory=True)
    add(source)
    add(config['release']['manifest'])
    if kind == 'oracle':
        from .allocation import _receipt_path, _roles
        add(config['platform']['build_record'])
        for filename in ('allocation.json', 'operation.json'):
            add(source.parent / filename)
        for current in config['nodes']:
            add(source.parent / 'nodes' / current['id'] / 'host-audit-final.json')
        for role in _roles(config):
            add(_receipt_path(SimpleNamespace(deployment=source), role))
            if role['kind'] != 'nfdc':
                add(source.parent / 'nodes' / role['node'] / ('host-audit-' + role['id'] + '.json'))
        gpu_nodes = {item['node'] for item in config['actors'] if not item['external'] and
                     item.get('resources', {}).get('gpu_devices')}
        for current in gpu_nodes:
            add(source.parent / 'nodes' / current / 'gpu-allocation.json')
        for current in config['actors']:
            if current['external'] or current['kind'] not in ('requester', 'provider'):
                continue
            runtime = source.parent / 'actors' / current['id'] / 'runtime' / config['deployment_id']
            if current['kind'] == 'requester':
                add(runtime / (current['id'] + '.json'))
                add(requester.get('statistics_file', str(runtime / (current['id'] + '-statistics.jsonl'))))
            else:
                add(current['parameters'].get('statistics-file', str(runtime / (current['id'] + '-statistics.jsonl'))))
        for turn in requester['turns']:
            for key in ('input_file', 'options_file', 'output_file', 'checkpoint_output_file'):
                if key in turn:
                    add(turn[key])
        return role_id, home, evidence, mounts
    if kind == 'nfd':
        add(Path(node['nfd']['socket']).parent, writable=True, directory=True)
    elif kind != 'gpu-probe':
        endpoint = _canonical(node['nfd']['socket'])
        if not stat.S_ISSOCK(endpoint.lstat().st_mode):
            raise cluster.ClusterError('CONTAINER_NFD_SOCKET_MISSING')
        mounts[endpoint] = 'ro'
    if management:
        return role_id, home, evidence, mounts
    if actor['resources']['gpu_devices']:
        add(source.parent / 'nodes' / node['id'] / 'gpu-allocation.json')
    for key in ('trust_schema', 'controller_certificate'):
        add(config['security'][key])
    if kind == 'controller':
        add(config['security']['controller_policy'])
    elif kind == 'provider':
        for path in config['model'].values():
            add(path)
        params = actor['parameters']
        add(params['grant-authority-public-key'])
        # NativeProtectedGrantCredentials reads the registry beside this hint,
        # then the declared public key relative to the registry's parent root.
        registry = _canonical(Path(params['grant-authority-public-key']).parent / 'trust-root-registry-v1.json')
        add(registry)
        relative = Path(_read_json(registry)['artifactPolicyAuthority']['publicKeyPath'])
        if relative.is_absolute() or '..' in relative.parts or not relative.parts:
            raise cluster.ClusterError('CONTAINER_REGISTRY_KEY_PATH_INVALID')
        add(registry.parent.parent / relative)
        add(params['recipient-key-map'])
        for path in provider_recipient_map(params['recipient-key-map'], actor['identity']).values():
            add(path)
        add(params.get('artifact-cache-dir', str(Path(config['storage']['cache_root']) / role_id)),
            writable=True, directory=True)
        if 'statistics-file' in params:
            output(params['statistics-file'])
        if 'selection-offer-key-file' in params:
            add(params['selection-offer-key-file'])
    elif kind == 'repo':
        add(config['storage']['repo_root'], writable=True, directory=True)
    elif kind == 'artifact-authority':
        for key in ('trust_schema_file', 'authority_private_key_file', 'requester_public_key_file',
                    'content_key_file'):
            if key in authority:
                add(authority[key])
        for path in authority.get('recipient_public_key_files', {}).values():
            add(path)
    elif kind == 'requester':
        for key, path in requester['grant'].items():
            if key.endswith('_file'):
                add(path)
        for path in requester.get('offer_admission', {}).get('public_key_files', {}).values():
            add(path)
        if 'options_file' in requester.get('request', {}):
            add(requester['request']['options_file'])
        catalog = requester['catalog']
        for obj in (catalog, catalog.get('source', {})):
            for key in ('file', 'initializer_file'):
                if key in obj:
                    add(obj[key])
        for key in ('repository', 'encrypted_repository'):
            if key in requester:
                add(requester[key]['path'], writable=True, directory=True)
                # BackendOwnershipLease uses the canonical backend path plus this suffix.
                lock = Path(str(_canonical(requester[key]['path'])) + '.authority.lock')
                add(lock, writable=True, create_file=True)
        for turn in requester['turns']:
            for key in ('input_file', 'options_file'):
                add(turn[key])
            for key in ('output_file', 'checkpoint_output_file'):
                if key in turn:
                    output(turn[key])
        if 'statistics_file' in requester:
            output(requester['statistics_file'])
        conversation = requester.get('conversation', {})
        if 'checkpoint_output_file' in conversation:
            output(conversation['checkpoint_output_file'])
        if 'parent_state_file' in conversation.get('turn', {}):
            add(conversation['turn']['parent_state_file'])
        journal = conversation.get('journal')
        if journal:
            add(Path(journal['state_root']) / journal['identity'], writable=True, directory=True)
            for key in journal['keys']:
                add(key['file'])
    # An output parent must not turn an individual read-only mount into writable
    # host access. The provisioned credential HOME is intentionally mutable.
    for target, mode in mounts.items():
        if mode == 'rw' and target != home and any(
                other_mode == 'ro' and _contains(target, other) for other, other_mode in mounts.items()):
            raise cluster.ClusterError('CONTAINER_WRITABLE_INPUT_OVERLAP:' + str(target))
    for target in repository_locks:
        if any(mode == 'ro' and (_contains(target, path) or _contains(path, target))
               for path, mode in mounts.items()):
            raise cluster.ClusterError('CONTAINER_WRITABLE_INPUT_OVERLAP:' + str(target))
    _prepare_repository_locks(repository_locks)
    return role_id, home, evidence, mounts


def stage_image(config, args, evidence, home, role_id, endpoint):
    """Own the complete staging process group, including timeout/cancel cleanup."""
    root = evidence / 'staging' / config['deployment_id']
    cluster._private_directory(root)
    with cluster._lock(root):
        old = cluster._read_state(root)
        if old:
            if (old['boot_id'] != args.boot_id or
                    old['configuration_sha256'] != args.configuration_sha256):
                raise cluster.ClusterError('SIF_STAGE_OPERATION_MISMATCH')
            if old['status'] != 'STOPPED' or old.get('needs_closeout'):
                raise cluster.ClusterError('SIF_STAGE_PREVIOUS_UNCLOSED:' + str(root))
        return _stage_owned(config, args, root, home, role_id, endpoint)


def _stage_owned(config, args, root, home, role_id, endpoint):
    state = {'schema': 'ndnsf-di-host-operation-v1', 'deployment_id': config['deployment_id'],
             'boot_id': args.boot_id, 'configuration_sha256': args.configuration_sha256,
             'evidence_root': str(root), 'status': 'STARTING', 'launches': []}
    stage_id = 'stage-' + hashlib.sha256(role_id.encode()).hexdigest()[:24]
    env = cluster._environment(home, endpoint, stage_id, state)
    if 'NDNSF_SIF_CACHE_DIR' in os.environ:
        env['NDNSF_SIF_CACHE_DIR'] = os.environ['NDNSF_SIF_CACHE_DIR']
    record = _read_json(Path(config['platform']['build_record']))
    deadline = time.monotonic() + args.timeout

    def guard():
        if time.monotonic() >= deadline:
            raise cluster.ClusterError('SIF_STAGE_TIMEOUT')

    platform = config['platform']
    started = time.monotonic()
    try:
        child, row = cluster._spawn(state, root, stage_id, 'staging',
            ['/bin/sh', str(SCRIPTS / 'stage-sif.sh'), platform['sif'],
             platform['sif_sha256'], str(record['sif']['bytes'])], env, guard)
        guard()
        code = child.wait(timeout=max(0.001, deadline - time.monotonic()))
        state['exit_code'] = code
    except BaseException as error:
        state['failure'] = str(error) or type(error).__name__
        raise
    finally:
        state['wall_seconds'] = time.monotonic() - started
        # Repeated scheduler TERM must not interrupt the bounded owned closeout.
        handler = signal.signal(signal.SIGTERM, signal.SIG_IGN)
        try:
            result = cluster._close(state, root)
        finally:
            signal.signal(signal.SIGTERM, handler)
    if result['status'] != 'STOPPED':
        raise cluster.ClusterError('SIF_STAGE_CLEANUP_UNKNOWN:' + str(root))
    with Path(row['log']).open('rb') as stream:
        raw = stream.read(1024 * 1024 + 1)
    if len(raw) > 1024 * 1024:
        raise cluster.ClusterError('SIF_STAGE_LOG_TOO_LARGE')
    lines = raw.decode('utf-8').splitlines()
    if code != 0:
        state['failure'] = 'SIF_STAGE_FAILED:' + str(code)
        cluster.write_json(root / 'state.json', state)
        raise cluster.ClusterError('SIF_STAGE_FAILED:' + str(code) + ':' + '\n'.join(lines[-8:]))
    image = lines[-1] if lines else ''
    if not image or not Path(image).is_absolute():
        raise cluster.ClusterError('CONTAINER_STAGED_IMAGE_INVALID')
    return image


def execute(args):
    config = load_frozen(args.deployment, args.configuration_sha256, args.boot_id, args.release_sha256)
    oracle = args.management == 'oracle'
    if not oracle and not os.environ.get('SLURM_JOB_ID'):
        raise cluster.ClusterError('APPTAINER_RUN_REQUIRES_SLURM')
    actor = next((item for item in config['actors'] if item['id'] == args.actor), None)
    if args.actor and (actor is None or actor['external']):
        raise cluster.ClusterError('CONTAINER_OWNED_ACTOR_REQUIRED')
    node_id = actor['node'] if actor else args.node
    node = next((item for item in config['nodes'] if item['id'] == node_id), None)
    if node is None or (not oracle and node['host'] not in (
            socket.gethostname(), socket.gethostname().split('.')[0])):
        raise cluster.ClusterError('CONTAINER_ALLOCATION_NODE_MISMATCH')
    role_id, home, evidence, mounts = role_mounts(config, args.deployment, actor,
                                                 node=node, management=args.management)
    large_data_mount = _large_data_mount(config, actor, evidence, mounts)
    target_plan = dict(mounts)
    if large_data_mount:
        target_plan[large_data_mount[1]] = 'rw'
    journal_mount = _journal_root_mount(config, actor, evidence, target_plan)
    workload = args.command
    if workload[:1] == ['--']:
        workload = workload[1:]
    probe = args.management == 'gpu-probe'
    if oracle:
        if args.command not in ([], ['--']):
            raise cluster.ClusterError('CONTAINER_ORACLE_WORKLOAD_FIXED')
        workload = [str(Path(config['release']['prefix']) / 'bin/spec190-multiturn-oracle'),
                    '--deployment-state', str(args.deployment.parent / 'allocation.json'),
                    '--require-multi-token']
        gpu_nodes = {item['node'] for item in config['actors'] if not item['external'] and
                     item['kind'] == 'provider' and item['resources']['gpu_devices']}
        if len(gpu_nodes) >= 2:
            workload.append('--require-distributed-gpu')
    elif probe:
        if not 0 < config['platform']['slurm']['gpu']['count'] <= 64:
            raise cluster.ClusterError('CONTAINER_GPU_PROBE_COUNT_INVALID')
        workload = [str(Path(config['release']['prefix']) / 'libexec/ndnsf-di/ndnsf-di-device-query'),
                    '--count', str(config['platform']['slurm']['gpu']['count'])]
        if args.command not in ([], ['--']):
            raise cluster.ClusterError('CONTAINER_GPU_PROBE_WORKLOAD_FIXED')
    elif not workload:
        # Use the image-installed console script's own interpreter/shebang.
        # Host sys.executable and an assumed image venv path are not portable.
        workload = [str(Path(config['release']['prefix']) / 'bin/ndnsf-di'), '_node',
                    '--deployment', str(args.deployment), '--boot-id', args.boot_id,
                    '--configuration-sha256', args.configuration_sha256,
                    '--release-sha256', args.release_sha256, '--timeout', str(args.timeout)]
        if args.actor:
            workload += ['--actor', args.actor]
        else:
            workload += ['--management', args.management, '--node', args.node]
        if args.debug:
            workload.append('--debug')
    platform = config['platform']
    gpu = probe or actor is not None and bool(actor['resources']['gpu_devices'])
    visibility = step_visibility({'resources': {'gpu_devices': [None] *
        config['platform']['slurm']['gpu']['count']}} if gpu else actor)
    if gpu and actor is not None:
        allocation = load_gpu_allocation(args.deployment.parent / 'nodes' / node_id /
                                         'gpu-allocation.json', args, config, node)
        slots = gpu_slots(actor, allocation)
        # Narrow the job allocation by stable identity, retaining declared logical order.
        # The native role independently verifies these UUIDs before launching its backend.
        visibility = ','.join(allocation['devices'][slot]['uuid'] for slot in slots)
    validator = runpy.run_path(str(SCRIPTS / 'validate-local-sif-build-record.py'))
    try:
        validator['validate'](platform['build_record'], platform['sif'], platform['sif_sha256'],
                              verify_sif_hash=False, purpose='deployment')
    except validator['BuildRecordError'] as error:
        raise cluster.ClusterError(str(error)) from error
    image = stage_image(config, args, evidence, home, role_id, node['nfd']['socket'])
    env = cluster._environment(home, node['nfd']['socket'], role_id,
                              {'boot_id': args.boot_id, 'configuration_sha256': args.configuration_sha256,
                               'release_sha256': args.release_sha256,
                               'diagnostic_logging': args.debug})
    # cleanenv retains the exact image's declared dependency PATH. Passing the
    # host-side generic PATH here would hide image-specific SDK/venv prefixes.
    env.pop('PATH')
    env.update(PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1')
    if os.environ.get('SLURM_JOB_ID'):
        env['SLURM_JOB_ID'] = os.environ['SLURM_JOB_ID']
    if 'SLURM_STEP_ID' in os.environ:
        env['SLURM_STEP_ID'] = os.environ['SLURM_STEP_ID']
    if gpu and 'SLURM_STEP_GPUS' in os.environ:
        env['SLURM_STEP_GPUS'] = os.environ['SLURM_STEP_GPUS']
    env['CUDA_VISIBLE_DEVICES'] = visibility
    env['CUDA_DEVICE_ORDER'] = 'PCI_BUS_ID'
    if any('\n' in value or '\r' in value for value in env.values()):
        raise cluster.ClusterError('CONTAINER_ENVIRONMENT_INVALID')
    # --env parses commas as assignment separators. A private env file preserves
    # an exact multi-GPU visibility string instead of silently changing it.
    descriptor, temporary = tempfile.mkstemp(prefix='.container-env-', dir=str(evidence))
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write(''.join(key + '=' + value + '\n' for key, value in sorted(env.items())))
        env_file = evidence / 'container.env'
        os.replace(temporary, env_file)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    command = [platform['apptainer'], 'exec', '--cleanenv', '--containall', '--no-eval',
               '--no-mount', 'hostfs,bind-paths',
               '--pwd', str(evidence), '--home', str(home) + ':' + str(home)]
    if gpu:
        command.append('--nv')
    command += ['--env-file', str(env_file)]
    if large_data_mount:
        source, target = large_data_mount
        command += ['--bind', str(source) + ':' + str(target) + ':rw']
    if journal_mount:
        parent, target = journal_mount
        command += ['--bind', str(parent) + ':' + str(target) + ':ro']
    for path, mode in sorted(mounts.items(), key=lambda item: str(item[0])):
        command += ['--bind', str(path) + ':' + str(path) + ':' + mode]
    command += [image, *workload]
    # Apptainer itself needs the host Slurm/GPU environment, but none of the
    # user's container override or loader variables may alter this invocation.
    host_env = {key: value for key, value in os.environ.items()
                if key in ('PATH', 'LANG', 'LC_ALL', 'TZ', 'CUDA_VISIBLE_DEVICES') or
                key.startswith('SLURM_')}
    os.execvpe(command[0], command, host_env)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deployment', required=True, type=Path)
    parser.add_argument('--boot-id', required=True)
    parser.add_argument('--configuration-sha256', required=True)
    parser.add_argument('--release-sha256', required=True)
    role = parser.add_mutually_exclusive_group(required=True)
    role.add_argument('--actor')
    role.add_argument('--management', choices=('nfd', 'nfdc', 'gpu-probe', 'oracle'))
    parser.add_argument('--node')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or (args.management and not args.node):
        parser.error('finite positive timeout and management node are required')
    def terminate(signum, frame):
        raise KeyboardInterrupt('SIGTERM')

    handler = signal.signal(signal.SIGTERM, terminate)
    try:
        try:
            execute(args)
        except (cluster.ClusterError, ValueError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
            parser.exit(1, str(error) + '\n')
    finally:
        signal.signal(signal.SIGTERM, handler)


if __name__ == '__main__':
    main()
