"""Freeze the small, explicit YOLO harness without copying runtime/model data.

This is content integrity, not source approval or runtime qualification. T007
must review the exact inventory, and launch gates must verify the bundle again.
Owner-writable parent directories are not a security boundary against that owner;
workers additionally mount this checked directory read-only.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat

from .yolo_profile import ClosureError, HASH, _file_identity, _object, _operator_path


# Explicit dependency closure: no recursive copying of the repository or models.
# Missing future application/collector files keep a production bundle incomplete.
REQUIRED_HARNESS_FILES = frozenset({
    "runtime/__init__.py", "runtime/baseline.py", "runtime/identities.py",
    "runtime/application.py",
    "runtime/worker.py", "runtime/yolo_worker.py", "runtime/yolo_profile.py",
    "runtime/yolo_submission.py", "runtime/yolo_bundle.py", "runtime/yolo_result.py",
    "runtime/yolo_operator.py", "runtime/yolo_collection.py", "runtime/yolo_graph_reference.py",
    "runtime/yolo_storage.py",
    "runtime/yolo_transport.py",
    "runtime/yolo_ssh.py",
    "runtime/yolo_negative.py",
    "runtime/yolo_launch_witness.py",
    "runtime/yolo_gpu_probe.py",
    "runtime/yolo_allocation.py",
    "apps/yolo.py", "apps/yolo_network.py", "jobs/yolo/submit.py", "jobs/yolo/run.sbatch",
    "schemas/tiger-yolo-v1.schema.json", "requirements-operator.txt",
    "lib/spec183_yolo_host_gate.py", "owners/yolo_reference.py", "owners/yolo_tensor_bundle.py",
})
MANIFEST = "harness-manifest.json"
MAX_FILE_BYTES = 4 * 1024 * 1024
MAX_TOTAL_BYTES = 16 * 1024 * 1024
PRIVATE_PEM = re.compile(rb"^-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----\r?$", re.M)
# The frozen harness is read-only, but Slurm must be able to execute its
# allocation wrapper after transport.  Keep this explicit instead of
# inheriting arbitrary source-tree modes into the immutable bundle.
EXECUTABLE_HARNESS_FILES = frozenset({"jobs/yolo/run.sbatch"})


def harness_source(root: Path, name: str) -> Path:
    """Map existing generic owners into generated frozen snapshots.

    Its maintained source stays in DI. Only the actual repository builder may
    read that source; frozen bundles and arbitrary source roots use their own
    declared file. No recursive source copying or runtime checkout fallback.
    """
    root = Path(root)
    tiger = Path(__file__).resolve().parents[1]
    repository = tiger.parents[1]
    sources = {
        'owners/yolo_reference.py': 'NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/reference.py',
        'owners/yolo_tensor_bundle.py': 'NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/tensor_bundle.py',
        # Tiger/lib is a pre-existing compatibility directory symlink. Freeze
        # its declared canonical owner, not the link or an arbitrary target.
        'lib/spec183_yolo_host_gate.py': 'packaging/ndnsf-di-container/lib/spec183_yolo_host_gate.py',
    }
    if (name in sources and root == tiger and tiger.name == 'TigerCluster'
            and tiger.parent.name == 'Experiments' and (repository / '.git').exists()):
        return repository / sources[name]
    return root / name


def _numpy_owner(name: str, root: Path | None = None):
    """Load the declared NumPy-only owner without importing native DI parents.

    Execution/collection callers verify their frozen harness before this call.
    This loader itself is not qualification authority.
    """
    import importlib.util
    import sys
    root = Path(__file__).resolve().parents[1] if root is None else Path(root)
    if name not in ('reference', 'tensor_bundle'):
        raise ClosureError('NUMPY_OWNER_NAME')
    path = harness_source(root, 'owners/yolo_' + name + '.py')
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ClosureError('REFERENCE_OWNER_SYMLINK')
    payload = _bytes(path)
    name = '_tiger_yolo_reference_' + hashlib.sha256(str(path).encode() + b'\0' + payload).hexdigest()
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        try:
            # Execute exactly the bytes just read, not another filesystem read.
            exec(compile(payload, str(path), 'exec'), module.__dict__)
        except BaseException:
            sys.modules.pop(name, None)
            raise
    return sys.modules[name]


def reference_owner(root: Path | None = None):
    return _numpy_owner('reference', root)


def tensor_bundle_owner(root: Path | None = None):
    return _numpy_owner('tensor_bundle', root)


def preparation_inventory(root: Path, plan: dict, *, receipt_present: bool = False) -> dict:
    """Exact public preparation inventory; integrity only, not authentication."""
    from .identities import identity_inventory
    root = Path(_operator_path(str(root), Path.cwd(), local=True))
    if any(p.is_symlink() for p in (root, *root.parents)) or not root.is_dir():
        raise ClosureError('PREPARATION_ROOT')
    roles = identity_inventory(plan['namespace'], plan['identities'])
    providers = {'BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'}
    if not {'user', 'controller', 'repo', *providers}.issubset(roles):
        raise ClosureError('PREPARATION_ROLES')
    # Verify the directory boundary before reading even the registry.
    contracts = root / 'contracts'
    if contracts.is_symlink() or not contracts.is_dir():
        raise ClosureError('PREPARATION_CONTRACTS')
    registry = json.loads(_bytes(contracts / 'trust-root-registry-v1.json'), object_pairs_hook=_object)
    if not isinstance(registry, dict):
        raise ClosureError('PREPARATION_REGISTRY')
    expected = {'root.cert', 'wrong-root.cert', 'identities.json',
        'recipient-public-keys.json', 'offer-public-key-map.json', 'offer-trust-root.json',
        'case-policy.json', 'case.json', 'trust-schema.conf', 'controller.policies',
        'service-manifest.json', 'service-manifest.json.sha256',
        'native-execution-plan.json', 'native-execution-plan.json.sha256',
        'runtime-publication.json', 'contracts/trust-root-registry-v1.json', 'contracts/authority.pub'}
    expected.update(role + '.cert' for role in roles)
    expected.update(directory + '/' + role + '.pub' for role in providers for directory in ('recipients', 'offers'))
    for owner in ('catalogue', 'modelManifest', 'artifactPolicyAuthority'):
        entry = registry.get(owner)
        if not isinstance(entry, dict):
            raise ClosureError('PREPARATION_REGISTRY')
        path = entry.get('publicKeyPath')
        if not isinstance(path, str) or not re.fullmatch(r'contracts/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.pub', path):
            raise ClosureError('PREPARATION_REGISTRY_PATH')
        expected.add(path)
    allowed = expected | ({'preparation.json'} if receipt_present else set())
    directories = {str(parent) for name in allowed for parent in Path(name).parents}
    found, pending = set(), [root]
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                path = Path(entry.path)
                relative = path.relative_to(root).as_posix()
                if entry.is_symlink():
                    raise ClosureError('PREPARATION_SYMLINK')
                if entry.is_dir(follow_symlinks=False):
                    if relative not in directories:
                        raise ClosureError('PREPARATION_EXTRA_DIRECTORY')
                    pending.append(path)
                elif entry.is_file(follow_symlinks=False) and relative in allowed:
                    found.add(relative)
                else:
                    raise ClosureError('PREPARATION_EXTRA_OR_SPECIAL_FILE')
    if found != allowed:
        raise ClosureError('PREPARATION_MISSING_FILE')
    records, total = {}, 0
    for name in sorted(expected):
        wire = _bytes(root / name)
        if PRIVATE_PEM.search(wire):
            raise ClosureError('PREPARATION_PRIVATE_KEY')
        total += len(wire)
        if total > MAX_TOTAL_BYTES:
            raise ClosureError('PREPARATION_TOO_LARGE')
        records[name] = {'bytes': len(wire), 'sha256': 'sha256:' + hashlib.sha256(wire).hexdigest()}
    return records


def verify_preparation(root: Path, plan: dict, *, expected_receipt_digest: str,
                       candidate_digest: str) -> dict:
    """Recompute public bytes against an externally pinned preparation receipt."""
    root = Path(_operator_path(str(root), Path.cwd(), local=True))
    if any(p.is_symlink() for p in (root, *root.parents)):
        raise ClosureError('PREPARATION_ROOT')
    raw = _bytes(root / 'preparation.json')
    if (not isinstance(expected_receipt_digest, str) or not HASH.fullmatch(expected_receipt_digest)
            or 'sha256:' + hashlib.sha256(raw).hexdigest() != expected_receipt_digest):
        raise ClosureError('PREPARATION_RECEIPT_DIGEST')
    receipt = json.loads(raw, object_pairs_hook=_object)
    if (not isinstance(receipt, dict) or receipt.get('schema') != 'tiger-yolo-preparation-v1'
            or receipt.get('status') != 'PREPARED' or receipt.get('qualification') != 'NOT_EVALUATED'
            or receipt.get('runId') != plan['runId'] or receipt.get('candidateDigest') != candidate_digest):
        raise ClosureError('PREPARATION_RECEIPT_BINDING')
    if receipt.get('publicFiles') != preparation_inventory(root, plan, receipt_present=True):
        raise ClosureError('PREPARATION_PUBLIC_BYTES')
    return receipt


def _bytes(path):
    try:
        fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE_BYTES:
                raise ClosureError("HARNESS_FILE_SIZE_OR_TYPE")
            payload = stream.read(MAX_FILE_BYTES + 1)
        if len(payload) > MAX_FILE_BYTES:
            raise ClosureError("HARNESS_FILE_SIZE_OR_TYPE")
        return payload
    except OSError as exc:
        raise ClosureError("HARNESS_FILE_UNAVAILABLE") from exc


def _load(manifest, expected, source_root=None):
    if not isinstance(expected, str) or not HASH.fullmatch(expected):
        raise ClosureError("HARNESS_MANIFEST_DIGEST")
    manifest = Path(_operator_path(str(manifest), Path.cwd(), local=True))
    source_root = (manifest.parent if source_root is None else
                   Path(_operator_path(str(source_root), Path.cwd(), local=True)))
    raw = _bytes(manifest)
    if "sha256:" + hashlib.sha256(raw).hexdigest() != expected:
        raise ClosureError("HARNESS_MANIFEST_DIGEST")
    try:
        value = json.loads(raw, object_pairs_hook=_object)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ClosureError("HARNESS_MANIFEST_DOCUMENT") from exc
    if (not isinstance(value, dict) or set(value) != {"schema", "files"}
            or value["schema"] != "tiger-yolo-harness-v1"
            or not isinstance(value["files"], dict)
            or set(value["files"]) != REQUIRED_HARNESS_FILES):
        raise ClosureError("HARNESS_INVENTORY")
    total = 0
    for row in value["files"].values():
        if (not isinstance(row, dict) or set(row) != {"bytes", "sha256"}
                or type(row["bytes"]) is not int or not 0 <= row["bytes"] <= MAX_FILE_BYTES
                or not isinstance(row["sha256"], str) or not HASH.fullmatch(row["sha256"])):
            raise ClosureError("HARNESS_FILE_RECORD")
        total += row["bytes"]
    if total > MAX_TOTAL_BYTES:
        raise ClosureError("HARNESS_TOTAL_BYTES")
    payloads = {}
    for name, row in sorted(value["files"].items()):
        source = harness_source(source_root, name)
        if any(p.is_symlink() for p in (source, *source.parents)):
            raise ClosureError('HARNESS_SOURCE_SYMLINK')
        _file_identity(source.parent, name, dict(row, path=source.name))
        raw_file = _bytes(source)
        if (len(raw_file) != row["bytes"]
                or "sha256:" + hashlib.sha256(raw_file).hexdigest() != row["sha256"]):
            raise ClosureError("HARNESS_CHANGED_DURING_CHECK:" + name)
        try:
            raw_file.decode("utf-8")
        except UnicodeError as exc:
            raise ClosureError("HARNESS_NOT_TEXT:" + name) from exc
        if b"\x00" in raw_file or PRIVATE_PEM.search(raw_file):
            raise ClosureError("HARNESS_FORBIDDEN_CONTENT:" + name)
        payloads[name] = raw_file
    return raw, payloads


def verify_harness(root: Path, *, expected_manifest_sha256: str) -> dict:
    """Check exact inventory, copied bytes and read-only filesystem modes."""
    root = Path(_operator_path(str(root), Path.cwd(), local=True))
    raw, payloads = _load(root / MANIFEST, expected_manifest_sha256)
    expected_files = set(payloads) | {MANIFEST}
    expected_dirs = {"."}
    for name in expected_files:
        expected_dirs.update(str(p) for p in Path(name).parents)
    def sealed(path):
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_mode & 0o222:
            raise ClosureError("HARNESS_NOT_SEALED")
        return info.st_mode

    actual_files, actual_dirs = set(), {"."}
    # Traverse only known directories. Reject an unexpected subtree at its
    # first entry, rather than recursively scanning arbitrary injected data.
    for name in sorted(expected_dirs):
        directory = root / name
        if not stat.S_ISDIR(sealed(directory)):
            raise ClosureError("HARNESS_DIRECTORY_TYPE")
        with os.scandir(directory) as entries:
            for entry in entries:
                relative = str(Path(entry.path).relative_to(root))
                if relative not in expected_files | expected_dirs:
                    raise ClosureError("HARNESS_EXTRA_CONTENT")
                mode = sealed(Path(entry.path))
                if stat.S_ISDIR(mode):
                    actual_dirs.add(relative)
                elif stat.S_ISREG(mode):
                    actual_files.add(relative)
                else:
                    raise ClosureError("HARNESS_FILE_TYPE")
    if actual_files != expected_files or actual_dirs != expected_dirs:
        raise ClosureError("HARNESS_EXTRA_CONTENT")
    return {"manifestSha256": expected_manifest_sha256, "files": len(payloads),
            "bytes": len(raw) + sum(map(len, payloads.values())),
            "integrity": "VERIFIED", "qualification": "NOT_EVALUATED"}


def freeze_harness(manifest: Path, destination: Path, *, expected_manifest_sha256: str,
                   source_root: Path | None = None) -> dict:
    """Copy verified small source bytes, not hardlinks to a mutable worktree.

    Validate every file before creating a destination. Destination must be new
    under an existing parent; partial failed writes are retained and never reused.
    The manifest is written last, then the tree is made read-only and rechecked.
    source_root lets generated manifests live outside the working tree; it
    changes physical lookup only, never expected file identities. No subprocess,
    SIF, model, secret provisioning or remote operation occurs.
    """
    raw, payloads = _load(manifest, expected_manifest_sha256, source_root=source_root)
    destination = Path(_operator_path(str(destination), Path.cwd(), local=True))
    if not destination.parent.is_dir():
        raise ClosureError("HARNESS_DESTINATION_PARENT")
    destination.mkdir(mode=0o700, exist_ok=False)
    for name, payload in list(payloads.items()) + [(MANIFEST, raw)]:
        path = destination / name
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        path.chmod(0o555 if name in EXECUTABLE_HARNESS_FILES else 0o444)
    directories = [p for p in destination.rglob("*") if p.is_dir()] + [destination]
    for directory in sorted(directories, key=lambda p: len(p.parts), reverse=True):
        directory.chmod(0o555)
        fd = os.open(str(directory), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    return verify_harness(destination, expected_manifest_sha256=expected_manifest_sha256)
