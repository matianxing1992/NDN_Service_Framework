"""Explicit file transport boundary; never prepares, qualifies or submits a run."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile

from .yolo_profile import HASH

SCHEMA = 'tiger-yolo-transport-v1'
MODES = {0o400, 0o444, 0o500, 0o555, 0o600, 0o644, 0o700, 0o755}


def _path(value):
    if not isinstance(value, str) or not value or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError('TRANSPORT_PATH')
    path = Path(value)
    if (not path.is_absolute() or str(path) != value or '..' in path.parts
            or path == Path(path.anchor)
            or any(p.is_symlink() for p in (path, *path.parents))):
        raise ValueError('TRANSPORT_PATH')
    return path


def _roots(roots):
    if not isinstance(roots, (tuple, list)) or len(roots) != 2:
        raise ValueError('TRANSPORT_ROOTS')
    result = tuple(_path(str(p)) for p in roots)
    if (result[0] == result[1] or result[0] in result[1].parents
            or result[1] in result[0].parents):
        raise ValueError('TRANSPORT_ROOTS')
    return result


def _destination(value, roots):
    path = _path(value)
    if not any(root in path.parents for root in roots):
        raise ValueError('TRANSPORT_OUTSIDE_ROOTS')
    return path


def _identity(path, output=None):
    path = _path(str(path))
    with os.fdopen(os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('TRANSPORT_REGULAR_FILE')
        digest = hashlib.sha256()
        count = 0
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
            count += len(chunk)
            if output is not None:
                output.write(chunk)
        after = os.fstat(stream.fileno())
    fields = ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')
    current = path.stat()
    if (any(getattr(before, k) != getattr(after, k) or getattr(after, k) != getattr(current, k)
            for k in fields) or count != before.st_size):
        raise ValueError('TRANSPORT_SOURCE_CHANGED')
    return dict(bytes=count, sha256='sha256:' + digest.hexdigest(),
                mode=stat.S_IMODE(before.st_mode))


def validate_manifest(value, *, roots):
    roots = _roots(roots)
    if (not isinstance(value, dict) or set(value) != {'schema', 'candidateDigest', 'roots', 'files'}
            or value['schema'] != SCHEMA or value['roots'] != [str(p) for p in roots]
            or not isinstance(value['candidateDigest'], str) or not HASH.fullmatch(value['candidateDigest'])
            or not isinstance(value['files'], list) or not 1 <= len(value['files']) <= 100000):
        raise ValueError('TRANSPORT_MANIFEST')
    names = []
    for row in value['files']:
        if (not isinstance(row, dict) or set(row) != {'path', 'bytes', 'sha256', 'mode', 'private'}
                or type(row['bytes']) is not int or row['bytes'] < 0
                or type(row['mode']) is not int or row['mode'] not in MODES
                or type(row['private']) is not bool
                or (row['private'] and row['mode'] not in (0o400, 0o600))
                or not isinstance(row['sha256'], str) or not HASH.fullmatch(row['sha256'])):
            raise ValueError('TRANSPORT_FILE_ROW')
        names.append(str(_destination(row['path'], roots)))
    if names != sorted(set(names)):
        raise ValueError('TRANSPORT_FILE_ORDER_OR_DUPLICATE')
    paths = {Path(name) for name in names}
    if any(any(parent in paths for parent in path.parents) for path in paths):
        raise ValueError('TRANSPORT_FILE_PARENT_COLLISION')
    return value


def inventory(paths, *, roots, candidate_digest, private_paths=()):
    """Hash explicitly selected files only. The caller owns semantic closure."""
    roots = _roots(roots)
    paths = list(paths)
    if len(paths) != len(set(map(str, paths))):
        raise ValueError('TRANSPORT_FILE_ORDER_OR_DUPLICATE')
    private_paths = set(map(str, private_paths))
    if not private_paths <= set(map(str, paths)):
        raise ValueError('TRANSPORT_PRIVATE_SELECTION')
    files = []
    for name in sorted(map(str, paths)):
        path = _destination(name, roots)
        files.append(dict(path=name, private=name in private_paths, **_identity(path)))
    return validate_manifest(dict(schema=SCHEMA, candidateDigest=candidate_digest,
        roots=[str(p) for p in roots], files=files), roots=roots)


def _matches(path, row):
    identity = _identity(path)
    return all(identity[key] == row[key] for key in ('bytes', 'sha256', 'mode'))


def candidate_inventory(profile_path, profile, prepared, *, provision, gates):
    """Enumerate the current fixed workload after the public CLI verifies gates.

    This consumes validated receipts; it does not infer PASS, walk result trees,
    inspect role homes, or execute native/model code. All locators must already
    use the declared same-name project layout.
    """
    from .yolo_profile import _read_plane
    from .yolo_bundle import REQUIRED_HARNESS_FILES, MANIFEST
    from .yolo_result import resolve_role_output
    roots = _roots((profile['storage']['remoteArtifactRoot'], profile['storage']['sharedRunRoot']))
    files = set()
    expected = {}
    def add(path, identity=None):
        path = _destination(str(path), roots)
        files.add(path)
        if identity is not None:
            row = {key:identity[key] for key in ('bytes','sha256') if key in identity}
            previous = expected.setdefault(path, {})
            if any(key in previous and previous[key] != value for key,value in row.items()):
                raise ValueError('TRANSPORT_EXPECTED_IDENTITY_CONFLICT')
            previous.update(row)
        return path
    def document(path):
        return _read_plane(add(path))
    def references(value):
        if isinstance(value, dict):
            if set(value) == {'path','bytes','sha256'}:
                add(value['path'], value)
            else:
                for item in value.values(): references(item)
        elif isinstance(value, list):
            for item in value: references(item)
    def bundle(path, harness_digest):
        path = Path(path)
        add(path/MANIFEST, {'sha256':harness_digest})
        rows = document(path/MANIFEST)['files']
        if set(rows) != REQUIRED_HARNESS_FILES:
            raise ValueError('TRANSPORT_HARNESS_INVENTORY')
        for name in REQUIRED_HARNESS_FILES: add(path/name, rows[name])
    def reference(package, repository):
        package = Path(package)
        value = document(package/'manifest.json')
        add(package/'oracle'/value['oracle']['outputPath'], {'sha256':value['oracle']['outputDigest']})
        add(Path(repository)/value['fixture']['path'], {'sha256':'sha256:'+value['fixture']['sha256']})
    add(Path(profile_path).absolute())
    references(profile)
    for plane in ('inputs','runtime','dispatch'):
        path = Path(profile['release'][plane]['path'])
        for row in document(path)['files'].values():
            add(path.parent/row['path'], row)
    # The E-plane harness is distinct from the prepared run's frozen copy.
    bundle(Path(profile['evidence']['harnessManifest']['path']).parent,
           profile['evidence']['harnessManifest']['sha256'])
    add(Path(prepared['plan']['output'])/'prepare.json')
    bundle(prepared['bundle'], prepared['harnessManifestSha256'])
    references(provision['publicInputs'])
    secret = add(provision['authorityPrivateKey'])
    package = Path(provision['package'])
    model = document(package/'manifest.json')
    add(package/'canonical/yolo26n.onnx', {'bytes':model['graph']['graphBytes'],'sha256':model['graph']['graphDigest']})
    add(package/model['weights']['path'], {'bytes':model['weights']['bytes'],'sha256':model['weights']['digest']})
    add(package/'oracle'/model['oracle']['outputPath'], {'sha256':model['oracle']['outputDigest']})
    for name, gate in gates.items():
        path = add(gate['path'])
        verdict = gate['receipt']
        if name == 'hostMinindn':
            add(verdict['sourceSeal']['path'], {'sha256':verdict['sourceSeal']['sha256']})
            references(verdict['cases'])
            continue
        root = path.parent
        saved = document(root/'prepare.json')
        bundle(saved['bundle'], saved['harnessManifestSha256'])
        collection = document(root/'collection-input.json')
        if collection['kind'] != 'normal':
            raise ValueError('TRANSPORT_NORMAL_PREREQUISITE_REQUIRED')
        def locate(path):
            return Path(os.path.abspath(str(root/Path(path))))
        nodes = {int(rank):locate(row['root']) for rank,row in collection['nodes'].items()}
        # Public reanalysis requires the issuer and each rank observation; a
        # receiver must retain them along with the original immutable verdict.
        version_roots = {'issuer': root/'prepare-output', **{str(rank):node for rank,node in nodes.items()}}
        if set(verdict.get('runtimeVersions', {})) != set(version_roots):
            raise ValueError('TRANSPORT_RUNTIME_VERSION_COVERAGE')
        for key, version_root in version_roots.items():
            observed = verdict['runtimeVersions'][key]
            add(version_root/'runtime-version/receipt.json', {'sha256':observed['receiptDigest']})
            add(version_root/'runtime-version/version.log', observed['log'])
        for node in nodes.values():
            receipt = document(node/'node-receipt.json')
            for launch in receipt['launches']:
                add(node/launch['logPath'], {'bytes':launch['logBytes'],'sha256':launch['logDigest']})
            if saved['case'] != 'local-cpu':
                add(node/'slurm-allocation.json'); add(node/'gpu-probe.json')
        for index, result in enumerate(verdict['requestResults']):
            request = nodes[0]/'user/requests'/str(index)
            for filename in ('graph-reference.json','lifecycle.jsonl','yolo-numerical.json',
                             'yolo-response.bin','yolo-public-assignments.json'):
                add(request/filename)
            for role, observed in result['execution']['roles'].items():
                if role != 'Merge':
                    add(resolve_role_output(nodes[observed['rank']]/role,
                        observed['native']['observation']['providerProfilePath']))
        for row in collection['references']:
            reference(locate(row['package']),locate(row['repository']))
        if saved['case'] != 'local-cpu':
            add(root/'allocation-terminal.json'); add(root/'srun-cleanup.json')
            for rank in nodes: add(root/('storage-rank'+str(rank)+'.json'))
    manifest = inventory(files, roots=roots, candidate_digest=prepared['candidateDigest'],
                         private_paths=[secret])
    for row in manifest['files']:
        if any(row[key] != value for key,value in expected.get(Path(row['path']),{}).items()):
            raise ValueError('TRANSPORT_EXPECTED_CONTENT_CHANGED')
    return manifest


def _capacity(pending):
    devices = {}
    for _, target, row in pending:
        parent = target.parent
        while not parent.exists():
            parent = parent.parent
        if not parent.is_dir():
            raise ValueError('TRANSPORT_DESTINATION_PARENT')
        device = parent.stat().st_dev
        info = os.statvfs(parent)
        free = info.f_bavail * info.f_frsize
        required, previous = devices.get(device, (0, free))
        devices[device] = (required + row['bytes'], min(previous, free))
    if any(required > free for required, free in devices.values()):
        raise ValueError('TRANSPORT_CAPACITY')


def receive(manifest, *, roots, staging, lock_root):
    """Publish only while the configured submission namespace is idle."""
    validate_manifest(manifest, roots=roots)
    lock_root = _path(str(lock_root))
    if any(lock_root == root or lock_root in root.parents or root in lock_root.parents
           for root in _roots(roots)):
        raise ValueError('TRANSPORT_LOCK_ROOT_OVERLAP')
    from .yolo_submission import transport_guard
    with transport_guard(lock_root):
        return _receive_files(manifest, roots=roots, staging=staging)


def _receive_files(manifest, *, roots, staging):
    """Verify staging/blobs/<sha256> then publish no-overwrite at original paths.

    Failed incoming staging is retained. Repeating this operation revalidates
    existing exact files; it never merges different bytes or calls a scheduler.
    """
    validate_manifest(manifest, roots=roots)
    staging = _path(str(staging))
    if not staging.is_dir():
        raise ValueError('TRANSPORT_STAGING')
    if any(staging == Path(row['path']) or staging in Path(row['path']).parents
           or Path(row['path']) in staging.parents for row in manifest['files']):
        raise ValueError('TRANSPORT_STAGING_OVERLAP')
    pending = []
    reused = 0
    # Validate every input and existing destination before the first publication.
    for row in manifest['files']:
        target = _destination(row['path'], _roots(roots))
        if target.exists():
            if not _matches(target, row):
                raise ValueError('TRANSPORT_DESTINATION_CONFLICT')
            reused += 1
            continue
        source = staging / 'blobs' / row['sha256'].split(':')[1]
        observed = _identity(source)
        if any(observed[key] != row[key] for key in ('bytes', 'sha256')):
            raise ValueError('TRANSPORT_BLOB_MISMATCH')
        pending.append((source, target, row))
    _capacity(pending)
    for source, target, row in pending:
        _destination(str(target), _roots(roots))
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix='.spec183-transfer-', dir=target.parent)
        temporary = Path(temporary)
        try:
            with os.fdopen(descriptor, 'wb') as output:
                observed = _identity(source, output)
                if any(observed[key] != row[key] for key in ('bytes', 'sha256')):
                    raise ValueError('TRANSPORT_BLOB_CHANGED')
                output.flush()
                os.fchmod(output.fileno(), row['mode'])
                os.fsync(output.fileno())
            try:
                os.link(temporary, target, follow_symlinks=False)
            except FileExistsError:
                if not _matches(target, row):
                    raise ValueError('TRANSPORT_DESTINATION_CONFLICT')
            directory = os.open(str(target.parent), os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            temporary.unlink()
    # The receipt describes transport only and can never substitute for a gate.
    for row in manifest['files']:
        if not _matches(Path(row['path']), row):
            raise ValueError('TRANSPORT_FINAL_MISMATCH')
    payload = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    return dict(schema='tiger-yolo-transport-receipt-v1', status='CONTENT_VERIFIED',
        qualification='NOT_EVALUATED', manifestDigest='sha256:' + hashlib.sha256(payload).hexdigest(),
        candidateDigest=manifest['candidateDigest'], files=len(manifest['files']), reused=reused)
