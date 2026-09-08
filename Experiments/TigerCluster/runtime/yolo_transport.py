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
