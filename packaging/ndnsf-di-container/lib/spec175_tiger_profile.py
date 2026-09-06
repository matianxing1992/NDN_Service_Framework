#!/usr/bin/env python3
"""Render and validate the single Spec175 Tiger submission profile.

The profile is deliberately small and deterministic.  It is not a second job
launcher: it turns one checked-in launch contract and one gate run record into
one explicit Slurm argv/environment pair.  No caller environment is consulted
for workload values.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
from typing import Any, Mapping


PROFILE_SCHEMA = "ndnsf-di-spec175-proven-tiger-profile-v1"
RUN_SCHEMA = "ndnsf-di-spec175-tiger-run-v1"
DELTA_SCHEMA = "ndnsf-di-spec175-proven-baseline-delta-v1"
GATES = {
    "control": {"job": "qualify-control.sbatch", "checklist": "control", "candidate": "G4T"},
    "stage-readiness": {"job": "qualify-stage-readiness.sbatch", "checklist": "stage-readiness", "candidate": "G5"},
    "multi-provider": {"job": "qualify-multiprovider.sbatch", "checklist": "functional", "candidate": "G6"},
    "conversation-residency": {"job": "qualify-conversation-residency.sbatch", "checklist": "functional", "candidate": "G6C"},
    "performance": {"job": "qualify-performance.sbatch", "checklist": "performance", "candidate": "G7"},
}

COMMON_RUN_KEYS = {
    "schema", "profileSha256", "gate", "runId", "candidate", "workload", "bundle", "output",
    "checklist", "checklistValidation", "profileDelta", "parameters", "model",
    "prerequisites",
}
CANDIDATE_KEYS = {"id", "closureManifest", "sif", "sifSha256", "remoteSif", "remoteSifSha256"}
PARAMETER_KEYS = {"providerCount", "gpuCount", "seed", "resourceEnvelope", "stageDeviceIds"}
MODEL_KEYS = {"manifest", "remoteRoot"}
COMMON_ALLOWED_DELTAS = {
    "candidate.id", "candidate.sif", "candidate.sifSha256", "candidate.remoteSif",
    "candidate.remoteSifSha256", "runId", "output", "bundle", "workload",
    "checklist", "checklistValidation", "profileDelta",
}
GATE_ALLOWED_DELTAS = {
    # Provider/GPU counts, seed, and Slurm resources are part of the proven
    # gate contract.  They are exported as fixed metadata, but a changed
    # value is rejected because the tracked job/bundle does not consume an
    # ambient override.  Stage device IDs are the only runtime parameter that
    # has an explicit checked-in consumer (the stage wrapper decodes it).
    "control": set(),
    "stage-readiness": {"parameters.stageDeviceIds", "model.manifest", "model.remoteRoot"},
    "multi-provider": {"model.manifest", "model.remoteRoot"},
    "conversation-residency": {"model.manifest", "model.remoteRoot"},
    "performance": {"model.manifest", "model.remoteRoot"},
}
VALUE_RE = re.compile(r"^[A-Za-z0-9_./:@%+=,-]+$")


class ProfileError(ValueError):
    """Raised for an invalid profile, run record, or unregistered delta."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _read_json(path: Path, schema: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProfileError(f"PROFILE_JSON_READ_FAILED:{path}:{exc}") from exc
    if not isinstance(value, dict) or value.get("schema") != schema:
        raise ProfileError(f"PROFILE_SCHEMA_MISMATCH:{path}:{schema}")
    return value


