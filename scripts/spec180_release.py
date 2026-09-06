#!/usr/bin/env python3
"""Fail-closed Spec180 profile rendering and submission boundary.

The renderer is deliberately side-effect free.  It validates one fixed
profile and one candidate-bound run record, then produces the exact Slurm
argv/environment that a submit wrapper may use.  Scheduler invocation is
available only through :func:`submit`, after rendering has succeeded.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
from typing import Any, Callable, Dict, Mapping, Optional, Sequence, Tuple


ROOT = Path(__file__).resolve().parents[1]
PROFILE_PATH = ROOT / "packaging/ndnsf-di-container/jobs/spec180/profile.json"
SCHEMA = "spec180-profile-v1"
RUN_SCHEMA = "spec180-run-record-v1"
RENDER_SCHEMA = "spec180-render-v1"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_GATES = ("yolo-functional", "qwen-functional")
_PROFILE_KEYS = {"schema", "profileId", "fixed", "gates", "allowedRunRecordFields"}
_FIXED_KEYS = {
    "apptainerPath", "apptainerVersion", "cwd", "exportMode", "runtime",
    "network", "identityIsolation", "initialSvsSettleMs", "ackTimeoutMs",
    "requestTimeoutMs", "admissionControl", "logging",
}
_GATE_KEYS = {
    "job", "nodes", "ntasks", "gres", "memory", "time", "providerCount",
    "modelProviderCount", "mergeProviderCount", "gpuProviderDevices",
    "mergeCudaVisibleDevices", "requestCount", "inputMode", "decodePolicy",
    "maxNewTokens",
}
_EXPECTED_GATES = {
    "yolo-functional": {
        "job": "yolo-functional.sbatch", "nodes": 1, "ntasks": 1,
        "gres": "gpu:rtx_6000:1", "memory": "96G", "time": "00:30:00",
        "providerCount": 4, "modelProviderCount": 3,
        "mergeProviderCount": 1, "gpuProviderDevices": [0],
        "mergeCudaVisibleDevices": None, "requestCount": 1,
        "inputMode": "REPO_REF", "decodePolicy": None, "maxNewTokens": None,
    },
    "qwen-functional": {
        "job": "qwen-functional.sbatch", "nodes": 1, "ntasks": 1,
        "gres": "gpu:rtx_6000:3", "memory": "96G", "time": "01:00:00",
        "providerCount": 3, "modelProviderCount": 3,
        "mergeProviderCount": 0, "gpuProviderDevices": [0, 1, 2],
        "mergeCudaVisibleDevices": None, "requestCount": 2,
        "inputMode": "INLINE", "decodePolicy": "greedy", "maxNewTokens": 8,
    },
}
_RUN_KEYS = {
    "schema", "runId", "gate", "profileId", "profileSha256", "candidate",
    "sif", "model", "workload", "output",
}
_CANDIDATE_KEYS = {"candidateId", "candidateDigest"}
_SIF_KEYS = {"path", "sha256"}
_MODEL_KEYS = {"manifest", "manifestSha256", "root"}
_WORKLOAD_KEYS = {"id", "manifest", "manifestSha256"}
_OUTPUT_KEYS = {"root"}


class ReleaseError(ValueError):
    """Raised when a Spec180 release input violates its fixed contract."""


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")


def canonical_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _duplicate_rejector(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseError("DUPLICATE_JSON_FIELD:" + key)
        result[key] = value
    return result


def load_json(path: Path | str) -> Dict[str, Any]:
    source = Path(path)
    try:
        value = json.loads(
            source.read_text(encoding="utf-8"),
            object_pairs_hook=_duplicate_rejector,
        )
    except ReleaseError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReleaseError("JSON_READ_FAILED:" + str(source)) from exc
    if not isinstance(value, dict):
        raise ReleaseError("JSON_ROOT_NOT_OBJECT:" + str(source))
    return value


def _keys(value: Mapping[str, Any], expected: set[str], label: str) -> None:
    unknown = sorted(set(value) - expected)
    if unknown:
        raise ReleaseError("UNKNOWN_" + label + "_FIELD:" + ",".join(unknown))


def _required(value: Mapping[str, Any], fields: Sequence[str], label: str) -> None:
    missing = [field for field in fields if field not in value]
    if missing:
        raise ReleaseError("MISSING_" + label + "_FIELD:" + ",".join(missing))


def _require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise ReleaseError("INVALID_DIGEST:" + label)
    return value


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ReleaseError("INVALID_STRING:" + label)
    return value


def _absolute_path(value: Any, label: str) -> str:
    path = _require_string(value, label)
    if not Path(path).is_absolute():
        raise ReleaseError("PATH_NOT_ABSOLUTE:" + label)
    return path


def _verify_file_digest(path: str, expected: str, label: str) -> None:
    target = Path(path)
    if not target.is_file():
        raise ReleaseError("FILE_MISSING:" + label)
    try:
        actual = _digest_bytes(target.read_bytes())
    except OSError as exc:
        raise ReleaseError("FILE_READ_FAILED:" + label) from exc
    if actual != expected:
        raise ReleaseError("FILE_DIGEST_MISMATCH:" + label)


def _validate_qwen_model_manifest(path: str) -> None:
    """Verify the signed external QWEN-F identity before scheduler mutation.

    The remote job repeats object/stage checks inside the image.  This host
    check deliberately verifies only the signed identity and fixed runtime
    policy; it must not accept a profile or workload value as an authority.
    """
    manifest_path = Path(path)
    manifest = load_json(manifest_path)
    if manifest.get("schema") != "spec180-model-manifest-v1":
        raise ReleaseError("MODEL_MANIFEST_SCHEMA_UNSUPPORTED")
    if manifest.get("modelFamily") != "Qwen3.6-27B":
        raise ReleaseError("MODEL_MANIFEST_MODEL_FAMILY_MISMATCH")
    model = manifest.get("model")
    model_id = model.get("id") if isinstance(model, Mapping) else model
    if model_id != "Qwen/Qwen3.6-27B":
        raise ReleaseError("MODEL_MANIFEST_MODEL_ID_MISMATCH")
    required = (
        "manifestRevision", "modelRevision", "sourceRevision", "modelIdentityDigest",
        "graph", "initializers", "stages", "stageManifest", "tokenizer",
        "chatTemplate", "stopPolicy", "runtime", "prompt", "signature",
    )
    missing = [field for field in required if field not in manifest]
    if missing:
        raise ReleaseError("MODEL_MANIFEST_FIELD_MISSING:" + ",".join(missing))
    runtime = manifest.get("runtime")
    if not isinstance(runtime, Mapping):
        raise ReleaseError("MODEL_MANIFEST_RUNTIME_INVALID")
    if runtime.get("backend") not in {"onnxruntime", "onnxruntime-cuda"}:
        raise ReleaseError("MODEL_MANIFEST_BACKEND_INVALID")
    if runtime.get("executionProvider") != "cuda":
        raise ReleaseError("MODEL_MANIFEST_EXECUTION_PROVIDER_INVALID")
    if runtime.get("cpuFallback") is not False:
        raise ReleaseError("MODEL_MANIFEST_CPU_FALLBACK_ENABLED")
    stages = manifest.get("stages")
    if not isinstance(stages, list) or len(stages) != 3:
        raise ReleaseError("MODEL_MANIFEST_STAGE_COUNT_INVALID")

    # Load the shared contract gate with an explicit sys.modules entry.  The
    # gate owns canonical serialization and the registered public-key digest;
    # this release layer must not duplicate cryptographic semantics.
    import sys
    registry_path = ROOT / "specs/180-ack-driven-cross-model-qualification" \
        / "contracts/trust-root-registry-v1.json"
    gate_path = ROOT / "scripts/spec180_contract_gate.py"
    spec = importlib.util.spec_from_file_location(
        "spec180_release_contract_gate", gate_path)
    if spec is None or spec.loader is None:
        raise ReleaseError("MODEL_MANIFEST_CONTRACT_GATE_IMPORT_FAILED")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
        registry, registry_issues = module.load_trust_registry(registry_path.parent.parent)
        if registry_issues or registry is None:
            codes = ",".join(issue.code for issue in registry_issues) or "missing"
            raise ReleaseError("MODEL_MANIFEST_TRUST_ROOT_INVALID:" + codes)
        valid, issues = module.verify_signed_manifest(
            manifest,
            registry["modelManifest"],
            feature_dir=registry_path.parent.parent,
            model_family="Qwen3.6-27B",
        )
        if not valid:
            codes = ",".join(issue.code for issue in issues) or "invalid"
            raise ReleaseError("MODEL_MANIFEST_SIGNATURE_INVALID:" + codes)
    finally:
        sys.modules.pop(spec.name, None)


def _validate_dispatch_workload(path: str, expected_digest: str,
                                gate: str, *, check_files: bool) -> None:
    """Consume the T011 dispatcher contract before any scheduler mutation.

    The release layer intentionally imports the single dispatcher validator
    rather than copying its gate/argument/environment rules.  This keeps the
    host-side pre-submit check and the in-image check on one schema owner.
    """
    if check_files:
        _verify_file_digest(path, expected_digest, "workload.manifest")
    dispatcher_path = ROOT / "scripts/run_spec180_case.py"
    if not dispatcher_path.is_file():
        raise ReleaseError("DISPATCHER_SOURCE_MISSING")
    spec = importlib.util.spec_from_file_location(
        "spec180_dispatch_contract", dispatcher_path)
    if spec is None or spec.loader is None:
        raise ReleaseError("DISPATCHER_IMPORT_FAILED")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        workload = load_json(path)
        normalized = module.validate_workload(workload, gate)
        if check_files:
            # The dispatch contract names an image-relative entrypoint.  The
            # source tree must contain the same registered entrypoint before
            # a scheduler call is allowed; otherwise a planned QWEN-F path
            # could consume a remote allocation only to fail at image entry.
            try:
                module.resolve_entrypoint(ROOT, normalized["entrypoint"])
            except Exception as exc:
                raise ReleaseError(
                    "WORKLOAD_ENTRYPOINT_NOT_READY:" + str(exc)) from exc
    except ReleaseError:
        raise
    except Exception as exc:
        reason = str(exc) or exc.__class__.__name__
        raise ReleaseError("WORKLOAD_DISPATCH_INVALID:" + reason) from exc


def _slurm_export_arg(environment: Mapping[str, str]) -> str:
    """Render the allow-listed job environment with Slurm's NONE baseline.

    ``--export=NONE`` by itself drops every ``SPEC180_*`` value.  The fixed
    profile therefore uses Slurm's explicit ``NONE,KEY=VALUE`` form; rejecting
    delimiters keeps a path from changing the export list semantics.
    """
    fields = []
    for key in sorted(environment):
        value = environment[key]
        if (not isinstance(key, str) or
                not re.fullmatch(r"[A-Z][A-Z0-9_]*", key) or
                not isinstance(value, str) or
                any(char in value for char in (",", "\n", "\r"))):
            raise ReleaseError("UNSAFE_SLURM_EXPORT:" + str(key))
        fields.append(key + "=" + value)
    return "--export=NONE," + ",".join(fields)


def validate_profile(profile: Mapping[str, Any]) -> Dict[str, Any]:
    if not isinstance(profile, Mapping):
        raise ReleaseError("PROFILE_NOT_OBJECT")
    _keys(profile, _PROFILE_KEYS, "PROFILE")
    _required(profile, tuple(_PROFILE_KEYS), "PROFILE")
    if profile.get("schema") != SCHEMA:
        raise ReleaseError("PROFILE_SCHEMA_UNSUPPORTED")
    _require_string(profile.get("profileId"), "profileId")
    fixed = profile.get("fixed")
    if not isinstance(fixed, Mapping):
        raise ReleaseError("PROFILE_FIXED_NOT_OBJECT")
    _keys(fixed, _FIXED_KEYS, "FIXED")
    _required(fixed, tuple(_FIXED_KEYS), "FIXED")
    if fixed["apptainerPath"] != "/opt/apptainer/1.5.3/bin/apptainer":
        raise ReleaseError("PROFILE_APPTAINER_PATH_MISMATCH")
    if fixed["apptainerVersion"] != "1.5.3":
        raise ReleaseError("PROFILE_APPTAINER_VERSION_MISMATCH")
    if fixed["cwd"] != "/bundle" or fixed["exportMode"] != "NONE":
        raise ReleaseError("PROFILE_CONTAINER_BOUNDARY_MISMATCH")
    if fixed["runtime"] != "onnxruntime":
        raise ReleaseError("PROFILE_RUNTIME_MISMATCH")
    if fixed["network"] != "host-nfd-checked-routes":
        raise ReleaseError("PROFILE_NETWORK_MISMATCH")
    if fixed["identityIsolation"] != "per-process-home-pib-tpm":
        raise ReleaseError("PROFILE_IDENTITY_ISOLATION_MISMATCH")
    if (fixed["initialSvsSettleMs"], fixed["ackTimeoutMs"],
            fixed["requestTimeoutMs"]) != (5000, 1500, 60000):
        raise ReleaseError("PROFILE_TIMING_MISMATCH")
    if fixed["admissionControl"] is not False:
        raise ReleaseError("PROFILE_ADMISSION_CONTROL_MUST_BE_DISABLED")
    gates = profile.get("gates")
    if not isinstance(gates, Mapping) or set(gates) != set(_GATES):
        raise ReleaseError("PROFILE_GATE_REGISTRY_MISMATCH")
    for gate_name in _GATES:
        gate = gates[gate_name]
        if not isinstance(gate, Mapping):
            raise ReleaseError("GATE_NOT_OBJECT:" + gate_name)
        _keys(gate, _GATE_KEYS, "GATE")
        _required(gate, tuple(_GATE_KEYS), "GATE")
        if dict(gate) != _EXPECTED_GATES[gate_name]:
            raise ReleaseError("GATE_FIXED_VALUE_MISMATCH:" + gate_name)
        if gate["nodes"] != 1 or gate["ntasks"] != 1:
            raise ReleaseError("GATE_RESOURCE_SHAPE_MISMATCH:" + gate_name)
        if gate["providerCount"] != (
                4 if gate_name == "yolo-functional" else 3):
            raise ReleaseError("GATE_PROVIDER_COUNT_MISMATCH:" + gate_name)
        if (gate["requestCount"] != _EXPECTED_GATES[gate_name]["requestCount"]
                or gate["gpuProviderDevices"]
                != _EXPECTED_GATES[gate_name]["gpuProviderDevices"]):
            raise ReleaseError("GATE_WORKLOAD_SHAPE_MISMATCH:" + gate_name)
        if gate["inputMode"] != (
                "REPO_REF" if gate_name == "yolo-functional" else "INLINE"):
            raise ReleaseError("GATE_INPUT_MODE_MISMATCH:" + gate_name)
    allowed = profile.get("allowedRunRecordFields")
    if (not isinstance(allowed, list) or
            set(allowed) != _RUN_KEYS or len(allowed) != len(_RUN_KEYS)):
        raise ReleaseError("PROFILE_RUN_FIELD_ALLOWLIST_MISMATCH")
    return dict(profile)


def load_profile(path: Path | str = PROFILE_PATH) -> Dict[str, Any]:
    return validate_profile(load_json(path))


def _validate_run_record(run: Mapping[str, Any], profile: Mapping[str, Any],
                         gate: str, *, check_files: bool) -> Dict[str, Any]:
    if not isinstance(run, Mapping):
        raise ReleaseError("RUN_NOT_OBJECT")
    _keys(run, _RUN_KEYS, "RUN")
    _required(run, tuple(_RUN_KEYS), "RUN")
    if run.get("schema") != RUN_SCHEMA:
        raise ReleaseError("RUN_SCHEMA_UNSUPPORTED")
    if gate not in _GATES or run.get("gate") != gate:
        raise ReleaseError("RUN_GATE_MISMATCH")
    if run.get("profileId") != profile["profileId"]:
        raise ReleaseError("RUN_PROFILE_ID_MISMATCH")
    if run.get("profileSha256") != canonical_digest(profile):
        raise ReleaseError("PROFILE_DIGEST_MISMATCH")
    _require_string(run.get("runId"), "runId")

    candidate = run.get("candidate")
    if not isinstance(candidate, Mapping):
        raise ReleaseError("CANDIDATE_NOT_OBJECT")
    _keys(candidate, _CANDIDATE_KEYS, "CANDIDATE")
    _required(candidate, tuple(_CANDIDATE_KEYS), "CANDIDATE")
    _require_string(candidate.get("candidateId"), "candidateId")
    _require_digest(candidate.get("candidateDigest"), "candidateDigest")

    sif = run.get("sif")
    if not isinstance(sif, Mapping):
        raise ReleaseError("SIF_NOT_OBJECT")
    _keys(sif, _SIF_KEYS, "SIF")
    _required(sif, tuple(_SIF_KEYS), "SIF")
    sif_path = _absolute_path(sif.get("path"), "sif.path")
    sif_digest = _require_digest(sif.get("sha256"), "sif.sha256")
    if check_files:
        _verify_file_digest(sif_path, sif_digest, "sif")

    model = run.get("model")
    if not isinstance(model, Mapping):
        raise ReleaseError("MODEL_NOT_OBJECT")
    _keys(model, _MODEL_KEYS, "MODEL")
    _required(model, tuple(_MODEL_KEYS), "MODEL")
    manifest_path = _absolute_path(model.get("manifest"), "model.manifest")
    manifest_digest = _require_digest(model.get("manifestSha256"),
                                      "model.manifestSha256")
    _absolute_path(model.get("root"), "model.root")
    if check_files:
        _verify_file_digest(manifest_path, manifest_digest, "model.manifest")
        if gate == "qwen-functional":
            _validate_qwen_model_manifest(manifest_path)

    workload = run.get("workload")
    if not isinstance(workload, Mapping):
        raise ReleaseError("WORKLOAD_NOT_OBJECT")
    _keys(workload, _WORKLOAD_KEYS, "WORKLOAD")
    _required(workload, tuple(_WORKLOAD_KEYS), "WORKLOAD")
    _require_string(workload.get("id"), "workload.id")
    workload_path = _absolute_path(workload.get("manifest"), "workload.manifest")
    workload_digest = _require_digest(workload.get("manifestSha256"),
                                      "workload.manifestSha256")
    _validate_dispatch_workload(
        workload_path, workload_digest, gate, check_files=check_files)

    output = run.get("output")
    if not isinstance(output, Mapping):
        raise ReleaseError("OUTPUT_NOT_OBJECT")
    _keys(output, _OUTPUT_KEYS, "OUTPUT")
    _required(output, tuple(_OUTPUT_KEYS), "OUTPUT")
    _absolute_path(output.get("root"), "output.root")
    return dict(run)


def render(profile: Mapping[str, Any], run: Mapping[str, Any],
           repository_root: Path | str = ROOT, *,
           check_files: bool = False) -> Dict[str, Any]:
    """Validate inputs and render a deterministic, side-effect-free report."""
    profile_value = validate_profile(profile)
    gate = str(run.get("gate", "")) if isinstance(run, Mapping) else ""
    run_value = _validate_run_record(
        run, profile_value, gate, check_files=check_files)
    root = Path(repository_root).resolve()
    job = root / "packaging/ndnsf-di-container/jobs/spec180" / \
        profile_value["gates"][gate]["job"]
    if not job.is_file():
        raise ReleaseError("JOB_ENTRYPOINT_MISSING:" + str(job))
    fixed = profile_value["fixed"]
    gate_config = profile_value["gates"][gate]
    effective = {
        "gate": gate,
        "profileId": profile_value["profileId"],
        "cwd": fixed["cwd"],
        "runtime": fixed["runtime"],
        "network": fixed["network"],
        "apptainerPath": fixed["apptainerPath"],
        "apptainerVersion": fixed["apptainerVersion"],
        "exportMode": fixed["exportMode"],
        "identityIsolation": fixed["identityIsolation"],
        "initialSvsSettleMs": fixed["initialSvsSettleMs"],
        "ackTimeoutMs": fixed["ackTimeoutMs"],
        "requestTimeoutMs": fixed["requestTimeoutMs"],
        "admissionControl": fixed["admissionControl"],
        "providerCount": gate_config["providerCount"],
        "modelProviderCount": gate_config["modelProviderCount"],
        "mergeProviderCount": gate_config["mergeProviderCount"],
        "gpuProviderDevices": gate_config["gpuProviderDevices"],
        "requestCount": gate_config["requestCount"],
        "inputMode": gate_config["inputMode"],
        "decodePolicy": gate_config["decodePolicy"],
        "maxNewTokens": gate_config["maxNewTokens"],
        "candidateId": run_value["candidate"]["candidateId"],
        "candidateDigest": run_value["candidate"]["candidateDigest"],
        "sifPath": run_value["sif"]["path"],
        "sifSha256": run_value["sif"]["sha256"],
        "modelManifest": run_value["model"]["manifest"],
        "modelManifestSha256": run_value["model"]["manifestSha256"],
        "modelRoot": run_value["model"]["root"],
        "workloadId": run_value["workload"]["id"],
        "workloadManifest": run_value["workload"]["manifest"],
        "workloadManifestSha256": run_value["workload"]["manifestSha256"],
        "outputRoot": run_value["output"]["root"],
    }
    environment = {
        "SPEC180_GATE": gate,
        "SPEC180_PROFILE_ID": profile_value["profileId"],
        "SPEC180_PROFILE_SHA256": canonical_digest(profile_value),
        "SPEC180_RUN_ID": run_value["runId"],
        "SPEC180_CANDIDATE_ID": run_value["candidate"]["candidateId"],
        "SPEC180_CANDIDATE_DIGEST": run_value["candidate"]["candidateDigest"],
        "SPEC180_SIF": run_value["sif"]["path"],
        "SPEC180_SIF_SHA256": run_value["sif"]["sha256"],
        "SPEC180_MODEL_MANIFEST": run_value["model"]["manifest"],
        "SPEC180_MODEL_MANIFEST_SHA256": run_value["model"]["manifestSha256"],
        "SPEC180_MODEL_ROOT": run_value["model"]["root"],
        "SPEC180_WORKLOAD": run_value["workload"]["manifest"],
        "SPEC180_WORKLOAD_SHA256": run_value["workload"]["manifestSha256"],
        "SPEC180_OUTPUT_ROOT": run_value["output"]["root"],
    }
    argv = [
        "sbatch", _slurm_export_arg(environment),
        "--nodes=1", "--ntasks=1", "--gres=" + gate_config["gres"],
        "--mem=" + gate_config["memory"], "--time=" + gate_config["time"],
        str(job),
    ]
    return {
        "schema": RENDER_SCHEMA,
        "status": "PASS",
        "gate": gate,
        "profileId": profile_value["profileId"],
        "profileSha256": canonical_digest(profile_value),
        "runId": run_value["runId"],
        "candidateId": run_value["candidate"]["candidateId"],
        "candidateDigest": run_value["candidate"]["candidateDigest"],
        "effectiveConfig": effective,
        "effectiveConfigSha256": canonical_digest(effective),
        "argv": argv,
        "argvSha256": canonical_digest(argv),
        "environment": environment,
        "environmentSha256": canonical_digest(environment),
    }


def submit(gate: str, profile: Mapping[str, Any], run: Mapping[str, Any],
           repository_root: Path | str = ROOT,
           *, scheduler: Optional[Callable[[Sequence[str], Mapping[str, str]], Any]] = None,
           check_files: bool = True) -> Dict[str, Any]:
    """Render first, then invoke the explicitly supplied scheduler adapter."""
    report = render(profile, run, repository_root, check_files=check_files)
    if report["gate"] != gate:
        raise ReleaseError("SUBMIT_GATE_MISMATCH")
    if scheduler is None:
        def scheduler(argv: Sequence[str], env: Mapping[str, str]) -> Any:
            return subprocess.run(
                list(argv), cwd=str(repository_root), env=dict(env),
                text=True, capture_output=True, check=False,
            )
    result = scheduler(report["argv"], report["environment"])
    if hasattr(result, "returncode") and result.returncode != 0:
        raise ReleaseError("SCHEDULER_REJECTED:" + str(result.returncode))
    report["status"] = "SUBMITTED"
    return report


def _cli() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("render", "submit"))
    parser.add_argument("gate_or_profile")
    parser.add_argument("profile_or_run")
    parser.add_argument("run_or_root")
    parser.add_argument("root", nargs="?", default=str(ROOT))
    args = parser.parse_args()
    try:
        if args.operation == "render":
            profile = load_profile(args.gate_or_profile)
            run = load_json(args.profile_or_run)
            report = render(profile, run, args.run_or_root, check_files=True)
        else:
            profile = load_profile(args.profile_or_run)
            run = load_json(args.run_or_root)
            report = submit(args.gate_or_profile, profile, run, args.root)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (OSError, ReleaseError, TypeError, ValueError) as exc:
        print(str(exc), file=__import__("sys").stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(_cli())
