"""Fail-closed allocation contracts for the Spec 170 D0/D1/D2 gates.

The module deliberately validates the allocation *contract* before a job calls
Apptainer.  It does not infer a GPU, remap a topology, or turn a two-Provider
profile into a one-Provider multi-GPU profile.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any, Mapping


class Spec170TopologyError(ValueError):
    pass


DIGEST = re.compile(r"^sha256:[a-f0-9]{64}$")
SAFE = re.compile(r"^[A-Za-z0-9._/@:+,-]+$")


GATE_CONTRACTS = {
    "d0-cpu": {"gpuCount": 0, "providerCount": 1, "nodeCount": 1, "nv": False},
    "d1-single": {"gpuCount": 1, "providerCount": 1, "nodeCount": 1, "nv": True},
    "d2a-local-two-gpu": {"gpuCount": 2, "providerCount": 1, "nodeCount": 1, "nv": True},
    "d2b-cross-provider": {"gpuCount": 2, "providerCount": 2, "nodeCount": 2, "nv": True},
    "d2h-hybrid": {"gpuCount": 2, "providerCount": 2, "nodeCount": 2, "nv": True},
}


def _fail(code: str, detail: object = "") -> None:
    suffix = ":" + str(detail) if detail != "" else ""
    raise Spec170TopologyError(code + suffix)


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path | str) -> str:
    source = Path(path)
    try:
        return digest_bytes(source.read_bytes())
    except OSError as exc:
        _fail("SPEC170_SIF_READ_FAILED", exc)
    raise AssertionError("unreachable")


def _digest(value: object, field: str) -> str:
    if not isinstance(value, str) or DIGEST.fullmatch(value) is None:
        _fail("SPEC170_DIGEST_INVALID", field)
    return value


def validate_gate_profile(profile: Mapping[str, Any]) -> dict[str, Any]:
    required = {
        "schemaVersion", "gate", "sourceDigest", "ociDigest", "sifPath",
        "sifSha256", "nodeCount", "gpuCount", "providerCount", "nv",
        "providerPlacement", "workload", "hiddenDefaults",
    }
    if not isinstance(profile, Mapping) or set(profile) != required:
        _fail("SPEC170_TOPOLOGY_FIELDS_INVALID")
    if profile["schemaVersion"] != "spec170-allocation-topology-v1":
        _fail("SPEC170_TOPOLOGY_SCHEMA_INVALID")
    gate = profile["gate"]
    if gate not in GATE_CONTRACTS:
        _fail("SPEC170_GATE_INVALID", gate)
    expected = GATE_CONTRACTS[gate]
    for field in ("sourceDigest", "ociDigest", "sifSha256"):
        _digest(profile[field], field)
    sif_path = profile["sifPath"]
    if not isinstance(sif_path, str) or not sif_path or not SAFE.fullmatch(sif_path):
        _fail("SPEC170_SIF_PATH_INVALID")
    if not isinstance(profile["workload"], str) or not profile["workload"]:
        _fail("SPEC170_WORKLOAD_INVALID")
    if profile["hiddenDefaults"] is not False:
        _fail("SPEC170_HIDDEN_DEFAULTS_FORBIDDEN")
    for field in ("nodeCount", "gpuCount", "providerCount"):
        if not isinstance(profile[field], int) or profile[field] < 0:
            _fail("SPEC170_COUNT_INVALID", field)
    if any(profile[field] != expected[field] for field in ("nodeCount", "gpuCount", "providerCount", "nv")):
        _fail("SPEC170_ALLOCATION_CONTRACT_MISMATCH", gate)
    placement = profile["providerPlacement"]
    allowed_placement = {
        "d0-cpu": "one-provider-cpu",
        "d1-single": "one-provider-one-gpu",
        "d2a-local-two-gpu": "one-provider-two-gpu",
        "d2b-cross-provider": "two-provider-one-gpu-each",
        "d2h-hybrid": "two-provider-hybrid",
    }[gate]
    if placement != allowed_placement:
        _fail("SPEC170_PROVIDER_PLACEMENT_MISMATCH", gate)
    return dict(profile)


def validate_exact_sif(profile: Mapping[str, Any], *, require_file: bool = True) -> dict[str, Any]:
    value = validate_gate_profile(profile)
    path = Path(value["sifPath"])
    if require_file and not path.is_file():
        _fail("SPEC170_SIF_MISSING", path)
    if path.is_file() and digest_file(path) != value["sifSha256"]:
        _fail("SPEC170_SIF_TAMPERED", path)
    return {"status": "PASS", "gate": value["gate"], "sifSha256": value["sifSha256"]}


def canonical_profile_digest(profile: Mapping[str, Any]) -> str:
    value = validate_gate_profile(profile)
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return digest_bytes(encoded)


__all__ = [
    "GATE_CONTRACTS", "Spec170TopologyError", "canonical_profile_digest",
    "digest_file", "validate_exact_sif", "validate_gate_profile",
]
