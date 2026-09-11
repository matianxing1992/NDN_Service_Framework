#!/usr/bin/env python3
"""Dispatch one candidate-bound Spec180 workload inside the sealed image.

This file is deliberately a small execution boundary, not another NDNSF
runner.  It validates the immutable workload and the fixed mount points, then
execs the maintained YOLO or Spec175 entrypoint with an allow-listed
environment.  Provider names, role assignments, ACK decisions, and model
boundaries remain owned by the protocol runner and its authenticated plan.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Mapping, Sequence


WORKLOAD_SCHEMA = "spec180-dispatch-workload-v1"
EVIDENCE_SCHEMA = "spec180-result-v1"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_KEY_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
_SHELL_RE = re.compile(r"[\x00\r\n;&|$`(){}<>`]")
_SECRET_RE = re.compile(
    r"(?i)(private[_ -]?key|secret|password|plaintext|token|credential|"
    r"input[_ -]?bytes|result[_ -]?bytes)"
)

WORKLOAD_FIELDS = frozenset({
    "schema", "gate", "case", "entrypoint", "args", "environment",
    "evidenceSchema",
})

YOLO_ENVIRONMENT_FIELDS = frozenset({
    "SPEC180_YOLO_CANONICAL_PACKAGE",
    "SPEC180_YOLO_CATALOGUE_REGISTRY",
    "SPEC180_YOLO_CATALOG_DATA_NAME",
    "SPEC180_YOLO_CATALOG_SIGNER",
    "SPEC180_YOLO_OFFER_TRUST_ROOT",
    "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP",
    "SPEC180_YOLO_NATIVE_REQUESTER_CONFIG",
    "SPEC180_YOLO_TOPOLOGY",
    "SPEC180_YOLO_CONFIG",
})

# These names are metadata bindings for the external QWEN-F runner; no ambient
# host path or private credential is accepted. The external 27B qualification
# is deliberately not routed to the Spec175 local wrapper: doing so would
# silently run the tiny fixture under a QWEN-F label.
QWEN_ENVIRONMENT_FIELDS = frozenset({
    "SPEC180_QWEN_MODEL",
    "SPEC180_QWEN_REVISION",
    "SPEC180_QWEN_MODEL_IDENTITY_DIGEST",
    "SPEC180_QWEN_PROMPT_DIGEST",
})

_PATH_FIELDS = frozenset({
    "SPEC180_YOLO_CANONICAL_PACKAGE",
    "SPEC180_YOLO_CATALOGUE_REGISTRY",
    "SPEC180_YOLO_OFFER_TRUST_ROOT",
    "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP",
    "SPEC180_YOLO_NATIVE_REQUESTER_CONFIG",
    "SPEC180_YOLO_TOPOLOGY",
    "SPEC180_YOLO_CONFIG",
})
_QWEN_DIGEST_FIELDS = frozenset({
    "SPEC180_QWEN_MODEL_IDENTITY_DIGEST",
    "SPEC180_QWEN_PROMPT_DIGEST",
})

_REGISTERED = {
    "yolo-functional": {
        "case": "Y-B",
        "entrypoint": "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py",
        "args": ("--case", "Y-B"),
    },
    "qwen-functional": {
        "case": "QWEN-F",
        # This is the dedicated production entrypoint for the external
        # Qwen3.6-27B ONNX workload. It validates the signed manifest/object
        # set itself; routing to the Spec175 M11 wrapper would be a false
        # qualification of the tiny local fixture.
        "entrypoint": "Experiments/NDNSF_DI_QwenAckDriven_Minindn.py",
        "args": ("--case", "QWEN-F"),
    },
}

_FIXED_RUNTIME_FIELDS = frozenset({
    "SPEC180_GATE", "SPEC180_PROFILE_ID", "SPEC180_PROFILE_SHA256",
    "SPEC180_RUN_ID", "SPEC180_CANDIDATE_ID", "SPEC180_CANDIDATE_DIGEST",
    "SPEC180_SIF", "SPEC180_SIF_SHA256", "SPEC180_MODEL_MANIFEST",
    "SPEC180_MODEL_MANIFEST_SHA256", "SPEC180_MODEL_ROOT",
    "SPEC180_WORKLOAD", "SPEC180_WORKLOAD_SHA256", "SPEC180_OUTPUT_ROOT",
    "SPEC180_CASE_OUTPUT_DIR",
})


class DispatcherError(ValueError):
    """Raised when the sealed-image dispatch contract is not satisfied."""


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise DispatcherError("INVALID_DIGEST:" + label)
    return value


def _load_json(path: Path) -> tuple[Mapping[str, Any], bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise DispatcherError("WORKLOAD_READ_FAILED") from exc
    try:
        value = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_fields,
        )
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise DispatcherError("WORKLOAD_NOT_JSON") from exc
    if not isinstance(value, Mapping):
        raise DispatcherError("WORKLOAD_NOT_OBJECT")
    return value, raw


def _reject_duplicate_fields(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise json.JSONDecodeError("duplicate field", "", 0)
        result[key] = value
    return result


def _check_relative_entrypoint(value: Any) -> str:
    if not isinstance(value, str) or not value:
        raise DispatcherError("ENTRYPOINT_INVALID")
    path = Path(value)
    if path.is_absolute() or "\\" in value or ".." in path.parts:
        raise DispatcherError("ENTRYPOINT_PATH_UNSAFE")
    if value.startswith("./") or value.endswith("/"):
        raise DispatcherError("ENTRYPOINT_PATH_UNSAFE")
    return path.as_posix()


def _check_argument_vector(value: Any, expected: Sequence[str], label: str) -> tuple[str, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise DispatcherError("ARGS_INVALID:" + label)
    if any(_SHELL_RE.search(item) for item in value):
        raise DispatcherError("ARGS_SHELL_FRAGMENT:" + label)
    if tuple(value) != tuple(expected):
        raise DispatcherError("ARGS_NOT_FIXED:" + label)
    return tuple(value)


def _check_environment(value: Any, gate: str) -> dict[str, str]:
    if not isinstance(value, Mapping):
        raise DispatcherError("ENVIRONMENT_NOT_OBJECT")
    allowed = (YOLO_ENVIRONMENT_FIELDS if gate == "yolo-functional"
               else QWEN_ENVIRONMENT_FIELDS)
    keys = list(value)
    if any(not isinstance(key, str) or not _KEY_RE.fullmatch(key)
           for key in keys):
        raise DispatcherError("ENVIRONMENT_KEY_INVALID")
    unknown = sorted(set(keys) - allowed)
    if unknown:
        raise DispatcherError("ENVIRONMENT_FIELD_FORBIDDEN:" + ",".join(unknown))
    result: dict[str, str] = {}
    for key, raw in value.items():
        if not isinstance(raw, str) or not raw:
            raise DispatcherError("ENVIRONMENT_VALUE_INVALID:" + key)
        if _SHELL_RE.search(raw) or _SECRET_RE.search(raw):
            raise DispatcherError("ENVIRONMENT_VALUE_FORBIDDEN:" + key)
        if key in _QWEN_DIGEST_FIELDS:
            _require_digest(raw, key)
        if key in _PATH_FIELDS:
            path = Path(raw)
            if (not path.is_absolute() or ".." in path.parts or
                    not (raw == "/bundle" or raw.startswith("/bundle/") or
                         raw == "/models" or raw.startswith("/models/"))):
                raise DispatcherError("ENVIRONMENT_PATH_UNSAFE:" + key)
        result[key] = raw
    if gate == "yolo-functional" and set(result) != set(YOLO_ENVIRONMENT_FIELDS):
        missing = sorted(YOLO_ENVIRONMENT_FIELDS - set(result))
        raise DispatcherError("ENVIRONMENT_FIELD_MISSING:" + ",".join(missing))
    if gate == "qwen-functional" and set(result) != set(QWEN_ENVIRONMENT_FIELDS):
        missing = sorted(QWEN_ENVIRONMENT_FIELDS - set(result))
        raise DispatcherError("ENVIRONMENT_FIELD_MISSING:" + ",".join(missing))
    return result


def validate_workload(workload: Mapping[str, Any], gate: str) -> dict[str, Any]:
    """Validate the closed dispatch schema and return a normalized copy."""
    if gate not in _REGISTERED:
        raise DispatcherError("GATE_INVALID:" + gate)
    if not isinstance(workload, Mapping):
        raise DispatcherError("WORKLOAD_NOT_OBJECT")
    unknown = sorted(set(workload) - WORKLOAD_FIELDS)
    missing = sorted(WORKLOAD_FIELDS - set(workload))
    if unknown:
        raise DispatcherError("WORKLOAD_FIELD_UNKNOWN:" + ",".join(unknown))
    if missing:
        raise DispatcherError("WORKLOAD_FIELD_MISSING:" + ",".join(missing))
    if workload.get("schema") != WORKLOAD_SCHEMA:
        raise DispatcherError("WORKLOAD_SCHEMA_UNSUPPORTED")
    if workload.get("gate") != gate:
        raise DispatcherError("WORKLOAD_GATE_MISMATCH")
    registered = _REGISTERED[gate]
    if workload.get("case") != registered["case"]:
        raise DispatcherError("WORKLOAD_CASE_MISMATCH")
    entrypoint = _check_relative_entrypoint(workload.get("entrypoint"))
    if entrypoint != registered["entrypoint"]:
        raise DispatcherError("ENTRYPOINT_NOT_REGISTERED")
    args = _check_argument_vector(workload.get("args"), registered["args"], gate)
    if workload.get("evidenceSchema") != EVIDENCE_SCHEMA:
        raise DispatcherError("EVIDENCE_SCHEMA_MISMATCH")
    environment = _check_environment(workload.get("environment"), gate)
    return {
        "schema": WORKLOAD_SCHEMA,
        "gate": gate,
        "case": registered["case"],
        "entrypoint": entrypoint,
        "args": list(args),
        "environment": environment,
        "evidenceSchema": EVIDENCE_SCHEMA,
    }


def load_and_validate_workload(path: Path, expected_digest: str,
                               gate: str) -> dict[str, Any]:
    """Read and digest-check the mounted workload before any child starts."""
    expected = _require_digest(expected_digest, "workload")
    workload, raw = _load_json(path)
    actual = digest_bytes(raw)
    if actual != expected:
        raise DispatcherError("WORKLOAD_DIGEST_MISMATCH")
    return validate_workload(workload, gate)


def resolve_entrypoint(bundle_root: Path, relative: str) -> Path:
    """Resolve a registered entrypoint beneath the image's `/bundle` root."""
    if not bundle_root.is_absolute():
        raise DispatcherError("BUNDLE_ROOT_NOT_ABSOLUTE")
    resolved = (bundle_root / _check_relative_entrypoint(relative)).resolve()
    try:
        resolved.relative_to(bundle_root.resolve())
    except ValueError as exc:
        raise DispatcherError("ENTRYPOINT_PATH_ESCAPES_BUNDLE") from exc
    if not resolved.is_file():
        raise DispatcherError("ENTRYPOINT_MISSING")
    return resolved


