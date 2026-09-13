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
CASES = (
    "yolo-minindn-normal", "yolo-minindn-negative", "qwen06b-minindn-cpu",
    "yolo-tiger-single-gpu", "yolo-tiger-two-node-normal",
    "yolo-tiger-two-node-negative", "yolo-tiger-two-node-reuse",
    "qwen06b-tiger-experimental",
)
YOLO_ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
_HEX = re.compile(r"^[0-9a-f]{64}$")
_IDENT = re.compile(r"^[A-Za-z0-9_.:/-]+$")
_PROFILE_KEYS = {
    "schemaVersion", "candidate", "case", "topology", "roles", "runtime",
    "model", "workload", "security", "timeouts", "resources", "evidence",
}
_CANDIDATE_KEYS = {"id", "sourceCommit", "sourceSealSha256"}
_MANIFEST_KEYS = {"schemaVersion", "candidateId", "candidateDigest", "source", "runtime",
                  "application", "harness", "configuration", "external", "security", "validation"}
_ASSET_KEYS = {"path", "sha256"}
_RUNTIME_KEYS = {
    "baseSif", "builder", "abiManifest", "allowCpuFallback", "application", "harness",
}


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
    _digest(candidate["sourceSealSha256"], "candidate.sourceSealSha256")

    topology = profile["topology"]
    if not isinstance(topology, Mapping):
        raise CandidateError("TOPOLOGY_NOT_OBJECT")
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
    for endpoint in endpoints:
        if not isinstance(endpoint, Mapping):
            raise CandidateError("NFD_ENDPOINT_NOT_OBJECT")
        _required(endpoint, ("host", "port"), "NFD_ENDPOINT")
        if endpoint["host"] not in hosts or type(endpoint["port"]) is not int or not 1024 <= endpoint["port"] <= 65535:
            raise CandidateError("NFD_ENDPOINT_INVALID")
    if profile["case"].startswith("yolo-"):
        roles = profile["roles"]
        if not isinstance(roles, list) or [item.get("name") for item in roles if isinstance(item, Mapping)] != list(YOLO_ROLES):
            raise CandidateError("YOLO_ROLE_ORDER")
        for role in roles:
            if not isinstance(role, Mapping):
                raise CandidateError("ROLE_NOT_OBJECT")
            _required(role, ("name", "identity", "node", "gpu", "backend", "service", "binary"), "ROLE")
            if not _IDENT.fullmatch(_string(role["identity"], "role.identity")):
                raise CandidateError("ROLE_IDENTITY")
            if type(role["gpu"]) is not int or role["gpu"] < -1 or role["gpu"] > 15:
                raise CandidateError("ROLE_GPU")
            _string(role["node"], "role.node"); _string(role["backend"], "role.backend")
            _path(role["binary"], "role.binary"); _string(role["service"], "role.service")
    elif not isinstance(profile["roles"], list) or not profile["roles"]:
        raise CandidateError("QWEN_ROLES_EMPTY")
    else:
        stages = [role.get("stage") for role in profile["roles"] if isinstance(role, Mapping)]
        if stages != list(range(len(profile["roles"]))):
            raise CandidateError("QWEN_STAGE_ORDER")
        for role in profile["roles"]:
            if not isinstance(role, Mapping):
                raise CandidateError("ROLE_NOT_OBJECT")
            _required(role, ("name", "identity", "node", "gpu", "backend", "service", "binary",
                             "stage", "dependencies", "artifactSha256", "tokenizerSha256", "modelUri"), "ROLE")
            if not isinstance(role["dependencies"], list):
                raise CandidateError("QWEN_DEPENDENCIES")
            _digest(role["artifactSha256"], "role.artifactSha256")
            _digest(role["tokenizerSha256"], "role.tokenizerSha256")
            _string(role["modelUri"], "role.modelUri")

    runtime = profile["runtime"]
    if not isinstance(runtime, Mapping):
        raise CandidateError("RUNTIME_NOT_OBJECT")
    _keys(runtime, _RUNTIME_KEYS, "RUNTIME")
    _required(runtime, ("baseSif", "builder", "abiManifest", "allowCpuFallback"), "RUNTIME")
    for key in ("baseSif", "builder", "abiManifest"):
        _asset(runtime[key], "runtime." + key)
    if type(runtime["allowCpuFallback"]) is not bool:
        raise CandidateError("RUNTIME_CPU_FALLBACK")
    application = runtime.get("application")
    if not isinstance(application, Mapping):
        raise CandidateError("APPLICATION_NOT_OBJECT")
    _required(application, ("bundle", "bundleSha256", "entrypoint", "extension"), "APPLICATION")
    _asset(application["bundle"], "runtime.application.bundle")
    _digest(application["bundleSha256"], "runtime.application.bundleSha256")
    _path(application["entrypoint"], "runtime.application.entrypoint")
    _path(application["extension"], "runtime.application.extension")
    harness = runtime.get("harness")
    if not isinstance(harness, Mapping):
        raise CandidateError("HARNESS_NOT_OBJECT")
    _required(harness, ("launcher", "collector"), "HARNESS")
    _path(harness["launcher"], "runtime.harness.launcher")
    _asset(harness["collector"], "runtime.harness.collector")

    model = profile["model"]
    if not isinstance(model, Mapping):
        raise CandidateError("MODEL_NOT_OBJECT")
    _required(model, ("family", "format", "backend", "model", "tokenizer"), "MODEL")
    for key in ("family", "format", "backend"):
        _string(model[key], "model." + key)
    _asset(model["model"], "model.model")
    _asset(model["tokenizer"], "model.tokenizer", optional=True)
    if profile["case"].startswith("qwen") and model["family"] != "Qwen3-0.6B":
        raise CandidateError("QWEN_MODEL_FAMILY_MISMATCH")
    if profile["case"].startswith("qwen"):
        _asset(model.get("stageManifest"), "model.stageManifest")
    if profile["case"].startswith("yolo") and model["family"] != "YOLOv8n":
        raise CandidateError("YOLO_MODEL_FAMILY_MISMATCH")
    if profile["case"] == "qwen06b-tiger-experimental" and model["format"] not in {"onnx", "gguf-q3"}:
        raise CandidateError("QWEN_FORMAT_UNSUPPORTED")

    workload = profile["workload"]
    if not isinstance(workload, Mapping):
        raise CandidateError("WORKLOAD_NOT_OBJECT")
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
    _required(security, ("identityRoot", "permissions"), "SECURITY")
    _path(security["identityRoot"], "security.identityRoot")
    _asset(security["permissions"], "security.permissions")

    timeouts = profile["timeouts"]
    if not isinstance(timeouts, Mapping):
        raise CandidateError("TIMEOUTS_NOT_OBJECT")
    _required(timeouts, ("startupSeconds", "requestSeconds", "completionSeconds", "cleanupSeconds"), "TIMEOUTS")
    for key in ("startupSeconds", "requestSeconds", "completionSeconds", "cleanupSeconds"):
        _positive_int(timeouts[key], "timeouts." + key, 86400)

    resources = profile["resources"]
    if not isinstance(resources, Mapping):
        raise CandidateError("RESOURCES_NOT_OBJECT")
    _required(resources, ("account", "partition", "cpusPerNode", "memory", "gpus"), "RESOURCES")
    for key in ("account", "partition", "memory"):
        if not re.fullmatch(r"[A-Za-z0-9_.:-]+", _string(resources[key], "resources." + key)):
            raise CandidateError("INVALID_RESOURCE:" + key)
    _positive_int(resources["cpusPerNode"], "resources.cpusPerNode", 256)
    if type(resources["gpus"]) is not int or resources["gpus"] < 0 or resources["gpus"] > 16:
        raise CandidateError("INVALID_RESOURCE:gpus")
    if resources["gpus"]:
        gpu = resources.get("gpu")
        if not isinstance(gpu, Mapping):
            raise CandidateError("GPU_CAPACITY_UNDECLARED")
        _required(gpu, ("uuid", "freeMemoryMb", "signature"), "GPU")
        if not re.fullmatch(r"GPU-[A-Za-z0-9-]+", _string(gpu["uuid"], "gpu.uuid")):
            raise CandidateError("GPU_UUID")
        _positive_int(gpu["freeMemoryMb"], "gpu.freeMemoryMb", 1000000)
        _digest(gpu["signature"], "gpu.signature")

    evidence = profile["evidence"]
    if not isinstance(evidence, Mapping):
        raise CandidateError("EVIDENCE_NOT_OBJECT")
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
    evidence = profile["evidence"]
    launcher_path = Path(runtime["harness"]["launcher"])
    launcher_sha = file_digest(launcher_path) if launcher_path.is_file() else "0" * 64
    extension_path = Path(runtime["application"]["extension"])
    extension_sha = file_digest(extension_path) if extension_path.is_file() else "0" * 64
    manifest = {
        "schemaVersion": CANDIDATE_SCHEMA,
        "candidateId": profile["candidate"]["id"],
        "source": {"commit": profile["candidate"]["sourceCommit"],
                    "sourceSealSha256": profile["candidate"]["sourceSealSha256"]},
        "runtime": {"baseSif": _asset_manifest(runtime["baseSif"], "baseSif"),
                    "builder": _asset_manifest(runtime["builder"], "builder"),
                    "abiManifest": _asset_manifest(runtime["abiManifest"], "abiManifest")},
        "application": {"bundle": _asset_manifest(runtime["application"]["bundle"], "application.bundle"),
                        "bundleSha256": runtime["application"]["bundleSha256"],
                        "entrypoint": runtime["application"]["entrypoint"],
                        "extension": runtime["application"]["extension"],
                        "extensionSha256": extension_sha},
        "harness": {"launcher": runtime["harness"]["launcher"],
                     "launcherSha256": launcher_sha,
                    "collector": _asset_manifest(runtime["harness"]["collector"], "harness.collector")},
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
    for section in ("source", "runtime", "application", "harness", "configuration",
                    "external", "security", "validation"):
        if not isinstance(manifest[section], Mapping):
            failures.append("CANDIDATE_SECTION_NOT_OBJECT:" + section)


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _check_asset(asset: Mapping[str, Any], label: str, failures: list[str]) -> None:
    path = asset.get("path")
    expected = asset.get("sha256")
    if path is None:
        if expected is not None:
            failures.append("OPTIONAL_DIGEST:" + label)
        return
    target = Path(path)
    if expected == "0" * 64:
        failures.append("PLACEHOLDER_DIGEST:" + label)
    if not target.is_file():
        failures.append("FILE_MISSING:" + label)
        return
    try:
        actual = file_digest(target)
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
        for section in ("runtime", "application", "harness", "external", "security"):
            value = candidate.get(section, {})
            for key, asset in value.items():
                if isinstance(asset, Mapping) and "path" in asset:
                    _check_asset(asset, section + "." + key, failures)
        _check_asset(profile["evidence"]["collector"], "evidence.collector", failures)
        _check_asset(profile["runtime"]["application"]["bundle"], "application.bundle", failures)
        _check_asset(profile["runtime"]["harness"]["collector"], "harness.collector", failures)
        launcher = Path(profile["runtime"]["harness"]["launcher"])
        if not launcher.is_file():
            failures.append("FILE_MISSING:harness.launcher")
        extension = Path(profile["runtime"]["application"]["extension"])
        if not extension.is_file():
            failures.append("FILE_MISSING:application.extension")
        _check_declared_file(candidate.get("application", {}).get("extension"),
                             candidate.get("application", {}).get("extensionSha256"),
                             "application.extension", failures)
        _check_declared_file(candidate.get("harness", {}).get("launcher"),
                             candidate.get("harness", {}).get("launcherSha256"),
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
        if not _inside(Path(profile["evidence"]["root"]), repo_root) and profile["topology"]["mode"] == "minindn":
            failures.append("EVIDENCE_ROOT_OUTSIDE_REPO")
        _native_checks({**profile, "application": profile["runtime"]["application"]}, failures, commands)
    except CandidateError as exc:
        failures.append(str(exc))
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
    if not records:
        failures.append("NO_RECORDS")
    for index, record in enumerate(records):
        if record.get("candidateDigest") != candidate_digest:
            failures.append("CANDIDATE_DIGEST_MISMATCH:" + str(index))
        if record.get("status") != expected_status:
            failures.append("STATUS:" + str(index))
        if record.get("exitCode") != 0:
            failures.append("EXIT_CODE:" + str(index))
        cleanup = record.get("cleanup")
        if not isinstance(cleanup, Mapping) or cleanup.get("reaped") is not True:
            failures.append("CLEANUP:" + str(index))
    return {"status": expected_status if not failures else "FAILED",
            "failures": sorted(set(failures)), "candidateDigest": candidate_digest}
