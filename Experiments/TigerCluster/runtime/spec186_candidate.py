"""Spec186 candidate/profile closure.

This module is intentionally an offline boundary.  It reads the declared
candidate, validates the effective profile, and performs local ELF/entrypoint
inspection.  It never creates a run directory and never invokes SSH, rsync,
Slurm or a campaign launcher.  The job adapter is the only owner of those
mutations after this gate returns ``ok``.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Dict, Iterable, Mapping, Optional, Sequence, Tuple


SCHEMA_VERSION = "ndnsf-spec186-experiment-profile-v1"
CANDIDATE_SCHEMA = "spec186-candidate-v1"
SPEC186_BASELINE_COMMIT = "575b43cc93bbed29932303caf3d09974f1585af7"
CASES = (
    "yolo-minindn-atomic", "yolo-minindn-normal", "yolo-minindn-negative", "qwen06b-minindn-cpu",
    "yolo-tiger-single-gpu", "yolo-tiger-two-node-normal",
    "yolo-tiger-two-node-negative", "yolo-tiger-two-node-reuse",
    "qwen06b-tiger-experimental",
)
YOLO_ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
YOLO_ATOMIC_ROLES = ("FullModel",)
_HEX = re.compile(r"^[0-9a-f]{64}$")
_IDENT = re.compile(r"^[A-Za-z0-9_.:/-]+$")
_RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
_PROFILE_KEYS = {
    "schemaVersion", "candidate", "case", "topology", "roles", "runtime",
    "model", "workload", "security", "timeouts", "resources", "evidence",
}
_CANDIDATE_KEYS = {"id", "sourceCommit", "sourceSealSha256"}
_MANIFEST_KEYS = {"schemaVersion", "candidateId", "candidateDigest", "source", "runtime",
                  "application", "harness", "configuration", "external", "security", "validation"}
_MANIFEST_SECTION_KEYS = {
    "source": {"commit", "sourceSealSha256"},
    "runtime": {"baseSif", "builder", "abiManifest", "apptainer"},
    "application": {"bundle", "bundleSha256", "entrypoint", "extension", "extensionSha256"},
    "harness": {"launcher", "launcherSha256", "collector", "inputs"},
    "configuration": {"profileSha256", "transportLayoutSha256"},
    "external": {"model", "tokenizer", "stageManifest", "input", "oracle"},
    "security": {"identityRoot", "permissions"},
    "validation": {"contract", "case"},
}
_MANIFEST_ASSET_FIELDS = {
    ("runtime", "baseSif"), ("runtime", "builder"), ("runtime", "abiManifest"),
    ("application", "bundle"), ("harness", "collector"),
    ("external", "model"), ("external", "tokenizer"),
    ("external", "stageManifest"), ("external", "input"), ("external", "oracle"),
    ("security", "permissions"),
}
_MANIFEST_ASSET_KEYS = {"path", "sha256", "label"}
_ASSET_KEYS = {"path", "sha256"}
_APPTAINER_KEYS = {"path", "version"}
_TOPOLOGY_KEYS = {"mode", "nodes", "hosts", "nfdEndpoints"}
_ENDPOINT_KEYS = {"host", "port"}
_YOLO_ROLE_KEYS = {
    "name", "identity", "node", "gpu", "backend", "service", "binary",
    "allowCpuFallback",
}
_QWEN_ROLE_KEYS = {
    "name", "identity", "node", "gpu", "backend", "service", "binary",
    "allowCpuFallback", "stage",
    "dependencies", "artifactSha256", "tokenizerSha256", "modelUri",
}
_APPLICATION_KEYS = {"bundle", "bundleSha256", "entrypoint", "extension"}
_HARNESS_KEYS = {"launcher", "collector", "inputs"}
_YOLO_HARNESS_INPUT_KEYS = {"caseBundle", "catalogueDataName", "catalogueSigner"}
_MODEL_KEYS = {"family", "format", "backend", "model", "tokenizer", "stageManifest"}
_WORKLOAD_KEYS = {"input", "oracle", "warmup", "measured", "requestMode"}
_SECURITY_KEYS = {"identityRoot", "permissions"}
_TIMEOUT_KEYS = {"startupSeconds", "requestSeconds", "completionSeconds", "cleanupSeconds"}
_RESOURCE_KEYS = {"account", "partition", "cpusPerNode", "memory", "gpus", "gpu"}
_GPU_KEYS = {"uuid", "freeMemoryMb", "signature"}
_EVIDENCE_KEYS = {"root", "collector"}
_RUNTIME_KEYS = {
    "baseSif", "builder", "abiManifest", "allowCpuFallback", "apptainer",
    "application", "harness",
}
APPTAINER_POLICY_VERSION = "1.5.3"


class CandidateError(ValueError):
    """A fail-closed profile or candidate error."""


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def canonical_digest(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_digest(path: Path) -> str:
    """Hash an immutable application directory by relative file and content."""
    root = Path(path)
    if not root.is_dir():
        raise OSError("not a directory: " + str(root))
    digest = hashlib.sha256()
    # The manifest stores this digest, so exclude it from the content set and
    # avoid an impossible self-referential hash.
    files = sorted(item for item in root.rglob("*")
                   if item.is_file() and item.name != "bundle-manifest.json")
    for item in files:
        relative = item.relative_to(root).as_posix().encode("utf-8")
        digest.update(relative)
        digest.update(b"\0")
        digest.update(bytes.fromhex(file_digest(item)))
        digest.update(b"\n")
    return digest.hexdigest()


def asset_digest(path: Path, *, allow_directory: bool = False) -> str:
    if path.is_file():
        return file_digest(path)
    if allow_directory and path.is_dir():
        return tree_digest(path)
    raise OSError("asset is neither a file nor an allowed directory: " + str(path))


def _duplicate_pairs(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CandidateError("DUPLICATE_JSON_FIELD:" + key)
        result[key] = value
    return result


def load_json(path: Path) -> Dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=_duplicate_pairs)
    except CandidateError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CandidateError("JSON_READ_FAILED:" + str(path)) from exc
    if not isinstance(value, dict):
        raise CandidateError("JSON_ROOT_NOT_OBJECT:" + str(path))
    return value


def _replace_tokens(value: Any, repo_root: Path, run_root: Optional[Path]) -> Any:
    if isinstance(value, str):
        value = value.replace("${REPO_ROOT}", str(repo_root))
        if "${RUN_ROOT}" in value:
            if run_root is None:
                raise CandidateError("RUN_ROOT_REQUIRED")
            value = value.replace("${RUN_ROOT}", str(run_root))
        return value
    if isinstance(value, list):
        return [_replace_tokens(item, repo_root, run_root) for item in value]
    if isinstance(value, dict):
        return {key: _replace_tokens(item, repo_root, run_root)
                for key, item in value.items()}
    return value


def _keys(value: Mapping[str, Any], expected: Iterable[str], label: str) -> None:
    unknown = sorted(set(value) - set(expected))
    if unknown:
        raise CandidateError("UNKNOWN_" + label + "_FIELD:" + ",".join(unknown))


def _required(value: Mapping[str, Any], fields: Iterable[str], label: str) -> None:
    missing = sorted(set(fields) - set(value))
    if missing:
        raise CandidateError("MISSING_" + label + "_FIELD:" + ",".join(missing))


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise CandidateError("INVALID_STRING:" + label)
    return value


def _digest(value: Any, label: str, *, allow_none: bool = False) -> Optional[str]:
    if allow_none and value is None:
        return None
    if not isinstance(value, str) or not _HEX.fullmatch(value):
        raise CandidateError("INVALID_DIGEST:" + label)
    return value


def _verify_source_lineage(source_commit: str, repo_root: Path) -> None:
    """Require the candidate source to descend from the Spec186 baseline."""
    # Tiger's replay archive intentionally omits .git.  The submit-side
    # candidate gate verifies lineage before staging; the git-less runtime
    # must consume that sealed source identity without attempting host VCS
    # discovery.
    # Callers commonly pass the TigerCluster subtree rather than the project
    # root.  Walk parents so the local static gate cannot silently skip the
    # check just because the profile lives below the repository root.
    git_root = next(
        (candidate for candidate in (repo_root, *repo_root.parents)
         if (candidate / ".git").exists()),
        None,
    )
    if git_root is None:
        return
    try:
        subprocess.check_call(
            ["git", "-C", str(git_root), "merge-base", "--is-ancestor",
             SPEC186_BASELINE_COMMIT, source_commit],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except subprocess.CalledProcessError as exc:
        raise CandidateError("SOURCE_BASELINE_LINEAGE") from exc
    except (OSError, subprocess.SubprocessError) as exc:
        raise CandidateError("SOURCE_LINEAGE_UNVERIFIABLE") from exc


def _path(value: Any, label: str) -> str:
    path = _string(value, label)
    if not Path(path).is_absolute() or "\x00" in path:
        raise CandidateError("PATH_NOT_ABSOLUTE:" + label)
    return path


def _asset(value: Any, label: str, *, optional: bool = False) -> Dict[str, Any]:
    if optional and value is None:
        return {"path": None, "sha256": None}
    if not isinstance(value, Mapping):
        raise CandidateError("ASSET_NOT_OBJECT:" + label)
    _keys(value, _ASSET_KEYS, label.upper())
    _required(value, _ASSET_KEYS, label.upper())
    return {"path": _path(value["path"], label + ".path"),
            "sha256": _digest(value["sha256"], label + ".sha256")}


def _positive_int(value: Any, label: str, maximum: int = 86400000) -> int:
    if type(value) is not int or not 0 < value <= maximum:
        raise CandidateError("INVALID_INTEGER:" + label)
    return value


def load_profile(path: Path, *, repo_root: Optional[Path] = None,
                 run_root: Optional[Path] = None) -> Dict[str, Any]:
    repo = (repo_root or Path(__file__).resolve().parents[3]).resolve()
    profile = _replace_tokens(load_json(Path(path)), repo, run_root)
    _keys(profile, _PROFILE_KEYS, "PROFILE")
    _required(profile, _PROFILE_KEYS, "PROFILE")
    if profile["schemaVersion"] != SCHEMA_VERSION:
        raise CandidateError("PROFILE_SCHEMA_UNSUPPORTED")
    if profile["case"] not in CASES:
        raise CandidateError("UNKNOWN_CASE:" + str(profile["case"]))

    candidate = profile["candidate"]
    if not isinstance(candidate, Mapping):
        raise CandidateError("CANDIDATE_NOT_OBJECT")
    _keys(candidate, _CANDIDATE_KEYS, "CANDIDATE")
    _required(candidate, _CANDIDATE_KEYS, "CANDIDATE")
    if not _IDENT.fullmatch(_string(candidate["id"], "candidate.id")):
        raise CandidateError("INVALID_IDENTIFIER:candidate.id")
    if not re.fullmatch(r"[0-9a-f]{40}", _string(candidate["sourceCommit"],
                                                    "candidate.sourceCommit")):
        raise CandidateError("INVALID_COMMIT:candidate.sourceCommit")
    _verify_source_lineage(candidate["sourceCommit"], repo)
    _digest(candidate["sourceSealSha256"], "candidate.sourceSealSha256")

    topology = profile["topology"]
    if not isinstance(topology, Mapping):
        raise CandidateError("TOPOLOGY_NOT_OBJECT")
    _keys(topology, _TOPOLOGY_KEYS, "TOPOLOGY")
    _required(topology, ("mode", "nodes", "hosts", "nfdEndpoints"), "TOPOLOGY")
    if topology["mode"] not in {"minindn", "tiger"}:
        raise CandidateError("TOPOLOGY_MODE")
    if type(topology["nodes"]) is not int or topology["nodes"] not in (1, 2):
        raise CandidateError("TOPOLOGY_NODES")
    if not isinstance(topology["hosts"], list) or len(topology["hosts"]) != topology["nodes"]:
        raise CandidateError("TOPOLOGY_HOSTS")
    hosts = [_string(item, "topology.host") for item in topology["hosts"]]
    if len(set(hosts)) != len(hosts):
        raise CandidateError("TOPOLOGY_HOSTS_NOT_DISTINCT")
    endpoints = topology["nfdEndpoints"]
    if not isinstance(endpoints, list) or len(endpoints) != topology["nodes"]:
        raise CandidateError("TOPOLOGY_NFD_ENDPOINTS")
    endpoint_hosts = []
    for endpoint in endpoints:
        if not isinstance(endpoint, Mapping):
            raise CandidateError("NFD_ENDPOINT_NOT_OBJECT")
        _keys(endpoint, _ENDPOINT_KEYS, "NFD_ENDPOINT")
        _required(endpoint, ("host", "port"), "NFD_ENDPOINT")
        endpoint_host = _string(endpoint["host"], "nfdEndpoint.host")
        endpoint_hosts.append(endpoint_host)
        if endpoint_host not in hosts or type(endpoint["port"]) is not int or not 1024 <= endpoint["port"] <= 65535:
            raise CandidateError("NFD_ENDPOINT_INVALID")
    if len(set(endpoint_hosts)) != len(endpoint_hosts):
        raise CandidateError("NFD_ENDPOINT_HOSTS_NOT_DISTINCT")
    if profile["case"].startswith("yolo-"):
        roles = profile["roles"]
        expected_roles = (YOLO_ATOMIC_ROLES if profile["case"] in {
                              "yolo-minindn-atomic", "yolo-tiger-single-gpu"}
                          else YOLO_ROLES)
        if not isinstance(roles, list) or [item.get("name") for item in roles if isinstance(item, Mapping)] != list(expected_roles):
            raise CandidateError("YOLO_ROLE_ORDER")
        for role in roles:
            if not isinstance(role, Mapping):
                raise CandidateError("ROLE_NOT_OBJECT")
            _keys(role, _YOLO_ROLE_KEYS, "ROLE")
            _required(role, ("name", "identity", "node", "gpu", "backend", "service", "binary",
                             "allowCpuFallback"), "ROLE")
            if not _IDENT.fullmatch(_string(role["identity"], "role.identity")):
                raise CandidateError("ROLE_IDENTITY")
            if role["node"] not in hosts:
                raise CandidateError("ROLE_NODE")
            if type(role["gpu"]) is not int or role["gpu"] < -1 or role["gpu"] > 15:
                raise CandidateError("ROLE_GPU")
            if type(role["allowCpuFallback"]) is not bool:
                raise CandidateError("ROLE_CPU_FALLBACK")
            _string(role["node"], "role.node")
            backend = _string(role["backend"], "role.backend")
            service = _string(role["service"], "role.service")
            if service != "/AI/YOLO/YOLO26n/" + role["name"]:
                raise CandidateError("ROLE_SERVICE")
            if (role["gpu"] >= 0) != ("cuda" in backend.lower()):
                raise CandidateError("ROLE_BACKEND_GPU_MISMATCH")
            expected_cuda = (topology["mode"] == "tiger" and
                             role["name"] != "Merge")
            if (role["gpu"] >= 0) != expected_cuda:
                raise CandidateError("YOLO_ROLE_GPU_POLICY")
            expected_backend = "onnxruntime-cuda" if expected_cuda else "onnxruntime-cpu"
            if backend != expected_backend:
                raise CandidateError("YOLO_ROLE_BACKEND_POLICY")
            _path(role["binary"], "role.binary")
        if len({role["identity"] for role in roles}) != len(roles):
            raise CandidateError("ROLE_IDENTITIES_NOT_DISTINCT")
    elif not isinstance(profile["roles"], list) or not profile["roles"]:
        raise CandidateError("QWEN_ROLES_EMPTY")
    else:
        stages = [role.get("stage") for role in profile["roles"] if isinstance(role, Mapping)]
        if stages != list(range(len(profile["roles"]))):
            raise CandidateError("QWEN_STAGE_ORDER")
        for role in profile["roles"]:
            if not isinstance(role, Mapping):
                raise CandidateError("ROLE_NOT_OBJECT")
            _keys(role, _QWEN_ROLE_KEYS, "ROLE")
            _required(role, ("name", "identity", "node", "gpu", "backend", "service", "binary",
                             "allowCpuFallback", "stage", "dependencies", "artifactSha256",
                             "tokenizerSha256", "modelUri"), "ROLE")
            for field in ("name", "identity", "node", "backend", "service", "modelUri"):
                _string(role[field], "role." + field)
            if role["node"] not in hosts:
                raise CandidateError("ROLE_NODE")
            if type(role["gpu"]) is not int or role["gpu"] < -1 or role["gpu"] > 15:
                raise CandidateError("ROLE_GPU")
            if type(role["allowCpuFallback"]) is not bool:
                raise CandidateError("ROLE_CPU_FALLBACK")
            if type(role["stage"]) is not int or role["stage"] < 0:
                raise CandidateError("QWEN_STAGE")
            if role["name"] != "Stage" + str(role["stage"]):
                raise CandidateError("QWEN_STAGE_NAME")
            if role["service"] != "/LLM/Pipeline/Stage/" + str(role["stage"]):
                raise CandidateError("QWEN_STAGE_SERVICE")
            expected_dependencies = [] if role["stage"] == 0 else ["Stage" + str(role["stage"] - 1)]
            if role["dependencies"] != expected_dependencies:
                raise CandidateError("QWEN_DEPENDENCY_ORDER")
            if (role["gpu"] >= 0) != ("cuda" in role["backend"].lower()):
                raise CandidateError("ROLE_BACKEND_GPU_MISMATCH")
            _path(role["binary"], "role.binary")
            if not isinstance(role["dependencies"], list):
                raise CandidateError("QWEN_DEPENDENCIES")
            if any(not isinstance(item, str) or not item for item in role["dependencies"]):
                raise CandidateError("QWEN_DEPENDENCY_NAME")
            _digest(role["artifactSha256"], "role.artifactSha256")
            _digest(role["tokenizerSha256"], "role.tokenizerSha256")
            _string(role["modelUri"], "role.modelUri")
        if len({role["identity"] for role in profile["roles"]}) != len(profile["roles"]):
            raise CandidateError("ROLE_IDENTITIES_NOT_DISTINCT")

    runtime = profile["runtime"]
    if not isinstance(runtime, Mapping):
        raise CandidateError("RUNTIME_NOT_OBJECT")
    _keys(runtime, _RUNTIME_KEYS, "RUNTIME")
    _required(runtime, ("baseSif", "builder", "abiManifest", "allowCpuFallback", "apptainer"), "RUNTIME")
    for key in ("baseSif", "builder", "abiManifest"):
        _asset(runtime[key], "runtime." + key)
    if type(runtime["allowCpuFallback"]) is not bool:
        raise CandidateError("RUNTIME_CPU_FALLBACK")
    if topology["mode"] == "tiger" and runtime["allowCpuFallback"]:
        raise CandidateError("TIGER_CPU_FALLBACK_FORBIDDEN")
    if any(role["allowCpuFallback"] != runtime["allowCpuFallback"]
           for role in profile["roles"]):
        raise CandidateError("ROLE_RUNTIME_CPU_FALLBACK_MISMATCH")
    if topology["mode"] == "tiger" and any(
            role["allowCpuFallback"] for role in profile["roles"]):
        raise CandidateError("TIGER_ROLE_CPU_FALLBACK_FORBIDDEN")
    apptainer = runtime["apptainer"]
    if not isinstance(apptainer, Mapping):
        raise CandidateError("APPTAINER_NOT_OBJECT")
    _keys(apptainer, _APPTAINER_KEYS, "APPTAINER")
    _required(apptainer, _APPTAINER_KEYS, "APPTAINER")
    _path(apptainer["path"], "runtime.apptainer.path")
    if apptainer["version"] != APPTAINER_POLICY_VERSION:
        raise CandidateError("APPTAINER_VERSION_POLICY")
    application = runtime.get("application")
    if not isinstance(application, Mapping):
        raise CandidateError("APPLICATION_NOT_OBJECT")
    _keys(application, _APPLICATION_KEYS, "APPLICATION")
    _required(application, ("bundle", "bundleSha256", "entrypoint", "extension"), "APPLICATION")
    _asset(application["bundle"], "runtime.application.bundle")
    _digest(application["bundleSha256"], "runtime.application.bundleSha256")
    if application["bundle"]["sha256"] != application["bundleSha256"]:
        raise CandidateError("APPLICATION_BUNDLE_DIGEST_MISMATCH")
    _path(application["entrypoint"], "runtime.application.entrypoint")
    _path(application["extension"], "runtime.application.extension")
    harness = runtime.get("harness")
    if not isinstance(harness, Mapping):
        raise CandidateError("HARNESS_NOT_OBJECT")
    _keys(harness, _HARNESS_KEYS, "HARNESS")
    _required(harness, ("launcher", "collector"), "HARNESS")
    _path(harness["launcher"], "runtime.harness.launcher")
    _asset(harness["collector"], "runtime.harness.collector")
    if profile["case"].startswith("yolo-"):
        inputs = harness.get("inputs")
        if not isinstance(inputs, Mapping):
            raise CandidateError("HARNESS_INPUTS_NOT_OBJECT")
        _keys(inputs, _YOLO_HARNESS_INPUT_KEYS, "HARNESS_INPUTS")
        _required(inputs, _YOLO_HARNESS_INPUT_KEYS, "HARNESS_INPUTS")
        _asset(inputs["caseBundle"], "runtime.harness.inputs.caseBundle")
        for key in ("catalogueDataName", "catalogueSigner"):
            _string(inputs[key], "runtime.harness.inputs." + key)
        if "/NDNSF/DI/" not in inputs["catalogueDataName"]:
            raise CandidateError("HARNESS_CATALOGUE_DATA_NAME")
        if inputs["catalogueSigner"] != inputs["catalogueDataName"].split("/NDNSF/DI/", 1)[0]:
            raise CandidateError("HARNESS_CATALOGUE_SIGNER")

    model = profile["model"]
    if not isinstance(model, Mapping):
        raise CandidateError("MODEL_NOT_OBJECT")
    _keys(model, _MODEL_KEYS, "MODEL")
    _required(model, ("family", "format", "backend", "model", "tokenizer"), "MODEL")
    for key in ("family", "format", "backend"):
        _string(model[key], "model." + key)
    _asset(model["model"], "model.model")
    _asset(model["tokenizer"], "model.tokenizer", optional=True)
    if profile["case"].startswith("qwen") and model["family"] != "Qwen3-0.6B":
        raise CandidateError("QWEN_MODEL_FAMILY_MISMATCH")
    if profile["case"].startswith("qwen"):
        _asset(model.get("stageManifest"), "model.stageManifest")
    elif model.get("stageManifest") is not None:
        raise CandidateError("YOLO_STAGE_MANIFEST_UNEXPECTED")
    if profile["case"].startswith("yolo") and model["family"] != "YOLO26n":
        raise CandidateError("YOLO_MODEL_FAMILY_MISMATCH")
    if profile["case"] == "qwen06b-tiger-experimental" and model["format"] not in {"onnx", "gguf-q3"}:
        raise CandidateError("QWEN_FORMAT_UNSUPPORTED")
    if profile["case"].startswith("qwen"):
        if model["format"] == "gguf-q3" and not model["backend"].startswith("llama.cpp"):
            raise CandidateError("QWEN_FORMAT_BACKEND_MISMATCH")
        if model["format"] == "onnx" and model["backend"] not in {"onnxruntime-cpu", "onnxruntime-cuda"}:
            raise CandidateError("QWEN_FORMAT_BACKEND_MISMATCH")
        if any(role["backend"] != model["backend"] for role in profile["roles"]):
            raise CandidateError("QWEN_ROLE_MODEL_BACKEND_MISMATCH")

    workload = profile["workload"]
    if not isinstance(workload, Mapping):
        raise CandidateError("WORKLOAD_NOT_OBJECT")
    _keys(workload, _WORKLOAD_KEYS, "WORKLOAD")
    _required(workload, ("input", "oracle", "warmup", "measured", "requestMode"), "WORKLOAD")
    _asset(workload["input"], "workload.input")
    _asset(workload["oracle"], "workload.oracle")
    for key in ("warmup", "measured"):
        _positive_int(workload[key], "workload." + key, 1000)
    if workload["requestMode"] not in {"normal", "negative"}:
        raise CandidateError("WORKLOAD_REQUEST_MODE")
    if profile["case"].endswith("negative") and workload["requestMode"] != "negative":
        raise CandidateError("NEGATIVE_REQUEST_MODE")

    security = profile["security"]
    if not isinstance(security, Mapping):
        raise CandidateError("SECURITY_NOT_OBJECT")
    _keys(security, _SECURITY_KEYS, "SECURITY")
    _required(security, ("identityRoot", "permissions"), "SECURITY")
    _path(security["identityRoot"], "security.identityRoot")
    _asset(security["permissions"], "security.permissions")

    timeouts = profile["timeouts"]
    if not isinstance(timeouts, Mapping):
        raise CandidateError("TIMEOUTS_NOT_OBJECT")
    _keys(timeouts, _TIMEOUT_KEYS, "TIMEOUTS")
    _required(timeouts, ("startupSeconds", "requestSeconds", "completionSeconds", "cleanupSeconds"), "TIMEOUTS")
    for key in ("startupSeconds", "requestSeconds", "completionSeconds", "cleanupSeconds"):
        _positive_int(timeouts[key], "timeouts." + key, 86400)

    resources = profile["resources"]
    if not isinstance(resources, Mapping):
        raise CandidateError("RESOURCES_NOT_OBJECT")
    _keys(resources, _RESOURCE_KEYS, "RESOURCES")
    _required(resources, ("account", "partition", "cpusPerNode", "memory", "gpus"), "RESOURCES")
    for key in ("account", "partition", "memory"):
        resource_value = _string(resources[key], "resources." + key)
        pattern = r"[A-Za-z0-9_.:-]+" if key != "memory" else r"[1-9][0-9]*(?:[KMG])?"
        if not re.fullmatch(pattern, resource_value):
            raise CandidateError("INVALID_RESOURCE:" + key)
    _positive_int(resources["cpusPerNode"], "resources.cpusPerNode", 256)
    if type(resources["gpus"]) is not int or resources["gpus"] < 0 or resources["gpus"] > 16:
        raise CandidateError("INVALID_RESOURCE:gpus")
    role_gpus = [role["gpu"] for role in profile["roles"]
                 if isinstance(role, Mapping) and type(role.get("gpu")) is int
                 and role["gpu"] >= 0]
    if bool(role_gpus) != bool(resources["gpus"]):
        raise CandidateError("GPU_ROLE_RESOURCE_MISMATCH")
    if resources["gpus"] and len(set(role_gpus)) > resources["gpus"]:
        raise CandidateError("GPU_RESOURCE_COUNT_TOO_SMALL")
    if not resources["gpus"] and "gpu" in resources:
        raise CandidateError("GPU_CAPACITY_UNEXPECTED")
    if resources["gpus"]:
        gpu = resources.get("gpu")
        if not isinstance(gpu, Mapping):
            raise CandidateError("GPU_CAPACITY_UNDECLARED")
        _keys(gpu, _GPU_KEYS, "GPU")
        _required(gpu, ("uuid", "freeMemoryMb", "signature"), "GPU")
        gpu_uuid = _string(gpu["uuid"], "gpu.uuid")
        if not re.fullmatch(r"GPU-[A-Za-z0-9-]+", gpu_uuid):
            raise CandidateError("GPU_UUID")
        _positive_int(gpu["freeMemoryMb"], "gpu.freeMemoryMb", 1000000)
        _digest(gpu["signature"], "gpu.signature")

    evidence = profile["evidence"]
    if not isinstance(evidence, Mapping):
        raise CandidateError("EVIDENCE_NOT_OBJECT")
    _keys(evidence, _EVIDENCE_KEYS, "EVIDENCE")
    _required(evidence, ("root", "collector"), "EVIDENCE")
    _path(evidence["root"], "evidence.root")
    _asset(evidence["collector"], "evidence.collector")
    return profile


def _asset_manifest(asset: Mapping[str, Any], label: str) -> Dict[str, Any]:
    if asset is None:
        return {"path": None, "sha256": None, "label": label}
    return {"path": asset["path"], "sha256": asset["sha256"], "label": label}


def build_candidate_manifest(profile: Mapping[str, Any], *, repo_root: Path) -> Dict[str, Any]:
    """Build a deterministic tuple; missing bytes remain a pre-dispatch error."""
    runtime = profile["runtime"]
    model = profile["model"]
    workload = profile["workload"]
    security = profile["security"]
    launcher_path = Path(runtime["harness"]["launcher"])
    launcher_sha = file_digest(launcher_path) if launcher_path.is_file() else "0" * 64
    extension_path = Path(runtime["application"]["extension"])
    extension_sha = file_digest(extension_path) if extension_path.is_file() else "0" * 64
    harness_inputs = runtime["harness"].get("inputs", {})
    manifest = {
        "schemaVersion": CANDIDATE_SCHEMA,
        "candidateId": profile["candidate"]["id"],
        "source": {"commit": profile["candidate"]["sourceCommit"],
                    "sourceSealSha256": profile["candidate"]["sourceSealSha256"]},
        "runtime": {"baseSif": _asset_manifest(runtime["baseSif"], "baseSif"),
                    "builder": _asset_manifest(runtime["builder"], "builder"),
                    "abiManifest": _asset_manifest(runtime["abiManifest"], "abiManifest"),
                    "apptainer": dict(runtime["apptainer"])},
        "application": {"bundle": _asset_manifest(runtime["application"]["bundle"], "application.bundle"),
                        "bundleSha256": runtime["application"]["bundleSha256"],
                        "entrypoint": runtime["application"]["entrypoint"],
                        "extension": runtime["application"]["extension"],
                        "extensionSha256": extension_sha},
        "harness": {"launcher": runtime["harness"]["launcher"],
                     "launcherSha256": launcher_sha,
                    "collector": _asset_manifest(runtime["harness"]["collector"], "harness.collector"),
                    "inputs": {
                        "caseBundle": _asset_manifest(
                            harness_inputs["caseBundle"],
                            "harness.inputs.caseBundle"),
                        "catalogueDataName": harness_inputs.get("catalogueDataName"),
                        "catalogueSigner": harness_inputs.get("catalogueSigner"),
                    } if profile["case"].startswith("yolo-") else {}},
        "configuration": {"profileSha256": canonical_digest(profile),
                          "transportLayoutSha256": canonical_digest(profile["topology"])},
        "external": {"model": _asset_manifest(model["model"], "model"),
                     "tokenizer": _asset_manifest(model["tokenizer"], "tokenizer"),
                     "stageManifest": _asset_manifest(model.get("stageManifest"), "stageManifest"),
                     "input": _asset_manifest(workload["input"], "input"),
                     "oracle": _asset_manifest(workload["oracle"], "oracle")},
        "security": {"identityRoot": security["identityRoot"],
                      "permissions": _asset_manifest(security["permissions"], "permissions")},
        "validation": {"contract": CANDIDATE_SCHEMA, "case": profile["case"]},
    }
    manifest["candidateDigest"] = canonical_digest(manifest)
    return manifest


def _validate_manifest_shape(manifest: Mapping[str, Any], failures: list[str]) -> None:
    _keys(manifest, _MANIFEST_KEYS, "CANDIDATE_MANIFEST")
    _required(manifest, _MANIFEST_KEYS, "CANDIDATE_MANIFEST")
    if (not isinstance(manifest.get("candidateDigest"), str) or
            not _HEX.fullmatch(manifest["candidateDigest"])):
        failures.append("CANDIDATE_DIGEST_INVALID")
    for section, expected_keys in _MANIFEST_SECTION_KEYS.items():
        value = manifest.get(section)
        if not isinstance(value, Mapping):
            failures.append("CANDIDATE_SECTION_NOT_OBJECT:" + section)
            continue
        unknown = sorted(set(value) - expected_keys)
        if unknown:
            failures.append("UNKNOWN_CANDIDATE_" + section.upper() + "_FIELD:" + ",".join(unknown))
        missing = sorted(expected_keys - set(value))
        if missing:
            failures.append("MISSING_CANDIDATE_" + section.upper() + "_FIELD:" + ",".join(missing))
        for key in expected_keys:
            if (section, key) not in _MANIFEST_ASSET_FIELDS:
                continue
            asset = value.get(key)
            if asset is None and (section, key) == ("external", "stageManifest"):
                continue
            if not isinstance(asset, Mapping):
                failures.append("CANDIDATE_ASSET_NOT_OBJECT:" + section + "." + key)
                continue
            asset_unknown = sorted(set(asset) - _MANIFEST_ASSET_KEYS)
            if asset_unknown:
                failures.append("UNKNOWN_CANDIDATE_ASSET_FIELD:" + section + "." + key + ":" + ",".join(asset_unknown))
            asset_missing = sorted(_MANIFEST_ASSET_KEYS - set(asset))
            if asset_missing:
                failures.append("MISSING_CANDIDATE_ASSET_FIELD:" + section + "." + key + ":" + ",".join(asset_missing))
        if section == "harness":
            inputs = value.get("inputs")
            if not isinstance(inputs, Mapping):
                failures.append("CANDIDATE_HARNESS_INPUTS_NOT_OBJECT")
                continue
            if inputs and set(inputs) != _YOLO_HARNESS_INPUT_KEYS:
                failures.append("CANDIDATE_HARNESS_INPUTS_INVALID")
            if inputs:
                case_bundle = inputs.get("caseBundle")
                if not isinstance(case_bundle, Mapping):
                    failures.append("CANDIDATE_HARNESS_CASE_BUNDLE_NOT_OBJECT")
                else:
                    unknown = sorted(set(case_bundle) - _MANIFEST_ASSET_KEYS)
                    missing = sorted(_MANIFEST_ASSET_KEYS - set(case_bundle))
                    if unknown:
                        failures.append("UNKNOWN_CANDIDATE_ASSET_FIELD:harness.inputs.caseBundle:" + ",".join(unknown))
                    if missing:
                        failures.append("MISSING_CANDIDATE_ASSET_FIELD:harness.inputs.caseBundle:" + ",".join(missing))


def _profile_placeholder_checks(profile: Mapping[str, Any], failures: list[str]) -> None:
    """Reject allocation templates before they can reach a scheduler."""
    resources = profile["resources"]
    for key in ("account", "partition"):
        if str(resources[key]).upper() in {"REPLACE_ME", "TODO", "TBD"}:
            failures.append("RESOURCE_PLACEHOLDER:" + key)
    gpu = resources.get("gpu")
    if isinstance(gpu, Mapping):
        if str(gpu.get("uuid", "")).upper() in {"GPU-REPLACE-ME", "GPU-TODO", "GPU-TBD"}:
            failures.append("GPU_UUID_PLACEHOLDER")
        if gpu.get("signature") == "0" * 64:
            failures.append("GPU_SIGNATURE_PLACEHOLDER")
    application_bundle = profile["runtime"]["application"]["bundle"]["path"]
    for role in profile["roles"]:
        if role.get("binary") != application_bundle:
            failures.append("ROLE_BINARY_BIND_MISMATCH:" + str(role.get("name")))
        if profile["case"].startswith("qwen"):
            if role.get("artifactSha256") == "0" * 64:
                failures.append("PLACEHOLDER_DIGEST:role.artifactSha256." + str(role.get("name")))
            if role.get("tokenizerSha256") == "0" * 64:
                failures.append("PLACEHOLDER_DIGEST:role.tokenizerSha256." + str(role.get("name")))


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _check_asset(asset: Any, label: str, failures: list[str],
                 *, allow_directory: bool = False) -> None:
    if not isinstance(asset, Mapping):
        failures.append("ASSET_NOT_OBJECT:" + label)
        return
    path = asset.get("path")
    expected = asset.get("sha256")
    if path is None:
        if expected is not None:
            failures.append("OPTIONAL_DIGEST:" + label)
        return
    if not isinstance(path, str) or not path or not Path(path).is_absolute() or "\x00" in path:
        failures.append("INVALID_ASSET_PATH:" + label)
        return
    if not isinstance(expected, str) or not _HEX.fullmatch(expected):
        failures.append("INVALID_DIGEST:" + label)
        return
    target = Path(path)
    if expected == "0" * 64:
        failures.append("PLACEHOLDER_DIGEST:" + label)
    if not target.is_file() and not (allow_directory and target.is_dir()):
        failures.append("FILE_MISSING:" + label)
        return
    try:
        actual = asset_digest(target, allow_directory=allow_directory)
        if actual != expected:
            failures.append("FILE_DIGEST_MISMATCH:" + label)
    except OSError:
        failures.append("FILE_READ_FAILED:" + label)


def _check_declared_file(path: Any, expected: Any, label: str,
                         failures: list[str]) -> None:
    if not isinstance(path, str) or not Path(path).is_file():
        failures.append("FILE_MISSING:" + label)
        return
    if expected == "0" * 64:
        failures.append("PLACEHOLDER_DIGEST:" + label)
        return
    if not isinstance(expected, str) or not _HEX.fullmatch(expected):
        failures.append("INVALID_DIGEST:" + label)
        return
    try:
        if file_digest(Path(path)) != expected:
            failures.append("FILE_DIGEST_MISMATCH:" + label)
    except OSError:
        failures.append("FILE_READ_FAILED:" + label)


def _harness_checks(profile: Mapping[str, Any], failures: list[str]) -> None:
    """Catch profile/entrypoint contract drift before any dispatch mutation."""
    launcher = Path(profile["runtime"]["harness"]["launcher"])
    if not launcher.is_file():
        return
    try:
        source = launcher.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        failures.append("HARNESS_SOURCE_READ_FAILED")
        return
    # The maintained Spec180 runner is package/config driven and requires the
    # canonical YOLO26n package.  Keep that contract explicit in each profile
    # so a command can be rendered before MiniNDN or a scheduler is started.
    if profile["case"].startswith("yolo-") and "SPEC180_YOLO_CANONICAL_PACKAGE" in source:
        if profile["model"]["family"] != "YOLO26n":
            failures.append("HARNESS_MODEL_FAMILY_MISMATCH:expected-YOLO26n")
        inputs = profile["runtime"]["harness"].get("inputs")
        if not isinstance(inputs, Mapping) or not isinstance(inputs.get("caseBundle"), Mapping):
            failures.append("HARNESS_ENVIRONMENT_UNDECLARED:caseBundle")
        else:
            required = ("NDNSF_DI_STATE_ROOT", "NDNSF_DI_ENVELOPE_KEY_FILE",
                        "SPEC180_YOLO_CANONICAL_PACKAGE", "SPEC180_YOLO_CATALOGUE_REGISTRY",
                        "SPEC180_YOLO_CATALOG_DATA_NAME", "SPEC180_YOLO_CATALOG_SIGNER",
                        "SPEC180_YOLO_OFFER_TRUST_ROOT", "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP",
                        "SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP", "SPEC180_YOLO_TOPOLOGY",
                        "SPEC180_YOLO_CONFIG")
            # The two run-owned paths are rendered from run_root; the remaining
            # values are derived from the immutable case bundle below.
            if not all(inputs.get(key) for key in ("catalogueDataName", "catalogueSigner")):
                failures.append("HARNESS_ENVIRONMENT_UNDECLARED:" + ",".join(required))
    if profile["case"] == "qwen06b-minindn-cpu":
        # The maintained Qwen entrypoint is an ONNX/onnxruntime native
        # harness.  A GGUF/Q3 profile cannot be passed to it by merely
        # renaming the stage-manifest argument; doing so would produce a
        # misleading process-start failure instead of a model qualification.
        if (profile["model"]["format"] != "onnx" or
                profile["model"]["backend"] != "onnxruntime-cpu"):
            failures.append("HARNESS_MODEL_CONTRACT_MISMATCH:expected-onnxruntime-cpu")
        required = ("canonical-source", "topology", "build", "controller-binary",
                    "input-token-ids", "delta-token-ids")
        failures.append("HARNESS_QWEN_INPUTS_UNDECLARED:" + ",".join(required))


def _apptainer_checks(profile: Mapping[str, Any], failures: list[str],
                      commands: list[list[str]]) -> None:
    """Require the declared runtime on local SIF execution paths.

    Tiger profiles are submitted from a login/control node; that node is never
    an NDNSF-DI SIF execution target. Their declared compute binary is therefore
    checked by the compute preflight, while local MiniNDN/SIF paths verify the
    local 1.5.3 executable here.
    """
    runtime = profile["runtime"]
    apptainer = runtime["apptainer"]
    if profile["topology"]["mode"] != "minindn":
        return
    target = Path(apptainer["path"])
    if not target.is_file() or not os.access(target, os.X_OK):
        failures.append("APPTAINER_EXECUTABLE_MISSING")
        return
    command = [str(target), "--version"]
    commands.append(command)
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10,
                                check=False, text=True)
    except (OSError, subprocess.SubprocessError) as exc:
        failures.append("APPTAINER_VERSION_PROBE:" + type(exc).__name__)
        return
    line = result.stdout.splitlines()[0].strip() if result.stdout.splitlines() else ""
    expected = "apptainer version " + APPTAINER_POLICY_VERSION
    if result.returncode != 0 or line != expected:
        failures.append("APPTAINER_VERSION_RUNTIME_MISMATCH")


def _native_checks(profile: Mapping[str, Any], failures: list[str], commands: list[list[str]]) -> None:
    app = profile.get("application", {})
    extension = app.get("extension")
    entrypoint = app.get("entrypoint")
    if extension:
        ext = Path(extension)
        if not ext.is_file():
            failures.append("NATIVE_EXTENSION_MISSING")
        else:
            # Pybind11 exports ``PyInit__ndnsf``. Loading under an arbitrary
            # probe name asks CPython for a different initializer and creates
            # a false ImportError even when the ELF closure is valid. Probe
            # the canonical package name in a subprocess so collector imports
            # cannot mask the result.
            package_root = ext.parent.parent
            probe = (
                "import sys; sys.path.insert(0, %r); "
                "import ndnsf._ndnsf; print(ndnsf._ndnsf.__file__)"
            ) % str(package_root)
            commands.append([sys.executable, "-c", probe])
            try:
                result = subprocess.run(
                    [sys.executable, "-c", probe], stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=20, check=False, text=True,
                )
                if result.returncode != 0:
                    failures.append("NATIVE_EXTENSION_IMPORT_FAILED:" + str(result.returncode))
            except (OSError, subprocess.SubprocessError) as exc:
                failures.append("NATIVE_EXTENSION_IMPORT_FAILED:" + type(exc).__name__)
            for tool, args in (("readelf", ["readelf", "-d", str(ext)]),
                               ("ldd", ["ldd", "-r", str(ext)])):
                commands.append(args)
                try:
                    result = subprocess.run(args, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, timeout=20,
                                            check=False, text=True)
                    if result.returncode != 0:
                        failures.append("NATIVE_" + tool.upper() + ":" + str(result.returncode))
                except (OSError, subprocess.SubprocessError) as exc:
                    failures.append("NATIVE_" + tool.upper() + ":" + type(exc).__name__)
    else:
        failures.append("NATIVE_EXTENSION_UNDECLARED")
    if entrypoint:
        target = Path(entrypoint)
        if not target.is_file() or not os.access(target, os.X_OK):
            failures.append("NATIVE_ENTRYPOINT_MISSING")
        else:
            command = [str(target), "--help"]
            commands.append(command)
            try:
                result = subprocess.run(command, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=10,
                                        check=False, text=True)
                if result.returncode != 0:
                    failures.append("NATIVE_ENTRYPOINT_HELP:" + str(result.returncode))
            except (OSError, subprocess.SubprocessError) as exc:
                failures.append("NATIVE_ENTRYPOINT_HELP:" + type(exc).__name__)
            for tool, args in (("readelf", ["readelf", "-d", str(target)]),
                               ("ldd", ["ldd", "-r", str(target)])):
                commands.append(args)
                try:
                    result = subprocess.run(args, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, timeout=20,
                                            check=False, text=True)
                    if result.returncode != 0:
                        failures.append("NATIVE_" + tool.upper() + ":" + str(result.returncode))
                except (OSError, subprocess.SubprocessError) as exc:
                    failures.append("NATIVE_" + tool.upper() + ":" + type(exc).__name__)
    else:
        failures.append("NATIVE_ENTRYPOINT_UNDECLARED")


def pre_dispatch(profile_path: Path, candidate_path: Path, *, repo_root: Path,
                 run_root: Optional[Path] = None) -> Dict[str, Any]:
    """Return a deterministic receipt without remote or scheduler side effects."""
    failures: list[str] = []
    commands: list[list[str]] = []
    try:
        profile = load_profile(profile_path, repo_root=repo_root, run_root=run_root)
        candidate = load_json(candidate_path)
        _validate_manifest_shape(candidate, failures)
        if candidate.get("schemaVersion") != CANDIDATE_SCHEMA:
            failures.append("CANDIDATE_SCHEMA")
        expected = build_candidate_manifest(profile, repo_root=repo_root)
        if candidate.get("candidateDigest") != expected["candidateDigest"]:
            failures.append("CANDIDATE_DIGEST_MISMATCH")
        if candidate.get("candidateId") != profile["candidate"]["id"]:
            failures.append("CANDIDATE_ID_MISMATCH")
        _profile_placeholder_checks(profile, failures)
        runtime_manifest = candidate.get("runtime")
        if isinstance(runtime_manifest, Mapping) and runtime_manifest.get("apptainer") != profile["runtime"]["apptainer"]:
            failures.append("APPTAINER_CONTRACT_MISMATCH")
        for section in ("runtime", "application", "harness", "external", "security"):
            value = candidate.get(section, {})
            if not isinstance(value, Mapping):
                continue
            for key, asset in value.items():
                if isinstance(asset, Mapping) and "path" in asset:
                    if section == "runtime" and key == "apptainer":
                        continue
                    _check_asset(
                        asset, section + "." + key, failures,
                        allow_directory=(section == "application" and key == "bundle"),
                    )
        _check_asset(profile["evidence"]["collector"], "evidence.collector", failures)
        _check_asset(profile["runtime"]["application"]["bundle"],
                     "application.bundle", failures, allow_directory=True)
        harness_inputs = profile["runtime"]["harness"].get("inputs", {})
        if isinstance(harness_inputs, Mapping) and isinstance(harness_inputs.get("caseBundle"), Mapping):
            _check_asset(harness_inputs["caseBundle"],
                         "harness.inputs.caseBundle", failures,
                         allow_directory=True)
        manifest_harness = candidate.get("harness", {})
        manifest_inputs = (manifest_harness.get("inputs", {})
                           if isinstance(manifest_harness, Mapping) else {})
        if isinstance(manifest_inputs, Mapping) and isinstance(
                manifest_inputs.get("caseBundle"), Mapping):
            _check_asset(manifest_inputs["caseBundle"],
                         "candidate.harness.inputs.caseBundle", failures,
                         allow_directory=True)
        _check_asset(profile["runtime"]["harness"]["collector"], "harness.collector", failures)
        launcher = Path(profile["runtime"]["harness"]["launcher"])
        if not launcher.is_file():
            failures.append("FILE_MISSING:harness.launcher")
        _harness_checks(profile, failures)
        extension = Path(profile["runtime"]["application"]["extension"])
        if not extension.is_file():
            failures.append("FILE_MISSING:application.extension")
        application_manifest = candidate.get("application")
        if not isinstance(application_manifest, Mapping):
            application_manifest = {}
        harness_manifest = candidate.get("harness")
        if not isinstance(harness_manifest, Mapping):
            harness_manifest = {}
        _check_declared_file(application_manifest.get("extension"),
                             application_manifest.get("extensionSha256"),
                             "application.extension", failures)
        _check_declared_file(harness_manifest.get("launcher"),
                             harness_manifest.get("launcherSha256"),
                             "harness.launcher", failures)
        for value in (profile["runtime"]["builder"]["path"],
                      profile["runtime"]["abiManifest"]["path"],
                      profile["runtime"]["application"]["bundle"]["path"],
                      profile["runtime"]["application"]["extension"],
                      profile["runtime"]["harness"]["launcher"],
                      profile["runtime"]["harness"]["collector"]["path"],
                      profile["evidence"]["root"]):
            path = Path(value)
            if str(path).startswith(str(repo_root.resolve()) + os.sep) and not _inside(path, repo_root):
                failures.append("SYMLINK_ESCAPE:" + str(path))
        if isinstance(harness_inputs, Mapping) and isinstance(harness_inputs.get("caseBundle"), Mapping):
            bundle_path = Path(harness_inputs["caseBundle"]["path"])
            if str(bundle_path).startswith(str(repo_root.resolve()) + os.sep) and not _inside(bundle_path, repo_root):
                failures.append("SYMLINK_ESCAPE:" + str(bundle_path))
        if not _inside(Path(profile["evidence"]["root"]), repo_root) and profile["topology"]["mode"] == "minindn":
            failures.append("EVIDENCE_ROOT_OUTSIDE_REPO")
        _apptainer_checks(profile, failures, commands)
        _native_checks({**profile, "application": profile["runtime"]["application"]}, failures, commands)
    except CandidateError as exc:
        failures.append(str(exc))
        profile = None
        candidate = None
    except (AttributeError, KeyError, OSError, TypeError, ValueError) as exc:
        # Candidate JSON is an untrusted boundary.  A malformed nested object
        # must produce a deterministic rejection receipt rather than a Python
        # traceback that could be mistaken for an orchestration failure.
        failures.append("PRE_DISPATCH_INPUT_INVALID:" + type(exc).__name__)
        profile = None
        candidate = None
    return {
        "schemaVersion": "spec186-pre-dispatch-v1",
        "ok": not failures,
        "failures": sorted(set(failures)),
        "candidateDigest": candidate.get("candidateDigest") if candidate else None,
        "case": profile.get("case") if profile else None,
        "commands": commands,
        "sideEffects": {"ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0},
    }


def earliest_restart_gate(before: Mapping[str, Any], after: Mapping[str, Any]) -> str:
    """Return the earliest gate invalidated by a candidate-plane change."""
    for section, gate in (("source", "T005"), ("runtime", "T006"),
                          ("application", "T005"), ("harness", "T005"),
                          ("configuration", "T005"), ("external", "T007/T008"),
                          ("security", "T005"), ("validation", "T005")):
        if before.get(section) != after.get(section):
            return gate
    return "profile-check"


def validate_terminal(records: Sequence[Mapping[str, Any]], candidate_digest: str,
                      *, expected_status: str = "PASS") -> Dict[str, Any]:
    failures: list[str] = []
    if not isinstance(candidate_digest, str) or not _HEX.fullmatch(candidate_digest):
        failures.append("CANDIDATE_DIGEST_INVALID")
    if not isinstance(expected_status, str) or not expected_status:
        failures.append("EXPECTED_STATUS_INVALID")
    if not records:
        failures.append("NO_RECORDS")
    run_ids: set[str] = set()
    for index, record in enumerate(records):
        if not isinstance(record, Mapping):
            failures.append("RECORD_NOT_OBJECT:" + str(index))
            continue
        if record.get("candidateDigest") != candidate_digest:
            failures.append("CANDIDATE_DIGEST_MISMATCH:" + str(index))
        if record.get("status") != expected_status:
            failures.append("STATUS:" + str(index))
        run_id = record.get("runId")
        if not isinstance(run_id, str) or not run_id:
            failures.append("RUN_ID_MISSING:" + str(index))
        else:
            run_ids.add(run_id)
            if _RUN_ID.fullmatch(run_id) is None:
                failures.append("RUN_ID_INVALID:" + str(index))
        if type(record.get("exitCode")) is not int or record.get("exitCode") != 0:
            failures.append("EXIT_CODE:" + str(index))
        protocol = record.get("protocol")
        if (not isinstance(protocol, Mapping) or
                protocol.get("candidateDigest") != candidate_digest or
                protocol.get("completed") is not True or
                protocol.get("terminalResponse") is not True):
            failures.append("PROTOCOL_EVIDENCE:" + str(index))
        numerical = record.get("numerical")
        if (not isinstance(numerical, Mapping) or
                numerical.get("candidateDigest") != candidate_digest or
                numerical.get("matched") is not True or
                numerical.get("independent") is not True or
                not isinstance(numerical.get("oracleDigest"), str) or
                not _HEX.fullmatch(numerical["oracleDigest"])):
            failures.append("NUMERICAL_EVIDENCE:" + str(index))
        roles = record.get("roles")
        role_names = {role.get("name") for role in roles
                      if isinstance(role, Mapping)} if isinstance(roles, list) else set()
        qwen_role_names = {
            "Stage" + str(index) for index in range(len(role_names))
        }
        role_name_shape = (role_names == set(YOLO_ROLES) or
                           role_names == qwen_role_names)
        role_evidence_invalid = (
            not isinstance(roles, list) or len(roles) < 2 or not role_name_shape or any(
                not isinstance(role, Mapping) or
                role.get("candidateDigest") != candidate_digest or
                not isinstance(role.get("name"), str) or not role.get("name") or
                role.get("observed") is not True or
                not isinstance(role.get("backend"), str) or not role.get("backend") or
                type(role.get("gpu")) is not int or role.get("gpu") < -1 or
                (role.get("gpu") >= 0) != ("cuda" in role.get("backend", "").lower())
                for role in roles) or
                len({role.get("name") for role in roles if isinstance(role, Mapping)}) != len(roles))
        if (not role_evidence_invalid and role_names == set(YOLO_ROLES) and
                any(role["gpu"] >= 0 for role in roles)):
            role_evidence_invalid = any(
                (role["gpu"] >= 0) != (role["name"] != "Merge") or
                role["backend"] != (
                    "onnxruntime-cuda" if role["name"] != "Merge"
                    else "onnxruntime-cpu")
                for role in roles)
        if role_evidence_invalid:
            failures.append("ROLE_EVIDENCE:" + str(index))
        process = record.get("process")
        if (not isinstance(process, Mapping) or
                process.get("candidateDigest") != candidate_digest or
                process.get("allExited") is not True or
                type(process.get("exitCode")) is not int or
                process.get("exitCode") != 0):
            failures.append("PROCESS_EVIDENCE:" + str(index))
        cleanup = record.get("cleanup")
        if (not isinstance(cleanup, Mapping) or
                cleanup.get("candidateDigest") != candidate_digest or
                cleanup.get("reaped") is not True):
            failures.append("CLEANUP:" + str(index))
    if len(run_ids) > 1:
        failures.append("RUN_ID_MISMATCH")
    return {"status": expected_status if not failures else "FAILED",
            "failures": sorted(set(failures)), "candidateDigest": candidate_digest}
