from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest

from spec180_yolo_inputs import declared_yolo_checkpoint


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/ndnsf-di/export_spec180_yolo26_onnx.py"


def load_exporter():
    spec = importlib.util.spec_from_file_location("spec180_yolo_export", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_checkpoint_digest_is_pinned_and_wrong_source_fails(tmp_path: Path):
    exporter = load_exporter()
    assert exporter.sha256_file(declared_yolo_checkpoint()) == exporter.EXPECTED_CHECKPOINT_SHA256
    wrong = tmp_path / "wrong.pt"
    wrong.write_bytes(b"not-the-registered-checkpoint")
    with pytest.raises(exporter.ExportError, match="does not match"):
        exporter.build_package(wrong, tmp_path / "out", input_size=32)


@pytest.mark.parametrize("kind,reason", [
    ("missing", "SPEC180_YOLO_CHECKPOINT_REQUIRED"),
    ("empty", "SPEC180_YOLO_CHECKPOINT_REQUIRED"),
    ("relative", "SPEC180_YOLO_CHECKPOINT_NOT_ABSOLUTE"),
    ("absent-file", "SPEC180_YOLO_CHECKPOINT_NOT_FILE"),
    ("directory", "SPEC180_YOLO_CHECKPOINT_NOT_FILE"),
])
def test_checkpoint_input_requires_explicit_existing_file(monkeypatch, tmp_path, kind, reason):
    monkeypatch.delenv("SPEC180_YOLO_CHECKPOINT", raising=False)
    values = {"empty": "", "relative": "yolo26n.pt",
              "absent-file": str(tmp_path / "absent.pt"), "directory": str(tmp_path)}
    if kind in values:
        monkeypatch.setenv("SPEC180_YOLO_CHECKPOINT", values[kind])
    with pytest.raises(ValueError, match=reason):
        declared_yolo_checkpoint()


def test_catalogue_is_provider_independent_and_ordered():
    exporter = load_exporter()
    semantic_partition = {
        "schema": "spec180-yolo-semantic-partition-v1",
        "roleNodeSets": {
            "BackboneNeck": ["node-a"], "DetectShard0": ["node-b"],
            "DetectShard1": ["node-c"], "Merge": ["node-d"],
        },
        "branchOwnership": {"backbone-neck": "BackboneNeck",
                             "detect-scale-0": "DetectShard0",
                             "detect-scale-1": "DetectShard1",
                             "postprocess-merge": "Merge"},
        "nodeBranch": {"node-a": "backbone-neck", "node-b": "detect-scale-0",
                        "node-c": "detect-scale-1", "node-d": "postprocess-merge"},
        "roleInterfaces": {
            role: {"inputs": [{"name": f"{role}-in", "dtype": "float32", "shape": []}],
                   "outputs": [{"name": f"{role}-out", "dtype": "float32", "shape": []}]}
            for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
        },
        "tensorInterfaces": [{
            "edgeId": "node-1", "producerNode": "node-a",
            "producerRole": "BackboneNeck", "consumerNodes": ["node-b"],
            "consumerRoles": ["DetectShard0"], "dtype": "float32", "shape": [],
        }],
        "dependencyEdges": [{"fromRole": "BackboneNeck", "toRole": "DetectShard0",
                              "tensorEdges": ["node-1"]}],
        "safeCuts": [{"cutId": "BackboneNeck-to-DetectShard0",
                       "fromRole": "BackboneNeck", "toRole": "DetectShard0",
                       "boundaryTensors": ["node-1"]}],
        "equivalence": {"oraclePath": "oracle/full-model-output.npy",
                         "method": "pytorch-reference-compared-with-cpu-onnxruntime",
                         "atol": 1e-3, "rtol": 1e-4},
    }
    catalogue = exporter.candidate_catalogue(
        graph_digest="sha256:" + "a" * 64,
        model_digest="sha256:" + "b" * 64,
        safe_cuts=semantic_partition["safeCuts"],
        semantic_partition=semantic_partition,
    )
    assert [item["candidateId"] for item in catalogue["candidates"]] == [
        "atomic-v1", "shared-backbone-two-shard-v1"]
    assert all(item["candidateDigest"].startswith("sha256:")
               for item in catalogue["candidates"])
    encoded = json.dumps(catalogue, sort_keys=True)
    assert "providerIdentity" not in encoded
    assert "providerName" not in encoded


def test_signing_requires_registered_public_key(tmp_path: Path):
    exporter = load_exporter()
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    key = Ed25519PrivateKey.generate()
    key_path = tmp_path / "catalogue-key.pem"
    key_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({
        "status": "CONFIGURED",
        "catalogue": {
            "authorityId": "authority",
            "keyId": "registered-key",
            "publicKeyAlgorithm": "ed25519",
            "signatureAlgorithm": "ed25519",
            "publicKeySha256": "sha256:" + "0" * 64,
        },
    }), encoding="utf-8")
    with pytest.raises(exporter.ExportError, match="public key path is missing"):
        exporter._sign_catalogue(
            {"schema": exporter.CATALOGUE_SCHEMA},
            key_path=key_path, registry_path=registry)
    assert hashlib.sha256(public).hexdigest() != "0" * 64


