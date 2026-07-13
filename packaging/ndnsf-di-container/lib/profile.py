"""Load and validate NDNSF-DI container deployment profiles."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
from typing import Any, Mapping

import yaml

from schema_utils import SchemaValidationError, validate_schema


class ProfileError(ValueError):
    """Profile syntax, schema, or cross-field policy failure."""


ENV_ALLOWLIST = frozenset({"HOME", "USER", "SLURM_JOB_ID", "NDNSF_RUN_ID"})
ENV_PATTERN = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def _expand(value: Any, environment: Mapping[str, str]) -> Any:
    if isinstance(value, dict):
        return {key: _expand(item, environment) for key, item in value.items()}
    if isinstance(value, list):
        return [_expand(item, environment) for item in value]
    if not isinstance(value, str):
        return value

    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in ENV_ALLOWLIST:
            raise ProfileError(f"PROFILE_ENV_NOT_ALLOWED:{name}")
        if name not in environment:
            raise ProfileError(f"PROFILE_ENV_MISSING:{name}")
        return environment[name]

    return ENV_PATTERN.sub(replace, value)


def _cross_validate(profile: dict[str, Any]) -> None:
    runtime = profile["runtime"]["kind"]
    backend = profile["backend"]
    if backend["requested"] == "cpu" and backend["allowCpuFallback"]:
        raise ProfileError("PROFILE_CPU_FALLBACK_MEANINGLESS")
    if runtime == "slurm-apptainer":
        storage = profile["storage"]
        if any(storage.get(key, "").startswith("/home/") for key in ("projectRoot", "imageRoot", "modelRoot", "evidenceRoot")):
            raise ProfileError("PROFILE_ITIGER_BULK_STORAGE_UNDER_HOME")
        slurm = profile["slurm"]
        if backend["requested"] != "cpu" and slurm["gpu"]["count"] < 1:
            raise ProfileError("PROFILE_GPU_REQUIRES_GRES")
        if slurm["nodes"] > 1:
            network = profile["network"]
            if network["topology"] != "multi-node-allocation" or not network.get("preflightEvidence"):
                raise ProfileError("PROFILE_MULTINODE_NETWORK_EVIDENCE_REQUIRED")


def validate_profile(profile: dict[str, Any]) -> dict[str, Any]:
    try:
        validate_schema(profile, "deployment-profile.schema.json")
    except SchemaValidationError as exc:
        raise ProfileError(f"PROFILE_INVALID:{exc}") from exc
    _cross_validate(profile)
    return profile


def load_profile(path: Path | str, environment: Mapping[str, str] | None = None) -> dict[str, Any]:
    source = Path(path)
    try:
        raw = yaml.safe_load(source.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ProfileError(f"PROFILE_READ_FAILED:{source}:{exc}") from exc
    if not isinstance(raw, dict):
        raise ProfileError("PROFILE_INVALID:$: expected object")
    expanded = _expand(raw, dict(os.environ if environment is None else environment))
    return validate_profile(expanded)


def profile_digest(profile: Mapping[str, Any]) -> str:
    encoded = json.dumps(profile, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()
