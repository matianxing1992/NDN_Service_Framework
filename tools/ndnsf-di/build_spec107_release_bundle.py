#!/usr/bin/env python3
"""Build a digest-bound Spec 107 release bundle after content safety scans."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.release_gate import build_spec107_release_gate  # noqa: E402
from spec107_lineage import assert_mutation_allowed  # noqa: E402


FORBIDDEN_KEYS = frozenset({
    "payload", "payloadb64", "token", "tokencontent", "usertoken",
    "providertoken", "tensor", "tensorcontent", "kv", "kvcontent",
    "secret", "privatekey", "private_key",
})
PRIVATE_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----", b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----BEGIN EC PRIVATE KEY-----", b"UserToken=", b"ProviderToken=",
)


class ReleaseBundleError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise ReleaseBundleError(code + (f":{detail}" if detail else ""))


def _scan_json(value: object, path: str = "$") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = re.sub(r"[^a-z0-9_]", "", str(key).lower())
            if normalized in FORBIDDEN_KEYS:
                _fail("FORBIDDEN_EVIDENCE_CONTENT", f"{path}.{key}")
            _scan_json(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _scan_json(child, f"{path}[{index}]")


def _scan_file(path: Path) -> None:
    raw = path.read_bytes()
    for marker in PRIVATE_MARKERS:
        if marker.lower() in raw.lower():
            _fail("FORBIDDEN_EVIDENCE_CONTENT", path.name)
    if path.suffix.lower() == ".json":
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            _fail("EVIDENCE_JSON_INVALID", f"{path}:{exc}")
        _scan_json(value)
    elif path.suffix.lower() == ".csv" and raw:
        header = raw.splitlines()[0].decode("utf-8", "replace")
        fields = {re.sub(r"[^a-z0-9_]", "", item.strip().lower())
                  for item in header.split(",")}
        forbidden = sorted(fields & FORBIDDEN_KEYS)
        if forbidden:
            _fail("FORBIDDEN_EVIDENCE_CONTENT", f"{path}:{forbidden[0]}")


def build_release_bundle(*, feature: Path | str, output: Path | str,
                         repo_root: Path | str) -> dict[str, Any]:
    root = Path(repo_root).resolve()
    feature_root = Path(feature).resolve()
    try:
        feature_root.relative_to(root / "specs")
    except ValueError:
        _fail("SPEC107_FEATURE_PATH_INVALID", str(feature_root))
    if "spec105" in feature_root.as_posix().lower():
        _fail("SPEC105_MUTATION_DENIED")
    destination = assert_mutation_allowed(output, repo_root=root)
    try:
        destination.relative_to(feature_root)
    except ValueError:
        _fail("SPEC107_GATE_OUTPUT_INVALID", str(destination))
    if destination.exists():
        _fail("SPEC107_GATE_OUTPUT_EXISTS", str(destination))
    input_path = feature_root / "release-input.json"
    try:
        payload = json.loads(input_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        _fail("SPEC107_RELEASE_INPUT_INVALID", str(exc))
    if not isinstance(payload, dict):
        _fail("SPEC107_RELEASE_INPUT_INVALID")
    manifest = payload.get("evidenceManifest")
    if not isinstance(manifest, list):
        _fail("SPEC107_EVIDENCE_MANIFEST_INVALID")
    scanned = []
    for row in manifest:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            _fail("SPEC107_EVIDENCE_MANIFEST_INVALID")
        path = (feature_root / row["path"]).resolve()
        try:
            path.relative_to(feature_root)
        except ValueError:
            _fail("SPEC107_EVIDENCE_PATH_ESCAPE", str(row["path"]))
        if not path.is_file():
            _fail("SPEC107_EVIDENCE_MISSING", str(row["path"]))
        _scan_file(path)
        scanned.append({
            "path": row["path"], "bytes": path.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    gate = build_spec107_release_gate(payload, evidence_root=feature_root)
    gate["bundleManifest"] = {
        "schema": "ndnsf-di-spec107-release-bundle-manifest-v1",
        "releaseInputSha256": "sha256:" + hashlib.sha256(
            input_path.read_bytes()).hexdigest(),
        "artifacts": scanned,
        "contentScan": "PASS",
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        with destination.open("x", encoding="utf-8") as stream:
            json.dump(gate, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except FileExistsError:
        _fail("SPEC107_GATE_OUTPUT_EXISTS", str(destination))
    return gate


__all__ = ["ReleaseBundleError", "build_release_bundle"]
