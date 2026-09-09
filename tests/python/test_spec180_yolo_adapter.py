from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest

from spec180_yolo_inputs import declared_yolo_checkpoint


ROOT = Path(__file__).resolve().parents[2]
EXPORTER = ROOT / "tools/ndnsf-di/export_spec180_yolo26_onnx.py"


def _load_exporter():
    spec = importlib.util.spec_from_file_location("spec180_yolo_export_adapter", EXPORTER)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _make_package(tmp_path: Path):
    exporter = _load_exporter()
    package = tmp_path / "package"
    exporter.build_package(declared_yolo_checkpoint(), package, input_size=32)
    return package


def test_yolo_adapter_enumerates_only_registered_candidates(tmp_path: Path):
    package = _make_package(tmp_path)
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
    from ndnsf_distributed_inference.splitter import canonical_contract_digest

    adapter = build_yolo26n_adapter(package, require_signature=False)
    model = adapter.describe_model(
        "YOLO26n", "sha256:" + "1" * 64,
        canonical_contract_digest({"fixture": "spec180"}),
    )
    candidates = adapter.splitter.enumerate_candidates(model, adapter.graph.snapshot)
    assert [item.execution_plan.roles for item in candidates] == [
        ("FullModel",),
        ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge"),
    ]
    assert candidates[1].execution_plan.roles[-1] == "Merge"
    assert candidates[1].cross_partition_tensors
    assert candidates[0].input_ingress_role == "FullModel"
    assert candidates[0].result_egress_role == "FullModel"
    assert candidates[0].merge_kind == "ONNX_POSTPROCESS"
    assert candidates[0].postprocessing["outputName"] == "predictions"
    assert candidates[1].input_ingress_role == "BackboneNeck"
    assert candidates[1].result_egress_role == "Merge"

    # Ownership is part of the runtime candidate identity, not catalogue-only
    # metadata.  Changing either endpoint must change the candidate digest.
    from dataclasses import replace
    changed = replace(candidates[1], result_egress_role="DetectShard1")
    assert changed.candidate_digest != candidates[1].candidate_digest


def test_tampered_registered_candidate_digest_fails_before_enumeration(tmp_path: Path):
    package = _make_package(tmp_path)
    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["catalogue"]["candidates"][0]["candidateDigest"] = "sha256:" + "0" * 64
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
    with pytest.raises(ValueError, match="candidate digest"):
        build_yolo26n_adapter(package, require_signature=False)


def test_signed_catalogue_requires_registered_verification(tmp_path: Path):
    exporter = _load_exporter()
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    key = Ed25519PrivateKey.generate()
    private_path = tmp_path / "authority.pem"
    private_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    public_path = tmp_path / "authority.pub"
    public_path.write_bytes(public)
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({
        "status": "CONFIGURED",
        "catalogue": {
            "authorityId": "authority", "keyId": "key",
            "publicKeyAlgorithm": "ed25519", "signatureAlgorithm": "ed25519",
            "publicKeyPath": "authority.pub",
            "publicKeySha256": "sha256:" + hashlib.sha256(public).hexdigest(),
        },
    }), encoding="utf-8")
    package = tmp_path / "signed"
    exporter.build_package(declared_yolo_checkpoint(), package, input_size=32,
                           signing_key=private_path, registry_path=registry)
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
    adapter = build_yolo26n_adapter(package, registry_path=registry)
    assert adapter.graph.snapshot.nodes

    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["catalogue"]["candidates"][0]["selectionPriority"] = 99
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError):
        build_yolo26n_adapter(package, registry_path=registry)


def test_catalogue_digest_mismatch_reports_trust_root_error(tmp_path: Path):
    """A malformed registry must not be masked by an unbound exception."""
    from ndnsf_distributed_inference.adapters.yolo.candidates import (
        verify_catalogue_signature,
    )

    public_path = tmp_path / "authority.pub"
    public_path.write_bytes(b"not-a-key")
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({
        "status": "CONFIGURED",
        "catalogue": {
            "authorityId": "authority", "keyId": "key",
            "publicKeyPath": public_path.name,
            "publicKeySha256": "sha256:" + "0" * 64,
        },
    }), encoding="utf-8")
    catalogue = {
        "schema": "spec180-yolo-catalogue-v1",
        "signature": {
            "authorityId": "authority", "keyId": "key",
            "algorithm": "ed25519", "valueB64": "",
        },
    }
    with pytest.raises(ValueError, match="trust-root public key digest mismatch"):
        verify_catalogue_signature(catalogue, registry)


@pytest.mark.parametrize("mutation,match", [
    ("graphRevision", "graph revision"),
    ("weights", "initializer"),
])
def test_package_integrity_failures_stop_before_candidate_use(
        tmp_path: Path, mutation: str, match: str):
    package = _make_package(tmp_path)
    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if mutation == "graphRevision":
        manifest[mutation] = "unknown-revision"
    else:
        (package / "canonical" / "yolo26n.weights").unlink()
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
    with pytest.raises(ValueError, match=match):
        build_yolo26n_adapter(package, require_signature=False)


def test_semantic_partition_is_runtime_bound_not_a_catalogue_hint(tmp_path: Path):
    package = _make_package(tmp_path)
    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    shared = next(item for item in manifest["catalogue"]["candidates"]
                  if item["candidateId"] == "shared-backbone-two-shard-v1")
    partition = shared["semanticPartition"]
    assert all("afterNode" not in item for item in partition["safeCuts"])
    assert "0.55" not in json.dumps(partition, sort_keys=True)
    moved = partition["roleNodeSets"]["BackboneNeck"].pop()
    partition["roleNodeSets"]["DetectShard0"].append(moved)
    partition["nodeBranch"][moved] = "detect-scale-0"
    canonical = json.dumps({key: value for key, value in shared.items()
                            if key != "candidateDigest"},
                           sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False).encode("utf-8")
    shared["candidateDigest"] = "sha256:" + hashlib.sha256(canonical).hexdigest()
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
    adapter = build_yolo26n_adapter(package, require_signature=False)
    model = adapter.describe_model(
        "YOLO26n", "sha256:" + "1" * 64,
        "sha256:" + "2" * 64,
    )
    with pytest.raises(ValueError, match="semantic"):
        adapter.splitter.enumerate_candidates(model, adapter.graph.snapshot)