def test_matching_ephemeral_authority_can_sign_catalogue(tmp_path: Path):
    exporter = load_exporter()
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    key = Ed25519PrivateKey.generate()
    key_path = tmp_path / "catalogue-key.pem"
    key_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    public_path = tmp_path / "catalogue.pub"
    public_path.write_bytes(public)
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({
        "status": "CONFIGURED",
        "catalogue": {
            "authorityId": "authority",
            "keyId": "registered-key",
            "publicKeyAlgorithm": "ed25519",
            "signatureAlgorithm": "ed25519",
            "publicKeyPath": "catalogue.pub",
            "publicKeySha256": "sha256:" + hashlib.sha256(public).hexdigest(),
            "manifestSchema": exporter.CATALOGUE_SCHEMA,
            "acceptedModelFamilies": ["YOLO26n"],
        },
    }), encoding="utf-8")
    signed = exporter._sign_catalogue(
        {"schema": exporter.CATALOGUE_SCHEMA, "modelFamily": "YOLO26n"},
        key_path=key_path, registry_path=registry)
    assert signed["signature"]["keyId"] == "registered-key"
    assert signed["signature"]["algorithm"] == "ed25519"


def test_matching_pem_authority_can_sign_catalogue(tmp_path: Path):
    """The registry digest covers the PEM file bytes, not raw-key bytes."""
    exporter = load_exporter()
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    key = Ed25519PrivateKey.generate()
    key_path = tmp_path / "catalogue-key.pem"
    key_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    public_path = tmp_path / "catalogue-authority.pub"
    public_path.write_bytes(key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    ))
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({
        "status": "CONFIGURED",
        "catalogue": {
            "authorityId": "authority",
            "keyId": "registered-key",
            "publicKeyAlgorithm": "ed25519",
            "signatureAlgorithm": "ed25519",
            "publicKeyPath": public_path.name,
            "publicKeySha256": "sha256:" + hashlib.sha256(
                public_path.read_bytes()).hexdigest(),
        },
    }), encoding="utf-8")
    signed = exporter._sign_catalogue(
        {"schema": exporter.CATALOGUE_SCHEMA, "modelFamily": "YOLO26n"},
        key_path=key_path, registry_path=registry)
    assert signed["signature"]["keyId"] == "registered-key"


def test_real_export_produces_external_initializer_manifest(tmp_path: Path):
    exporter = load_exporter()
    manifest = exporter.build_package(declared_yolo_checkpoint(), tmp_path / "package", input_size=32)
    assert manifest["schema"] == exporter.MANIFEST_SCHEMA
    assert manifest["providerIndependent"] is True
    graph = tmp_path / "package" / "canonical" / "yolo26n.onnx"
    weights = tmp_path / "package" / "canonical" / "yolo26n.weights"
    assert graph.is_file() and graph.stat().st_size > 0
    assert weights.is_file() and weights.stat().st_size > 0
    assert manifest["weights"]["digest"] == "sha256:" + exporter.sha256_file(weights)
    assert len(manifest["catalogue"]["candidates"]) == 2
    assert manifest["fixture"]["path"] == (
        "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm")
    assert manifest["oracle"]["outputShape"][0] == 1
    assert manifest["oracle"]["outputShape"][2] == 6
    assert 0 < manifest["oracle"]["outputShape"][1] <= 300
    assert manifest["catalogue"]["candidates"][1]["semanticPartition"]["equivalence"]["method"] == (
        "pytorch-reference-compared-with-cpu-onnxruntime")


def test_640_oracle_matches_cpu_onnx_runtime_after_canonical_row_sort(
        tmp_path: Path):
    import onnxruntime as ort
    import numpy as np

    exporter = load_exporter()
    package = tmp_path / "package"
    manifest = exporter.build_package(declared_yolo_checkpoint(), package)
    fixture = ROOT / "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm"
    inputs = exporter._fixture_tensor(fixture, 640).numpy().astype(np.float32)
    session = ort.InferenceSession(
        str(package / "canonical/yolo26n.onnx"),
        providers=["CPUExecutionProvider"],
    )
    actual = session.run(["predictions"], {"images": inputs})[0]
    expected = np.load(package / "oracle/full-model-output.npy",
                       allow_pickle=False)
    actual = exporter._canonicalize_detection_rows(actual)
    assert list(expected.shape) == manifest["oracle"]["outputShape"]
    assert list(expected.shape)[0] == 1
    assert list(expected.shape)[2] == 6
    assert np.allclose(actual, expected, atol=1e-3, rtol=1e-4)
