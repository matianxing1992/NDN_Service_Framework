"""YOLO candidate integrity, independent of runtime qualification and execution.

The operator profile and workload-specific receipt validators must consume
this layer. A verified digest is never permission to build, upload, or submit.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat


REQUIRED_FILES = {
    "inputs": {"sourceLock", "sourceSeal", "buildDefinition", "baseSif"},
    "runtime": {"sif", "nativeManifest", "libraryLock"},
    "dispatch": {"effectiveProfile", "harnessManifest", "modelManifest", "oracle",
                 "fixture", "trustPolicy", "validationContract"},
}
HASH = re.compile(r"sha256:[0-9a-f]{64}\Z")
APPLICATION_NAME = re.compile(
    r"/(?:[A-Za-z0-9_-][A-Za-z0-9_.-]*)(?:/[A-Za-z0-9_-][A-Za-z0-9_.-]*)*\Z"
)


class ClosureError(ValueError):
    """A candidate cannot be reproduced from the declared inputs."""


def application_sync_prefix(application_name: str) -> str:
    """Return the canonical Sync group for one application namespace.

    ``application_name`` is already an absolute NDN namespace.  Keeping the
    slash construction here prevents callers from deriving Sync from a
    Provider prefix or from accidentally producing ``//sync``/``/group``.
    """
    if not isinstance(application_name, str) or not APPLICATION_NAME.fullmatch(application_name):
        raise ClosureError("APPLICATION_NAME")
    return application_name + "/sync"


def _object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ClosureError("DUPLICATE_JSON_KEY")
        value[key] = item
    return value


def _read_plane(path):
    try:
        fd = os.open(str(path), os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise ClosureError("PLANE_FILE_TYPE")
            content = stream.read(4 * 1024 * 1024 + 1)
        if len(content) > 4 * 1024 * 1024:
            raise ClosureError("PLANE_TOO_LARGE")
        value = json.loads(content, object_pairs_hook=_object)
        # Reject non-finite numbers, including deeply nested values.
        json.dumps(value, allow_nan=False)
        return value
    except (OSError, UnicodeError, ValueError, RecursionError) as exc:
        if isinstance(exc, ClosureError):
            raise
        raise ClosureError("PLANE_DOCUMENT") from exc


def _file_identity(root, name, row):
    if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.:/-]{0,200}", name)
            or not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}):
        raise ClosureError("FILE_RECORD")
    if (type(row["bytes"]) is not int or row["bytes"] < 0
            or not isinstance(row["sha256"], str) or not HASH.fullmatch(row["sha256"])):
        raise ClosureError("FILE_METADATA:" + name)
    relative = row["path"]
    if (not isinstance(relative, str) or not relative
            or any(c in relative for c in "\x00\n\r\\")
            or PurePosixPath(relative).is_absolute()
            or any(part in ("", ".", "..") for part in relative.split("/"))):
        raise ClosureError("FILE_PATH:" + name)
    file = root
    for part in PurePosixPath(relative).parts:
        file = file / part
        if file.is_symlink():
            raise ClosureError("FILE_SYMLINK:" + name)
    try:
        file.resolve().relative_to(root)
    except ValueError as exc:
        raise ClosureError("FILE_PATH:" + name) from exc
    try:
        # NONBLOCK prevents special files from hanging a preflight. NOFOLLOW
        # rejects a replaced final symlink; workers must recheck before use.
        fd = os.open(str(file), os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode) or before.st_size != row["bytes"]:
                raise ClosureError("FILE_SIZE_OR_TYPE:" + name)
            h = hashlib.sha256()
            remaining = before.st_size
            while remaining:
                chunk = stream.read(min(remaining, 4 * 1024 * 1024))
                if not chunk:
                    raise ClosureError("FILE_CHANGED_DURING_CHECK:" + name)
                h.update(chunk)
                remaining -= len(chunk)
            if stream.read(1):
                raise ClosureError("FILE_CHANGED_DURING_CHECK:" + name)
            after = os.fstat(stream.fileno())
        current = file.stat()
        keys = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        if any(getattr(before, key) != getattr(other, key)
               for other in (after, current) for key in keys):
            raise ClosureError("FILE_CHANGED_DURING_CHECK:" + name)
    except OSError as exc:
        raise ClosureError("FILE_UNAVAILABLE:" + name) from exc
    observed = "sha256:" + h.hexdigest()
    if observed != row["sha256"]:
        raise ClosureError("FILE_DIGEST:" + name)
    return {"sha256": observed, "bytes": row["bytes"]}


def check_plane(path: Path, *, expected_stage: str, parent_id=None) -> dict:
    """Verify one content plane without requiring artifacts from a later stage."""
    path = Path(path).resolve()
    value = _read_plane(path)
    if (not isinstance(value, dict)
            or set(value) != {"schema", "stage", "parentId", "files", "parameters"}):
        raise ClosureError("PLANE_FIELDS")
    if not isinstance(expected_stage, str) or expected_stage not in REQUIRED_FILES:
        raise ClosureError("PLANE_STAGE")
    if value["schema"] != "tiger-yolo-plane-v1" or value["stage"] != expected_stage:
        raise ClosureError("PLANE_STAGE")
    if ((expected_stage == "inputs" and parent_id is not None)
            or (expected_stage != "inputs" and
                (not isinstance(parent_id, str) or not HASH.fullmatch(parent_id)))
            or value["parentId"] != parent_id):
        raise ClosureError("PLANE_PARENT")
    if not isinstance(value["parameters"], dict):
        raise ClosureError("PLANE_PARAMETERS")
    if (not isinstance(value["files"], dict) or len(value["files"]) > 4096
            or not REQUIRED_FILES[expected_stage].issubset(value["files"])):
        raise ClosureError("PLANE_INVENTORY")
    identities = {}
    for name, row in value["files"].items():
        identities[name] = _file_identity(path.parent, name, row)
    basis = {"schema": value["schema"], "stage": expected_stage,
             "parentId": parent_id, "files": identities,
             "parameters": value["parameters"]}
    encoded = json.dumps(basis, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    return {"id": "sha256:" + hashlib.sha256(encoded).hexdigest(),
            "stage": expected_stage, "integrity": "VERIFIED",
            "qualification": "NOT_EVALUATED"}


def check_chain(paths: dict, *, through: str) -> dict:
    """Recompute every ancestor; never accept a caller's remembered parent ID.

    This is the content-integrity portion only. Workload validators must still
    establish transitive inventory completeness, safe effective configuration,
    and genuine execution evidence before a launch can be authorized.
    """
    order = tuple(REQUIRED_FILES)
    if through not in order or not isinstance(paths, dict) or set(paths) - set(order):
        raise ClosureError("CHAIN_STAGES")
    needed = order[:order.index(through) + 1]
    if not set(needed).issubset(paths):
        raise ClosureError("CHAIN_STAGES")
    identities = {}
    parent_id = None
    for stage in needed:
        checked = check_plane(paths[stage], expected_stage=stage, parent_id=parent_id)
        parent_id = checked["id"]
        identities[stage] = parent_id
    return {"stage": through, "identities": identities,
            "integrity": "VERIFIED", "qualification": "NOT_EVALUATED"}


def _operator_path(value, base, *, local):
    # Apptainer's bind grammar uses colon/comma; do not permit ambiguous paths.
    if (any(c in value for c in ":,\\") or value.startswith("~")
            or any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise ClosureError("PROFILE_PATH")
    path = Path(value)
    if local:
        path = path if path.is_absolute() else base / path
        # Check before normalization: link/../x must not hide the link.
        for part in (path,) + tuple(path.parents):
            if part.is_symlink():
                raise ClosureError("PROFILE_SYMLINK")
        return os.path.abspath(str(path))
    if not path.is_absolute() or any(p in (".", "..", "") for p in value.split("/")[1:]):
        raise ClosureError("PROFILE_REMOTE_PATH")
    return str(path)


def load_operator_profile(path: Path, *, stage: str) -> dict:
    """Read-only schema/path/budget validation, NOT integrity or qualification.

    Relative local references are anchored to the profile, never the caller's
    cwd. Remote paths are lexical only: no SSH, mkdir, hashing, or launch here.
    Future-stage artifacts need not exist to describe an inputs-stage plan.
    Callers must separately verify content closure and genuine gate receipts.
    """
    if stage not in REQUIRED_FILES:
        raise ClosureError("PROFILE_STAGE")
    try:
        from jsonschema import Draft7Validator
    except ImportError as exc:
        raise ClosureError("PROFILE_OPERATOR_DEPENDENCIES") from exc
    path = Path(_operator_path(str(path), Path.cwd(), local=True))
    value = _read_plane(path)
    schema_path = Path(__file__).resolve().parents[1] / "schemas/tiger-yolo-v1.schema.json"
    schema = _read_plane(schema_path)
    Draft7Validator.check_schema(schema)
    error = next(Draft7Validator(schema).iter_errors(value), None)
    if error is not None:
        # Report location/constraint only, never interpolate user values.
        raise ClosureError("PROFILE_SCHEMA:" + "/".join(map(str, error.absolute_path))
                           + ":" + error.validator)

    def strict_scalars(item):
        if isinstance(item, float):
            raise ClosureError("PROFILE_INTEGER_REQUIRED")
        if isinstance(item, str) and any(ord(c) < 32 or ord(c) == 127 for c in item):
            raise ClosureError("PROFILE_CONTROL_CHARACTER")
        if isinstance(item, dict):
            for key, entry in item.items():
                strict_scalars(key)
                strict_scalars(entry)
        elif isinstance(item, list):
            for entry in item:
                strict_scalars(entry)

    strict_scalars(value)
    stages = tuple(REQUIRED_FILES)
    if not set(stages[:stages.index(stage) + 1]).issubset(value["release"]):
        raise ClosureError("PROFILE_STAGE_ARTIFACTS")
    from .identities import identity_inventory
    try:
        identity_inventory(value["security"]["identityNamespace"])
    except ValueError as exc:
        raise ClosureError("PROFILE_IDENTITY_NAMESPACE") from exc
    timing = value["timing"]
    requests = sum(value["schedule"]["twoNode"].values())
    minimum = (timing["stagingSeconds"] + timing["startupSeconds"]
               + (requests * timing["requestDeadlineMs"] + 999) // 1000
               + timing["cleanupSeconds"])
    if value["cluster"]["wallTimeSeconds"] < minimum:
        raise ClosureError("PROFILE_WALLTIME_BUDGET")
    if timing["progressTimeoutSeconds"] > (timing["requestDeadlineMs"] + 999) // 1000:
        raise ClosureError("PROFILE_PROGRESS_BUDGET")
    # Canonical document fingerprint is not the candidate E identity.
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()

    def resolve_refs(item):
        if isinstance(item, dict):
            if set(item) == {"path", "bytes", "sha256"}:
                item["path"] = _operator_path(item["path"], path.parent, local=True)
            else:
                for entry in item.values():
                    resolve_refs(entry)

    resolve_refs(value)
    value["runtime"]["apptainer"] = _operator_path(value["runtime"]["apptainer"], path.parent, local=True)
    value["security"]["authorityPrivateKey"] = _operator_path(
        value["security"]["authorityPrivateKey"], path.parent, local=True)
    for key in ("localArtifactRoot", "remoteArtifactRoot", "sharedRunRoot", "sharedLockRoot", "scratchRoot"):
        value["storage"][key] = _operator_path(value["storage"][key], path.parent,
                                              local=key == "localArtifactRoot")
    return {"structure": "VALIDATED", "integrity": "NOT_EVALUATED",
            "qualification": "NOT_EVALUATED", "stage": stage,
            "documentDigest": "sha256:" + hashlib.sha256(encoded).hexdigest(),
            "minimumWallTimeSeconds": minimum, "profile": value}


def effective_profile_document(profile: dict) -> dict:
    """One path-independent behavior representation for plans and dispatch."""
    def logical(item):
        if isinstance(item, dict):
            if set(item) == {'path', 'bytes', 'sha256'}:
                return {key: item[key] for key in ('bytes', 'sha256')}
            return {key: logical(value) for key, value in item.items()}
        if isinstance(item, list):
            return [logical(value) for value in item]
        return item

    behavior = logical(profile)
    behavior.pop('release')
    behavior.pop('profileId')
    behavior['runtime'].pop('apptainer')
    behavior['security'].pop('authorityPrivateKey')
    behavior['storage'] = {key: profile['storage'][key] for key in ('peakBytes', 'marginBytes')}
    return dict(schema='tiger-yolo-effective-profile-v1', profileId=profile['profileId'],
                effectiveBehavior=behavior)


def check_operator_profile(path: Path, *, stage: str) -> dict:
    """Inspect the current stage's bytes, without granting execution authority.

    Workload/source owner validation and actual gate evidence are separate from
    these minimum content-plane records. A successful hash check permits an
    offline frozen copy, but never executing that copy or submitting a job.
    """
    loaded = load_operator_profile(path, stage=stage)
    profile = loaded["profile"]
    order = tuple(REQUIRED_FILES)
    needed = order[:order.index(stage) + 1]
    paths = {}

    def verify_reference(name, row):
        file = Path(_operator_path(row["path"], Path(path).absolute().parent, local=True))
        _file_identity(file.parent, name, dict(row, path=file.name))
        return file

    for plane in needed:
        paths[plane] = verify_reference(plane, profile["release"][plane])
    checked = check_chain(paths, through=stage)
    # The reference must still bind the manifest parsed by check_chain.
    for plane in needed:
        verify_reference(plane, profile["release"][plane])
    harness = None
    if stage == "dispatch":
        from .yolo_bundle import verify_harness, MANIFEST
        reference = profile["evidence"]["harnessManifest"]
        manifest = verify_reference("harnessManifest", reference)
        declared = _read_plane(paths["dispatch"])["files"]["harnessManifest"]
        if (manifest.name != MANIFEST or
                any(declared[key] != reference[key] for key in ("bytes", "sha256"))):
            raise ClosureError("HARNESS_DISPATCH_BINDING")
        harness = verify_harness(manifest.parent, expected_manifest_sha256=reference["sha256"])
        row = _read_plane(paths['dispatch'])['files']['effectiveProfile']
        snapshot_path = paths['dispatch'].parent / row['path']
        _file_identity(paths['dispatch'].parent, 'effectiveProfile', row)
        snapshot = _read_plane(snapshot_path)
        if snapshot != effective_profile_document(profile):
            raise ClosureError('EFFECTIVE_PROFILE_BINDING')
        _file_identity(paths['dispatch'].parent, 'effectiveProfile', row)
        verify_reference("dispatch", profile["release"]["dispatch"])
    result = {"status": "INCOMPLETE", "stage": stage,
            "structure": loaded["structure"], "integrity": checked["integrity"],
            "integrityScope": "declared-content-planes",
            "qualification": "NOT_EVALUATED", "identities": checked["identities"],
            "documentDigest": loaded["documentDigest"],
            "minimumWallTimeSeconds": loaded["minimumWallTimeSeconds"],
            "pending": ["TRANSITIVE_OWNER_VALIDATION", "FROZEN_EXECUTION_WIRING",
                        "WORKLOAD_GATE_VALIDATION"]}
    if harness is not None:
        result["harness"] = harness
    return result


def resolve_provision_inputs(path: Path, *, plan: dict, runtime_candidate_digest: str) -> dict:
    """Resolve the offline issuer inputs without copying keys or launching.

    trustPolicy is the pinned YOLO trust-root registry; descriptor is the
    maintained service configuration template. Catalogue authentication and
    private/public key matching remain with the installed issuer/adapter.
    This mapping is NOT runtime qualification.
    """
    report = check_operator_profile(path, stage="dispatch")
    loaded = load_operator_profile(path, stage="dispatch")
    profile = loaded["profile"]
    if (loaded["documentDigest"] != report["documentDigest"]
            or plan.get("documentDigest") != report["documentDigest"]):
        raise ClosureError("PROVISION_PROFILE_CHANGED")
    if not isinstance(runtime_candidate_digest, str) or not HASH.fullmatch(runtime_candidate_digest):
        raise ClosureError("PROVISION_RUNTIME_CANDIDATE")

    def checked(row, name):
        file = Path(row["path"])
        _file_identity(file.parent, name, dict(row, path=file.name))
        return file

    template = checked(profile["workload"]["descriptor"], "template")
    package_manifest = checked(profile["workload"]["packageManifest"], "packageManifest")
    registry_path = checked(profile["security"]["trustPolicy"], "trustPolicy")
    if package_manifest.name != "manifest.json" or registry_path.parent.name != "contracts":
        raise ClosureError("PROVISION_INPUT_LAYOUT")
    registry = _read_plane(registry_path)
    epoch = profile["security"]["protectionEpoch"]
    policy = registry.get("artifactPolicyAuthority") if isinstance(registry, dict) else None
    if (not isinstance(policy, dict) or not isinstance(policy.get("protectionEpochs"), list)
            or epoch not in policy["protectionEpochs"]):
        raise ClosureError("PROVISION_POLICY_EPOCH")
    public_inputs = {"template.json": dict(profile["workload"]["descriptor"]),
                     "trust/contracts/trust-root-registry-v1.json": dict(profile["security"]["trustPolicy"])}
    for owner in ("catalogue", "modelManifest", "artifactPolicyAuthority"):
        row = registry.get(owner)
        if not isinstance(row, dict):
            raise ClosureError("PROVISION_TRUST_OWNER")
        relative = row.get("publicKeyPath", "")
        if not isinstance(relative, str) or not re.fullmatch(r"contracts/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.pub", relative):
            raise ClosureError("PROVISION_PUBLIC_KEY_PATH")
        key = registry_path.parent.parent / relative
        if (any(p.is_symlink() for p in (key, *key.parents)) or not key.is_file()
                or key.stat().st_size > 65536):
            raise ClosureError("PROVISION_PUBLIC_KEY_FILE")
        identity = _file_identity(key.parent, owner, {"path": key.name, "bytes": key.stat().st_size,
                                                    "sha256": row.get("publicKeySha256")})
        public_inputs["trust/" + relative] = dict(identity, path=str(key))
    private_key = Path(profile["security"]["authorityPrivateKey"])
    try:
        info = private_key.stat()
    except OSError as exc:
        raise ClosureError("PROVISION_PRIVATE_KEY_UNAVAILABLE") from exc
    if (not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077
            or info.st_uid != os.getuid() or not 0 < info.st_size <= 65536):
        raise ClosureError("PROVISION_PRIVATE_KEY_PERMISSIONS")
    manifest = _read_plane(package_manifest)
    catalogue = manifest.get("catalogue") if isinstance(manifest, dict) else None
    if not isinstance(catalogue, dict) or not isinstance(catalogue.get("candidates"), list):
        raise ClosureError("PROVISION_PLACEMENT_CANDIDATE")
    candidates = catalogue["candidates"]
    candidate_id = "shared-backbone-two-shard-v1"
    candidates = [c for c in candidates if isinstance(c, dict) and c.get("candidateId") == candidate_id]
    if (len(candidates) != 1 or not isinstance(candidates[0].get("candidateDigest"), str)
            or not HASH.fullmatch(candidates[0]["candidateDigest"])):
        raise ClosureError("PROVISION_PLACEMENT_CANDIDATE")
    runtime_path = checked(profile["release"]["runtime"], "runtime")
    image = _read_plane(runtime_path)["files"]["sif"]
    # The runtime plane has already validated relative file membership.
    sif = runtime_path.parent / image["path"]
    descriptor = {"schema": "tiger-yolo-prepare-input-v2", "plan": plan,
        "templateDigest": profile["workload"]["descriptor"]["sha256"],
        "manifestDigest": profile["workload"]["packageManifest"]["sha256"],
        "registryDigest": profile["security"]["trustPolicy"]["sha256"],
        "protectionEpoch": epoch, "placementCandidateId": candidate_id,
        "placementCandidateDigest": candidates[0]["candidateDigest"],
        "runtimeCandidateDigest": runtime_candidate_digest}
    return {"qualification": "NOT_EVALUATED", "descriptor": descriptor,
            "publicInputs": public_inputs, "authorityPrivateKey": str(private_key),
            "package": str(package_manifest.parent),
            "runtimeProfile": {"apptainer": profile["runtime"]["apptainer"],
                               "apptainerVersion": profile["runtime"]["apptainerVersion"],
                               "sif": str(sif), "sifSha256": image["sha256"][7:]}}


def resolve_run_plan(path: Path, *, stage: str, case: str, run_id: str, output: Path) -> dict:
    """Resolve a deterministic, non-executable run description for review.

    Candidate E and a qualified immutable bundle are NOT produced here. Node
    placement is an allowed startup layout, never a fabricated DI Selection.
    Runtime argv, mounts and signed material are supplied by downstream owners.
    """
    if not isinstance(run_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]{1,47}", run_id):
        raise ClosureError("RUN_ID")
    loaded = load_operator_profile(path, stage=stage)
    profile = loaded["profile"]
    if case not in profile["cases"]:
        raise ClosureError("RUN_CASE")
    # The operator supplies --output on the command line, so it is a cwd
    # reference (like _safe_output), never a profile-relative reference like
    # the profile's own file rows.
    output = Path(_operator_path(str(output), Path.cwd(), local=True))
    if output == Path(output.anchor):
        raise ClosureError("RUN_OUTPUT_ROOT")
    from .identities import identity_inventory
    from .yolo_worker import assigned_roles, PROVIDER_ROLES, MODEL_ROLES
    namespace = profile["security"]["identityNamespace"] + "/" + run_id
    nodes = []
    count = 1 if case in ("local-cpu", "single-node-gpu") else 2
    for rank in range(count):
        roles = assigned_roles(case, rank)
        nodes.append({"rank": rank, "roles": list(roles),
                      "providerRoles": {role: "cuda:0" if role in MODEL_ROLES and case != "local-cpu"
                                        else "cpu" for role in roles if role in PROVIDER_ROLES}})
    identities = identity_inventory(namespace, {role: namespace + "/" + role
                                   for node in nodes for role in node["roles"]})
    schedule = ({"warmup": 0, "measured": 1} if case == "negative-dependency" else
                profile["schedule"]["singleNode" if count == 1 else "twoNode"])
    requests = [{"index": n, "warmup": n < schedule["warmup"],
                 "requestId": namespace + "/requests/" + hashlib.sha256(
                     ("tiger-yolo-request-v1:" + namespace + "/" + str(n)).encode()).hexdigest()[:32],
                 "output": str(output / run_id / "node0" / "user" / "requests" / str(n))}
                for n in range(schedule["warmup"] + schedule["measured"])]

    behavior = effective_profile_document(profile)['effectiveBehavior']
    case_behavior = {"profile": behavior, "case": case, "nodes": nodes,
                     "schedule": schedule}
    basis = json.dumps(case_behavior, sort_keys=True, separators=(",", ":")).encode()
    return {"schema": "tiger-yolo-run-plan-v1", "status": "PLANNED",
            "qualification": "NOT_EVALUATED", "runId": run_id, "case": case,
            "caseBehaviorDigest": "sha256:" + hashlib.sha256(basis).hexdigest(),
            "documentDigest": loaded["documentDigest"], "effectiveBehavior": case_behavior,
            "namespace": namespace, "applicationName": namespace, "identities": identities, "nodes": nodes,
            "requests": requests, "output": str(output / run_id), "allocation": None,
            "unresolved": ["APPLICATION_ARGV", "IMMUTABLE_BINDINGS", "SIGNED_ROLE_MATERIAL",
                           "ACTUAL_ALLOCATION", "QUALIFIED_CANDIDATE"]}