def _strict_keys(value: Mapping[str, Any], allowed: set[str], label: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ProfileError(f"{label}: unknown keys: {','.join(unknown)}")


def _nonempty(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ProfileError(f"{label}: nonempty string required")
    return value.strip()


def _digest(value: Any, label: str) -> str:
    value = _nonempty(value, label).lower()
    if value.startswith("sha256:"):
        value = value[7:]
    if len(value) != 64 or any(char not in "0123456789abcdef" for char in value):
        raise ProfileError(f"{label}: invalid sha256")
    return "sha256:" + value


def _without_prefix(value: str, prefix: str) -> str:
    return value[len(prefix):] if value.startswith(prefix) else value


def _path(value: Any, label: str, *, absolute: bool = True) -> str:
    value = _nonempty(value, label)
    if absolute and not Path(value).is_absolute():
        raise ProfileError(f"{label}: absolute path required")
    if any(char in value for char in ("\n", "\r", "\x00")):
        raise ProfileError(f"{label}: control character is forbidden")
    return value


def _file(value: Any, label: str) -> str:
    value = _path(value, label)
    if not Path(value).is_file():
        raise ProfileError(f"{label}: regular file required")
    return value


def load_profile(path: Path) -> dict[str, Any]:
    profile = _read_json(path, PROFILE_SCHEMA)
    _strict_keys(profile, {"schema", "profileId", "common", "gates", "trackedFiles"}, "profile")
    _nonempty(profile.get("profileId"), "profile.profileId")
    common = profile.get("common")
    if not isinstance(common, dict):
        raise ProfileError("profile.common: object required")
    _strict_keys(common, {"submitEntry", "jobRoot", "runner", "cwd", "apptainer", "exportMode", "fixedEnvironment", "fixedConfig"}, "profile.common")
    for key in ("submitEntry", "jobRoot", "runner", "cwd"):
        _nonempty(common.get(key), f"profile.common.{key}")
    if common.get("exportMode") != "NONE":
        raise ProfileError("profile.common.exportMode: must be NONE")
    apptainer = common.get("apptainer")
    if not isinstance(apptainer, dict):
        raise ProfileError("profile.common.apptainer: object required")
    _strict_keys(apptainer, {"path", "version", "args"}, "profile.common.apptainer")
    _path(apptainer.get("path"), "profile.common.apptainer.path")
    _nonempty(apptainer.get("version"), "profile.common.apptainer.version")
    if apptainer.get("args") != ["exec", "--cleanenv"]:
        raise ProfileError("profile.common.apptainer.args: expected exec --cleanenv")
    if not isinstance(common.get("fixedEnvironment"), list) or not common["fixedEnvironment"]:
        raise ProfileError("profile.common.fixedEnvironment: nonempty list required")
    if not isinstance(common.get("fixedConfig"), dict):
        raise ProfileError("profile.common.fixedConfig: object required")
    gates = profile.get("gates")
    if not isinstance(gates, dict) or set(gates) != set(GATES):
        raise ProfileError("profile.gates: exactly the five registered gates are required")
    for gate, expected in GATES.items():
        item = gates[gate]
        if not isinstance(item, dict):
            raise ProfileError(f"profile.gates.{gate}: object required")
        _strict_keys(item, {"job", "checklistGate", "candidateGate", "allowlist", "fixedConfig"}, f"profile.gates.{gate}")
        if item.get("job") != expected["job"] or item.get("checklistGate") != expected["checklist"] or item.get("candidateGate") != expected["candidate"]:
            raise ProfileError(f"profile.gates.{gate}: registered mapping mismatch")
        allow = item.get("allowlist")
        if not isinstance(allow, list) or set(allow) != COMMON_ALLOWED_DELTAS | GATE_ALLOWED_DELTAS[gate]:
            raise ProfileError(f"profile.gates.{gate}.allowlist: must equal registered allowlist")
        if not isinstance(item.get("fixedConfig"), dict):
            raise ProfileError(f"profile.gates.{gate}.fixedConfig: object required")
    tracked = profile.get("trackedFiles")
    if not isinstance(tracked, list) or not tracked:
        raise ProfileError("profile.trackedFiles: nonempty list required")
    seen: set[str] = set()
    for index, item in enumerate(tracked):
        if not isinstance(item, dict):
            raise ProfileError(f"profile.trackedFiles[{index}]: object required")
        _strict_keys(item, {"path", "sha256"}, f"profile.trackedFiles[{index}]")
        relative = _nonempty(item.get("path"), f"profile.trackedFiles[{index}].path")
        if Path(relative).is_absolute() or relative in seen:
            raise ProfileError(f"profile.trackedFiles[{index}].path: relative and unique required")
        seen.add(relative)
        _digest(item.get("sha256"), f"profile.trackedFiles[{index}].sha256")
    return profile


def load_run(path: Path, gate: str) -> dict[str, Any]:
    run = _read_json(path, RUN_SCHEMA)
    _strict_keys(run, COMMON_RUN_KEYS, "run")
    _digest(run.get("profileSha256"), "run.profileSha256")
    if run.get("gate") != gate or gate not in GATES:
        raise ProfileError("run.gate: does not match registered submission gate")
    _nonempty(run.get("runId"), "run.runId")
    candidate = run.get("candidate")
    if not isinstance(candidate, dict):
        raise ProfileError("run.candidate: object required")
    _strict_keys(candidate, CANDIDATE_KEYS, "run.candidate")
    _nonempty(candidate.get("id"), "run.candidate.id")
    for key in ("closureManifest", "sif"):
        _file(candidate.get(key), f"run.candidate.{key}")
    for key in ("sifSha256", "remoteSifSha256"):
        _digest(candidate.get(key), f"run.candidate.{key}")
    _path(candidate.get("remoteSif"), "run.candidate.remoteSif")
    for key in ("workload", "checklist", "checklistValidation"):
        _file(run.get(key), f"run.{key}")
    # The render command creates this report; submit requires it to exist and
    # rechecks its hashes against a fresh render.
    _path(run.get("profileDelta"), "run.profileDelta")
    _path(run.get("bundle"), "run.bundle")
    if not Path(run["bundle"]).is_dir():
        raise ProfileError("run.bundle: directory required")
    _path(run.get("output"), "run.output")
    params = run.get("parameters")
    if not isinstance(params, dict):
        raise ProfileError("run.parameters: object required")
    _strict_keys(params, PARAMETER_KEYS, "run.parameters")
    for key in ("providerCount", "gpuCount", "seed"):
        if not isinstance(params.get(key), int) or params[key] < 0:
            raise ProfileError(f"run.parameters.{key}: nonnegative integer required")
    _nonempty(params.get("resourceEnvelope"), "run.parameters.resourceEnvelope")
    if "stageDeviceIds" in params:
        devices = params["stageDeviceIds"]
        if not isinstance(devices, list) or not devices or any(not isinstance(item, int) or item < 0 for item in devices):
            raise ProfileError("run.parameters.stageDeviceIds: nonnegative integer list required")
        if gate != "stage-readiness":
            raise ProfileError("run.parameters.stageDeviceIds: only stage-readiness may set device IDs")
        if len(devices) != params["gpuCount"] or len(set(devices)) != len(devices):
            raise ProfileError("run.parameters.stageDeviceIds: must uniquely match gpuCount")
    elif gate == "stage-readiness":
        raise ProfileError("run.parameters.stageDeviceIds: required for stage-readiness")
    model = run.get("model")
    if gate == "control":
        if model not in (None, {}):
            raise ProfileError("run.model: control must omit model")
    else:
        if not isinstance(model, dict):
            raise ProfileError("run.model: object required for model gate")
        _strict_keys(model, MODEL_KEYS, "run.model")
        _path(model.get("manifest"), "run.model.manifest")
        _path(model.get("remoteRoot"), "run.model.remoteRoot")
        if not Path(model["manifest"]).is_file():
            raise ProfileError("run.model.manifest: regular file required")
    prerequisites = run.get("prerequisites", {})
    if not isinstance(prerequisites, dict):
        raise ProfileError("run.prerequisites: object required")
    allowed_prereq = {"G4T", "G5", "G6", "G6C"}
    if set(prerequisites) - allowed_prereq:
        raise ProfileError("run.prerequisites: unknown gate")
    for name, value in prerequisites.items():
        _file(value, f"run.prerequisites.{name}")
    return run


def _flatten(mapping: Mapping[str, Any], prefix: str = "") -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in mapping.items():
        name = f"{prefix}.{key}" if prefix else key
        if isinstance(value, dict):
            output.update(_flatten(value, name))
        else:
            output[name] = value
    return output


def _tracked_file_digests(profile: dict[str, Any], repository_root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in profile["trackedFiles"]:
        relative = item["path"]
        path = repository_root / relative
        if not path.is_file():
            raise ProfileError(f"PROFILE_TRACKED_FILE_MISSING:{relative}")
        actual = sha256_file(path)
        expected = _digest(item["sha256"], f"trackedFiles.{relative}")
        if actual != expected:
            raise ProfileError(f"PROFILE_TRACKED_FILE_CHANGED:{relative}")
        result[relative] = actual
    return result


def _dynamic_values(run: dict[str, Any]) -> dict[str, Any]:
    values: dict[str, Any] = {
        "candidate.id": run["candidate"]["id"],
        "candidate.sif": run["candidate"]["sif"],
        "candidate.sifSha256": run["candidate"]["sifSha256"],
        "candidate.remoteSif": run["candidate"]["remoteSif"],
        "candidate.remoteSifSha256": run["candidate"]["remoteSifSha256"],
        "runId": run["runId"],
        "output": run["output"],
        "bundle": run["bundle"],
        "workload": run["workload"],
        "checklist": run["checklist"],
        "checklistValidation": run["checklistValidation"],
        "profileDelta": run["profileDelta"],
    }
    for key, value in run["parameters"].items():
        values[f"parameters.{key}"] = value
    if isinstance(run.get("model"), dict):
        for key, value in run["model"].items():
            values[f"model.{key}"] = value
    return values


def render_effective_config(profile: dict[str, Any], run: dict[str, Any], repository_root: Path) -> dict[str, Any]:
    gate = run["gate"]
    common = profile["common"]
    policy = profile["gates"][gate]
    job = repository_root / common["jobRoot"] / policy["job"]
    if not job.is_file():
        raise ProfileError(f"PROFILE_JOB_MISSING:{job}")
    directives: dict[str, str] = {}
    for line in job.read_text(encoding="utf-8").splitlines():
        if line.startswith("#SBATCH --") and "=" in line:
            key, value = line[len("#SBATCH --"):].split("=", 1)
            directives[key] = value
    fixed = policy["fixedConfig"]
    if fixed.get("jobName") and directives.get("job-name") != fixed["jobName"]:
        raise ProfileError(f"PROFILE_JOB_DIRECTIVE_MISMATCH:job-name:{job}")
    if fixed.get("resourceEnvelope"):
        fields = ["nodes", "ntasks", "gres", "mem", "time"]
        actual_envelope = ";".join(
            f"{field}={directives[field]}" for field in fields if field in directives
        )
        if actual_envelope != fixed["resourceEnvelope"]:
            raise ProfileError(f"PROFILE_JOB_RESOURCE_MISMATCH:{job}")
    return {
        "profileId": profile["profileId"],
        "gate": gate,
        "job": str(job),
        "jobRelative": policy["job"],
        "checklistGate": policy["checklistGate"],
        "candidateGate": policy["candidateGate"],
        "common": {
            "submitEntry": common["submitEntry"],
            "jobRoot": common["jobRoot"],
            "runner": common["runner"],
            "cwd": common["cwd"],
            "apptainer": common["apptainer"],
            "exportMode": common["exportMode"],
            "fixedEnvironment": common["fixedEnvironment"],
            "fixedConfig": common["fixedConfig"],
        },
        "gateFixedConfig": policy["fixedConfig"],
        "dynamic": _dynamic_values(run),
        "trackedFileDigests": _tracked_file_digests(profile, repository_root),
    }


def _validate_dynamic_deltas(profile: dict[str, Any], run: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    values = _dynamic_values(run)
    allowed = set(profile["gates"][run["gate"]]["allowlist"])
    errors: list[dict[str, Any]] = []
    allowlisted: list[dict[str, Any]] = []
    fixed = profile["gates"][run["gate"]]["fixedConfig"]
    for key, value in sorted(values.items()):
        fixed_parameter = key.startswith("parameters.") and key[11:] in fixed
        if key not in allowed and not fixed_parameter:
            errors.append({"field": key, "value": value, "reason": "UNREGISTERED_DELTA"})
        if isinstance(value, str) and ("\n" in value or "\r" in value or "\x00" in value):
            errors.append({"field": key, "reason": "CONTROL_CHARACTER"})
        if isinstance(value, str) and key not in {"output", "bundle", "workload", "checklist", "checklistValidation", "profileDelta", "candidate.sif", "candidate.remoteSif", "model.manifest", "model.remoteRoot", "parameters.resourceEnvelope"} and not VALUE_RE.fullmatch(value):
            errors.append({"field": key, "value": value, "reason": "UNSAFE_VALUE"})
        if key.startswith("parameters.") and key[11:] in fixed and value != fixed[key[11:]]:
            if key in allowed:
                allowlisted.append({"field": key, "baseline": fixed[key[11:]], "value": value, "reason": "ALLOWLISTED_DELTA"})
            else:
                errors.append({"field": key, "value": value, "reason": "UNREGISTERED_DELTA"})
    return errors, allowlisted


def render_sbatch_argv(profile: dict[str, Any], run: dict[str, Any], repository_root: Path, profile_path: Path) -> tuple[list[str], dict[str, str], dict[str, Any]]:
    if run.get("profileSha256") != canonical_digest(profile):
        raise ProfileError("PROFILE_DIGEST_MISMATCH")
    effective = render_effective_config(profile, run, repository_root)
    errors, allowlisted_deltas = _validate_dynamic_deltas(profile, run)
    if errors:
        raise ProfileError("PROFILE_DELTA_REJECTED:" + json.dumps(errors, sort_keys=True))
    common = profile["common"]
    policy = profile["gates"][run["gate"]]
    candidate = run["candidate"]
    exports = {
        "SPEC175_GATE": run["gate"],
        "SPEC175_CANDIDATE_GATE": policy["candidateGate"],
        "SPEC175_CHECKLIST_GATE": policy["checklistGate"],
        "SPEC175_PROFILE_ID": profile["profileId"],
        "SPEC175_PROFILE_SHA256": canonical_digest(profile),
        "SPEC175_CLOSURE_MANIFEST": candidate["closureManifest"],
        "SPEC175_LOCAL_SIF": candidate["sif"],
        "SPEC175_LOCAL_SIF_SHA256": _without_prefix(candidate["sifSha256"], "sha256:"),
        "SPEC175_REMOTE_SIF": candidate["remoteSif"],
        "SPEC175_REMOTE_SIF_SHA256": _without_prefix(candidate["remoteSifSha256"], "sha256:"),
        "SPEC175_WORKLOAD": run["workload"],
        "SPEC175_BUNDLE": run["bundle"],
        "SPEC175_OUTPUT": run["output"],
        "SPEC175_PRE_TIGER_CHECKLIST": run["checklist"],
        "SPEC175_PRE_TIGER_CHECKLIST_VALIDATION": run["checklistValidation"],
        "SPEC175_PROFILE_DELTA": run["profileDelta"],
        "SPEC175_RUN_ID": run["runId"],
        "SPEC175_PROVIDER_COUNT": str(run["parameters"]["providerCount"]),
        "SPEC175_GPU_COUNT": str(run["parameters"]["gpuCount"]),
        "SPEC175_SEED": str(run["parameters"]["seed"]),
        "SPEC175_RESOURCE_ENVELOPE": run["parameters"]["resourceEnvelope"],
    }
    if run.get("model"):
        exports["SPEC175_MODEL_MANIFEST"] = run["model"]["manifest"]
        exports["SPEC175_REMOTE_MODEL_ROOT"] = run["model"]["remoteRoot"]
    if "stageDeviceIds" in run["parameters"]:
        # Slurm's --export list is comma-delimited.  Transport stage IDs with
        # colons and decode them inside the checked-in stage wrapper.
        exports["SPEC175_STAGE_DEVICE_IDS"] = ":".join(str(item) for item in run["parameters"]["stageDeviceIds"])
    for key, value in exports.items():
        if not isinstance(value, str) or "," in value:
            raise ProfileError(f"PROFILE_EXPORT_UNSAFE_VALUE:{key}")
    export_arg = "--export=NONE," + ",".join(f"{key}={exports[key]}" for key in sorted(exports))
    argv = ["sbatch", "--parsable", export_arg, str(repository_root / common["jobRoot"] / policy["job"])]
    effective["exports"] = exports
    effective["allowlistedDeltas"] = allowlisted_deltas
    effective["argv"] = argv
    effective["argvSha256"] = canonical_digest(argv)
    effective["profilePath"] = str(profile_path)
    return argv, exports, effective


def validate_and_render(profile_path: Path, run_path: Path, repository_root: Path) -> dict[str, Any]:
    profile = load_profile(profile_path)
    try:
        raw = json.loads(run_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProfileError(f"RUN_RECORD_READ_FAILED:{run_path}:{exc}") from exc
    run = load_run(run_path, raw.get("gate", ""))
    argv, exports, effective = render_sbatch_argv(profile, run, repository_root, profile_path)
    return {
        "schema": DELTA_SCHEMA,
        "status": "PASS",
        "profileId": profile["profileId"],
        "profileSha256": canonical_digest(profile),
        "runId": run["runId"],
        "gate": run["gate"],
        "allowedDeltas": profile["gates"][run["gate"]]["allowlist"],
        "differences": effective.get("allowlistedDeltas", []),
        "provenBaselineExactDelta": "PASS",
        "effectiveConfig": effective,
        "effectiveConfigSha256": canonical_digest(effective),
        "argv": argv,
        "argvSha256": canonical_digest(argv),
        "sealedEnvironment": exports,
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
