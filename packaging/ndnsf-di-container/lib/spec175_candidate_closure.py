#!/usr/bin/env python3
"""Validate the immutable Spec175 promotion candidate before dispatch.

This module is deliberately dependency-light and side-effect free while it
validates a candidate.  The command-line wrapper may write a report, but the
validation itself never creates directories, contacts Tiger, uploads files,
stages a model, or invokes a scheduler.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


SCHEMA = "ndnsf-di-spec175-candidate-closure-v1"
PLANES = {"sealed-runtime", "host-replay", "tiger-submit", "model-workload"}
GATES = {"G0", "G1", "G2", "G3", "G4", "G4T", "G5", "G6", "G6C", "G7"}
TUPLE_COMPONENTS = (
    "sourceSeal",
    "exactSif",
    "hostReplay",
    "submitBundle",
    "workloadConfig",
    "modelArtifacts",
    "validationContract",
)
TERMINAL_COMPONENTS = ("G0", "G1", "G2", "G3", "G4")
CLOSURE_KINDS = (
    "helper",
    "environment",
    "interpreter",
    "argument",
    "cwd",
    "artifact",
    "sidecar",
    "mount",
    "identity",
    "token",
    "policy",
    "isolated-root",
    "startup",
    "timeout",
    "resource",
)


class CandidateClosureError(ValueError):
    """Raised when a candidate closure is incomplete or inconsistent."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _digest(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    value = value.lower()
    if value.startswith("sha256:"):
        value = value[7:]
    if len(value) != 64 or any(char not in "0123456789abcdef" for char in value):
        return None
    return "sha256:" + value


def _resolve(path_value: Any, base: Path, label: str, errors: list[str]) -> Path | None:
    if not isinstance(path_value, str) or not path_value.strip():
        errors.append(f"{label}: path is required")
        return None
    path = Path(path_value).expanduser()
    if not path.is_absolute():
        path = base / path
    path = path.resolve()
    if not path.is_file():
        errors.append(f"{label}: missing regular file: {path}")
        return None
    if path.stat().st_size == 0:
        errors.append(f"{label}: file is empty: {path}")
        return None
    return path


def _bound(record: Any, base: Path, label: str, errors: list[str]) -> dict[str, Any] | None:
    if not isinstance(record, dict):
        errors.append(f"{label}: expected {{path, sha256}}")
        return None
    path = _resolve(record.get("path"), base, label, errors)
    expected = _digest(record.get("sha256"))
    if expected is None:
        errors.append(f"{label}: invalid sha256")
    if path is None or expected is None:
        return None
    actual = sha256(path)
    if actual != expected:
        errors.append(f"{label}: sha256 mismatch")
    return {"path": str(path), "sha256": actual, "bytes": path.stat().st_size}


def _require_nonempty_string(value: Any, label: str, errors: list[str]) -> str | None:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{label}: non-empty string is required")
        return None
    return value.strip()


def transition_for_plane(plane: str) -> dict[str, Any]:
    """Return the deterministic restart/invalidation policy for one change."""
    if plane == "sealed-runtime":
        return {"restartGate": "G0", "invalidatedGates": ["G0", "G1", "G2", "G3", "G4"]}
    if plane == "host-replay":
        return {"restartGate": "G0", "invalidatedGates": ["G0", "G3", "G4"]}
    if plane == "tiger-submit":
        return {"restartGate": "G4T", "invalidatedGates": ["G4T"]}
    if plane == "model-workload":
        return {"restartGate": "G5", "invalidatedGates": ["G5", "G6", "G6C", "G7"]}
    raise CandidateClosureError(f"unknown changedPlane: {plane}")


def classify_transition(previous: dict[str, Any], current: dict[str, Any]) -> str:
    """Classify a tuple transition and reject changes outside one ownership plane."""
    changed = [key for key in TUPLE_COMPONENTS if previous.get(key) != current.get(key)]
    if len(changed) != 1:
        raise CandidateClosureError(
            "candidate transition must change exactly one ownership plane; "
            f"changed components={changed!r}"
        )
    component = changed[0]
    return {
        "sourceSeal": "sealed-runtime",
        "exactSif": "sealed-runtime",
        "hostReplay": "host-replay",
        "submitBundle": "tiger-submit",
        "workloadConfig": "model-workload",
        "modelArtifacts": "model-workload",
        "validationContract": "tiger-submit",
    }[component]


def _validate_transition(payload: dict[str, Any], errors: list[str]) -> None:
    plane = payload.get("changedPlane")
    if plane not in PLANES:
        errors.append("changedPlane must be one of " + ", ".join(sorted(PLANES)))
        return
    previous = payload.get("previousIdentities")
    current = payload.get("newIdentities")
    if not isinstance(previous, dict) or not isinstance(current, dict):
        errors.append("previousIdentities and newIdentities are required objects")
        return
    try:
        classified = classify_transition(previous, current)
    except CandidateClosureError as exc:
        errors.append(str(exc))
        return
    if classified != plane:
        errors.append(f"changedPlane {plane!r} does not match tuple transition {classified!r}")
    policy = transition_for_plane(plane)
    if payload.get("restartGate") != policy["restartGate"]:
        errors.append("restartGate does not match changedPlane policy")
    if payload.get("invalidatedGates") != policy["invalidatedGates"]:
        errors.append("invalidatedGates do not match changedPlane policy")