def _require_runtime_arg(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or _SHELL_RE.search(value):
        raise DispatcherError("RUNTIME_ARGUMENT_INVALID:" + label)
    return value


def build_child_environment(workload_environment: Mapping[str, str],
                            runtime: Mapping[str, str], output_root: Path,
                            *, python_path: str = "/bundle/NDNSF-DistributedInference:/bundle/pythonWrapper"
                            ) -> dict[str, str]:
    """Build the closed child environment; ambient host variables are dropped."""
    runtime_values = {
        key: _require_runtime_arg(runtime.get(key), key)
        for key in _FIXED_RUNTIME_FIELDS if key in runtime
    }
    if runtime_values.get("SPEC180_OUTPUT_ROOT") != str(output_root):
        raise DispatcherError("OUTPUT_ROOT_RUNTIME_MISMATCH")
    if set(workload_environment) & set(runtime_values):
        raise DispatcherError("ENVIRONMENT_RUNTIME_OVERRIDE")
    result = {
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "PYTHONPATH": python_path,
        "HOME": "/tmp/spec180-home",
        **runtime_values,
        **{str(key): str(value) for key, value in workload_environment.items()},
        "SPEC180_CASE_OUTPUT_DIR": str(output_root),
    }
    return result


def build_dispatch_command(workload: Mapping[str, Any], bundle_root: Path,
                           *, python_executable: str | None = None
                           ) -> list[str]:
    entrypoint = resolve_entrypoint(bundle_root, str(workload["entrypoint"]))
    interpreter = python_executable or sys.executable
    if not Path(interpreter).is_absolute():
        raise DispatcherError("PYTHON_EXECUTABLE_NOT_ABSOLUTE")
    return [interpreter, str(entrypoint), *[str(item) for item in workload["args"]]]


def _validate_mount_args(args: argparse.Namespace, environment: Mapping[str, str]) -> None:
    expected = {
        "sif": "/inputs/candidate.sif",
        "model_manifest": "/inputs/model-manifest.json",
        "model_root": "/models",
        "workload": "/inputs/workload.json",
        "output_root": "/evidence",
    }
    for name, value in expected.items():
        if getattr(args, name) != value:
            raise DispatcherError("MOUNT_PATH_MISMATCH:" + name)
    if environment.get("SPEC180_WORKLOAD") not in (None, args.workload):
        raise DispatcherError("WORKLOAD_ENVIRONMENT_PATH_MISMATCH")
    if environment.get("SPEC180_OUTPUT_ROOT") not in (None, args.output_root):
        raise DispatcherError("OUTPUT_ENVIRONMENT_PATH_MISMATCH")


def dispatch(args: argparse.Namespace, environment: Mapping[str, str] | None = None) -> None:
    """Validate all mounted inputs and replace this process with the runner."""
    env = dict(os.environ if environment is None else environment)
    _validate_mount_args(args, env)
    if not Path(args.output_root).is_dir():
        raise DispatcherError("OUTPUT_ROOT_MISSING")
    try:
        if any(Path(args.output_root).iterdir()):
            raise DispatcherError("OUTPUT_ROOT_NOT_EMPTY")
    except OSError as exc:
        raise DispatcherError("OUTPUT_ROOT_UNREADABLE") from exc
    for label, value in (
        ("candidateId", args.candidate_id),
        ("runId", args.run_id),
        ("profileId", args.profile_id),
    ):
        _require_runtime_arg(value, label)
    _require_digest(args.profile_sha256, "profile")
    _require_digest(args.candidate_digest, "candidate")
    _require_digest(args.sif_sha256, "sif")
    _require_digest(args.model_manifest_sha256, "model-manifest")
    workload = load_and_validate_workload(
        Path(args.workload), args.workload_sha256, args.gate)
    workload_environment = workload["environment"]
    runtime = {
        "SPEC180_GATE": args.gate,
        "SPEC180_PROFILE_ID": args.profile_id,
        "SPEC180_PROFILE_SHA256": args.profile_sha256,
        "SPEC180_RUN_ID": args.run_id,
        "SPEC180_CANDIDATE_ID": args.candidate_id,
        "SPEC180_CANDIDATE_DIGEST": args.candidate_digest,
        "SPEC180_SIF": args.sif,
        "SPEC180_SIF_SHA256": args.sif_sha256,
        "SPEC180_MODEL_MANIFEST": args.model_manifest,
        "SPEC180_MODEL_MANIFEST_SHA256": args.model_manifest_sha256,
        "SPEC180_MODEL_ROOT": args.model_root,
        "SPEC180_WORKLOAD": args.workload,
        "SPEC180_WORKLOAD_SHA256": args.workload_sha256,
        "SPEC180_OUTPUT_ROOT": args.output_root,
    }
    child_env = build_child_environment(
        workload_environment, runtime, Path(args.output_root))
    command = build_dispatch_command(workload, Path("/bundle"))
    print(
        "SPEC180_DISPATCH_START "
        f"gate={args.gate} case={workload['case']} "
        f"entrypoint={workload['entrypoint']} workload={args.workload}",
        flush=True,
    )
    os.execvpe(command[0], command, child_env)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gate", choices=tuple(_REGISTERED), required=True)
    parser.add_argument("--profile-id", required=True)
    parser.add_argument("--profile-sha256", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--candidate-digest", required=True)
    parser.add_argument("--sif", required=True)
    parser.add_argument("--sif-sha256", required=True)
    parser.add_argument("--model-manifest", required=True)
    parser.add_argument("--model-manifest-sha256", required=True)
    parser.add_argument("--model-root", required=True)
    parser.add_argument("--workload", required=True)
    parser.add_argument("--output-root", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        dispatch(args)
    except DispatcherError as exc:
        print("SPEC180_DISPATCH status=UNQUALIFIED error=" + str(exc),
              file=sys.stderr, flush=True)
        return 78
    return 0  # pragma: no cover - os.execvpe does not return on success


if __name__ == "__main__":
    raise SystemExit(main())
