"""Verify a frozen external application against its exact base SIF identity.

Content verification is not MiniNDN or GPU qualification.
"""
from pathlib import Path, PurePosixPath
import hashlib
import json
import re


def _digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def application_root(profile):
    """Resolve only the explicit layered profile; legacy commands stay distinct."""
    layout = profile.get('layout')
    reference = profile.get('applicationManifest')
    if 'layout' not in profile and 'applicationManifest' not in profile:
        return None
    if layout != 'layered-v1' or not isinstance(reference, dict):
        raise ValueError('APP_LAYOUT')
    if set(reference) != {'path', 'bytes', 'sha256'}:
        raise ValueError('APP_REFERENCE')
    manifest = Path(reference['path'])
    if (manifest.name != 'application-manifest.json' or manifest.is_symlink()
            or type(reference['bytes']) is not int
            or manifest.stat().st_size != reference['bytes']):
        raise ValueError('APP_REFERENCE')
    verify_application(manifest.parent, manifest_sha256=reference['sha256'],
                       base_sif_sha256='sha256:' + profile['sifSha256'])
    return manifest.parent


def verify_application(root, *, manifest_sha256, base_sif_sha256):
    root = Path(root)
    if (not root.is_absolute() or not root.is_dir()
            or any(p.is_symlink() for p in (root, *root.parents))):
        raise ValueError('APP_ROOT')
    manifest = root / 'application-manifest.json'
    if manifest.is_symlink() or _digest(manifest) != manifest_sha256:
        raise ValueError('APP_MANIFEST_DIGEST')
    body = json.loads(manifest.read_text())
    if (not isinstance(body, dict) or set(body) != {
            'schemaVersion', 'layout', 'scope', 'baseSifSha256', 'sourceSealDigest',
            'sourceRevision', 'buildIdentity', 'buildKey', 'files'}
            or body.get('schemaVersion') != 'spec183-external-application-v1'
            or body.get('layout') != 'layered-v1'
            or body.get('scope') != 'BUILT_APPLICATION_CANDIDATE'):
        raise ValueError('APP_MANIFEST_SCHEMA')
    if (not re.fullmatch(r'sha256:[0-9a-f]{64}', str(base_sif_sha256))
            or body.get('baseSifSha256') != base_sif_sha256):
        raise ValueError('APP_BASE_MISMATCH')
    if (not re.fullmatch(r'sha256:[0-9a-f]{64}', str(body.get('sourceSealDigest')))
            or not re.fullmatch(r'[0-9a-f]{40}', str(body.get('sourceRevision')))):
        raise ValueError('APP_SOURCE_IDENTITY')
    identity = body.get('buildIdentity', {})
    if (not isinstance(identity, dict)
            or set(identity) != {'baseSifSha256', 'flags', 'builderSha256', 'targets'}
            or identity.get('baseSifSha256') != base_sif_sha256
            or not isinstance(identity.get('flags'), str) or not identity['flags']
            or not re.fullmatch(r'sha256:[0-9a-f]{64}', str(identity.get('builderSha256')))
            or identity.get('targets') != ['App_ServiceController', 'di-native-provider', 'di-native-fault-provider']
            or hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
            != body.get('buildKey')):
        raise ValueError('APP_BUILD_IDENTITY')
    names = set()
    rows = body.get('files')
    if not isinstance(rows, list) or not rows:
        raise ValueError('APP_FILES')
    for row in rows:
        name = row.get('path') if isinstance(row, dict) else None
        if (not isinstance(row, dict) or set(row) != {'path', 'bytes', 'sha256'}
                or not isinstance(name, str) or not name or name in names
                or PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts
                or str(PurePosixPath(name)) != name or '\\' in name
                or name == 'application-manifest.json'):
            raise ValueError('APP_FILE_PATH')
        path = root / name
        # Applications must not shadow the foundational native/Python packages.
        if (path.name.startswith(('libndn-', 'libndnsd.', 'libnac-abe.', '_ndnsf.', '_py_repoclient.'))
                or name.startswith(('repo/pythonWrapper/', 'repo/NDNSF-DistributedRepo/pythonWrapper/'))):
            raise ValueError('APP_BASE_LIBRARY_SHADOW')
        if (any(p.is_symlink() for p in (path, *path.parents)) or not path.is_file()
                or type(row.get('bytes')) is not int or row['bytes'] != path.stat().st_size
                or not re.fullmatch(r'sha256:[0-9a-f]{64}', str(row.get('sha256')))
                or _digest(path) != row['sha256']):
            raise ValueError('APP_FILE_CHANGED:' + name)
        names.add(name)
    actual = set()
    for path in root.rglob('*'):
        if path.is_symlink():
            raise ValueError('APP_SYMLINK')
        if path.is_file() and path != manifest:
            actual.add(path.relative_to(root).as_posix())
    if actual != names:
        raise ValueError('APP_FILE_SET')
    if not {'bin/di-native-provider', 'bin/di-native-fault-provider',
            'bin/App_ServiceController'}.issubset(names):
        raise ValueError('APP_ENTRYPOINT_SET')
    return body