def validate(
    manifest_path: Path,
    *,
    expected_gate: str | None = None,
    expected_sif: Path | None = None,
    expected_sif_sha256: str | None = None,
) -> dict[str, Any]:
    """Return a machine-readable validation report; never raise for bad input."""
    errors: list[str] = []
    manifest_path = manifest_path.expanduser().resolve()
    try:
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {"schema": SCHEMA, "status": "FAIL", "errors": [f"manifest unreadable: {exc}"]}
    if not isinstance(payload, dict):
        return {"schema": SCHEMA, "status": "FAIL", "errors": ["manifest root must be an object"]}
    if payload.get("schema") != SCHEMA:
        errors.append(f"schema must equal {SCHEMA}")
    if payload.get("status") not in {"PASS", "PROMOTABLE"}:
        errors.append("status must be PASS or PROMOTABLE")
    candidate_id = _require_nonempty_string(payload.get("candidateId"), "candidateId", errors)
    selected_gate = _require_nonempty_string(payload.get("selectedGate"), "selectedGate", errors)
    if selected_gate is not None and selected_gate not in GATES:
        errors.append(f"selectedGate is not registered: {selected_gate}")
    if expected_gate is not None and selected_gate != expected_gate:
        errors.append("selectedGate does not match submission gate")
    _validate_transition(payload, errors)

    components = payload.get("components")
    if not isinstance(components, dict):
        components = {}
        errors.append("components must be an object")
    bound_components: dict[str, dict[str, Any]] = {}
    for name in TUPLE_COMPONENTS:
        record = _bound(components.get(name), manifest_path.parent, f"components.{name}", errors)
        if record is not None:
            bound_components[name] = record

    terminal = payload.get("terminalGates")
    if not isinstance(terminal, dict):
        terminal = {}
        errors.append("terminalGates must be an object")
    bound_terminal: dict[str, dict[str, Any]] = {}
    for gate in TERMINAL_COMPONENTS:
        record = _bound(terminal.get(gate), manifest_path.parent, f"terminalGates.{gate}", errors)
        if record is not None:
            bound_terminal[gate] = record

    closure = payload.get("closure")
    if not isinstance(closure, dict):
        closure = {}
        errors.append("closure must be an object")
    bound_closure: list[dict[str, Any]] = []
    seen_closure_ids: set[str] = set()
    for index, item in enumerate(closure.get("files", ()) if isinstance(closure.get("files", ()), list) else ()):
        if not isinstance(item, dict):
            errors.append(f"closure.files[{index}]: expected object")
            continue
        item_id = _require_nonempty_string(item.get("id"), f"closure.files[{index}].id", errors)
        kind = _require_nonempty_string(item.get("kind"), f"closure.files[{index}].kind", errors)
        if kind is not None and kind not in CLOSURE_KINDS:
            errors.append(f"closure.files[{index}].kind is not registered: {kind}")
        if item_id is not None and item_id in seen_closure_ids:
            errors.append(f"closure.files[{index}].id is duplicated: {item_id}")
        if item_id is not None:
            seen_closure_ids.add(item_id)
        record = _bound(item, manifest_path.parent, f"closure.files[{index}]", errors)
        if record is not None:
            record.update({"id": item_id, "kind": kind})
            bound_closure.append(record)
    if not bound_closure:
        errors.append("closure.files must contain at least one bound executable/configuration input")
    required_kinds = set(CLOSURE_KINDS)
    observed_kinds = {str(item.get("kind")) for item in bound_closure}
    missing_kinds = sorted(required_kinds - observed_kinds)
    if missing_kinds:
        errors.append("closure.files missing kinds: " + ", ".join(missing_kinds))

    tuple_value = payload.get("candidateTuple")
    if not isinstance(tuple_value, dict):
        errors.append("candidateTuple must be an object")
    else:
        for name in TUPLE_COMPONENTS:
            if _digest(tuple_value.get(name)) is None:
                errors.append(f"candidateTuple.{name} must be a sha256 digest")
            elif name in bound_components and _digest(tuple_value[name]) != bound_components[name]["sha256"]:
                errors.append(f"candidateTuple.{name} does not match components.{name}")

    if expected_sif is not None:
        expected_sif = expected_sif.expanduser().resolve()
        exact = bound_components.get("exactSif")
        if exact is None or Path(exact["path"]).resolve() != expected_sif:
            errors.append("components.exactSif does not match expected SIF path")
        if not expected_sif.is_file():
            errors.append("expected SIF is missing")
        elif exact is not None and sha256(expected_sif) != exact["sha256"]:
            errors.append("components.exactSif does not match expected SIF bytes")
    if expected_sif_sha256 is not None:
        expected = _digest(expected_sif_sha256)
        if expected is None:
            errors.append("expected SIF digest is invalid")
        elif _digest((payload.get("candidateTuple") or {}).get("exactSif")) != expected:
            errors.append("candidate exact SIF digest does not match submit digest")

    result = {
        "schema": SCHEMA,
        "status": "PASS" if not errors else "FAIL",
        "candidateId": candidate_id,
        "selectedGate": selected_gate,
        "changedPlane": payload.get("changedPlane"),
        "restartGate": payload.get("restartGate"),
        "invalidatedGates": payload.get("invalidatedGates"),
        "manifest": str(manifest_path),
        "manifestSha256": sha256(manifest_path) if manifest_path.is_file() else None,
        "components": bound_components,
        "terminalGates": bound_terminal,
        "closureFiles": bound_closure,
        "errors": errors,
    }
    return result

