from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/spec180_contract_gate.py"
FEATURE = ROOT / "specs/180-ack-driven-cross-model-qualification"


def load_gate():
    spec = importlib.util.spec_from_file_location("spec180_contract_gate", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    # Python 3.8's dataclasses resolve postponed annotations through
    # sys.modules while the module is being executed.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_repository_documents_have_complete_registry_and_no_qualification_shortcut():
    gate = load_gate()
    result = gate.run_gate(ROOT, FEATURE)
    assert result["status"] == "PASS"
    assert result["contractReady"] is True
    assert result["qualificationReady"] is False
    assert result["readinessScope"] == "DOCUMENT_AND_TRUST_ROOT_CONTRACT"
    assert result["trustRootStatus"] == "CONFIGURED"
    assert result["summary"]["traceabilityComplete"] is True
    assert len(result["summary"]["successCriteria"]) == 9
    codes = {item["code"] for item in result["issues"]}
    assert codes == set()
    assert "DUPLICATE_TASK_ID" not in codes
    assert "TASK_REGISTRY_INCOMPLETE" not in codes


def _configured_fixture(tmp_path: Path):
    gate = load_gate()
    feature = tmp_path / "feature"
    (feature / "contracts").mkdir(parents=True)
    private = __import__(
        "cryptography.hazmat.primitives.asymmetric.ed25519",
        fromlist=["Ed25519PrivateKey"],
    ).Ed25519PrivateKey.generate()
    from cryptography.hazmat.primitives import serialization
    public = private.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_path = feature / "contracts" / "catalogue-test.pub"
    key_path.write_bytes(public)
    entry = {
        "authorityId": "test-authority",
        "keyId": "test-key",
        "publicKeyAlgorithm": "ed25519",
        "signatureAlgorithm": "ed25519",
        "publicKeyPath": "contracts/catalogue-test.pub",
        "publicKeySha256": "sha256:" + hashlib.sha256(public).hexdigest(),
        "manifestSchema": "spec180-yolo-catalogue-v1",
        "acceptedModelFamilies": ["YOLO26n"],
    }
    return gate, feature, private, entry


def _signed_manifest(gate, private, *, key_id="test-key", algorithm="ed25519"):
    body = {"schemaVersion": 1, "modelFamily": "YOLO26n", "graphDigest": "sha256:" + "a" * 64}
    signature = private.sign(gate.canonical_bytes(body))
    body["signature"] = {
        "keyId": key_id,
        "algorithm": algorithm,
        "valueB64": base64.b64encode(signature).decode("ascii"),
    }
    return body


def test_signed_manifest_uses_registered_key_and_canonical_view(tmp_path: Path):
    gate, feature, private, entry = _configured_fixture(tmp_path)
    ok, issues = gate.verify_signed_manifest(
        _signed_manifest(gate, private), entry, feature_dir=feature)
    assert ok is True
    assert issues == []


def test_unsigned_manifest_fails_before_enumeration(tmp_path: Path):
    gate, feature, _private, entry = _configured_fixture(tmp_path)
    ok, issues = gate.verify_signed_manifest(
        {"schemaVersion": 1, "modelFamily": "YOLO26n"},
        entry, feature_dir=feature)
    assert ok is False
    assert {item.code for item in issues} == {"MANIFEST_UNSIGNED"}


def test_unknown_key_and_algorithm_fail_closed(tmp_path: Path):
    gate, feature, private, entry = _configured_fixture(tmp_path)
    manifest = _signed_manifest(gate, private, key_id="wrong-key", algorithm="rsa-sha256")
    ok, issues = gate.verify_signed_manifest(manifest, entry, feature_dir=feature)
    assert ok is False
    assert {item.code for item in issues} == {
        "MANIFEST_UNKNOWN_KEY", "MANIFEST_SIGNATURE_ALGORITHM",
    }


def test_tampering_after_signature_is_rejected(tmp_path: Path):
    gate, feature, private, entry = _configured_fixture(tmp_path)
    manifest = _signed_manifest(gate, private)
    manifest["graphDigest"] = "sha256:" + "b" * 64
    ok, issues = gate.verify_signed_manifest(manifest, entry, feature_dir=feature)
    assert ok is False
    assert {item.code for item in issues} == {"MANIFEST_SIGNATURE_INVALID"}


def test_registry_path_escape_is_rejected(tmp_path: Path):
    gate = load_gate()
    feature = tmp_path / "feature"
    (feature / "contracts").mkdir(parents=True)
    entry = {
        "authorityId": "a", "keyId": "k", "publicKeyAlgorithm": "ed25519",
        "signatureAlgorithm": "ed25519", "publicKeyPath": "../../outside.pub",
        "publicKeySha256": "sha256:" + "0" * 64,
        "manifestSchema": "schema", "acceptedModelFamilies": ["YOLO26n"],
    }
    issues = gate._validate_trust_entry(entry, "catalogue", feature)
    assert any(item.code == "TRUST_ROOT_PATH_ESCAPE" for item in issues)


def test_unconfigured_registry_is_a_hard_gate(tmp_path: Path):
    gate, feature, _private, entry = _configured_fixture(tmp_path)
    registry = {
        "schemaVersion": 1,
        "status": "UNCONFIGURED",
        "catalogue": entry,
        "modelManifest": dict(entry, keyId="model-key"),
    }
    registry_path = feature / "contracts" / "trust-root-registry-v1.json"
    registry_path.write_text(json.dumps(registry), encoding="utf-8")
    _loaded, issues = gate.load_trust_registry(feature)
    assert any(item.code == "TRUST_ROOT_UNCONFIGURED" for item in issues)
