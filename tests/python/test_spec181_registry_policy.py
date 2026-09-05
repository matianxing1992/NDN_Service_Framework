"""T001 registry policy failures before grant publication (unit fixtures)."""
import hashlib
import json
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

from ndnsf_distributed_inference.security import registry_keys


@pytest.fixture
def registry(tmp_path):
    contracts = tmp_path / "contracts"
    contracts.mkdir()
    key = ed25519.Ed25519PrivateKey.generate()
    public = key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    (contracts / "authority.pub").write_bytes(public)
    private_path = tmp_path / "artifact-policy-authority.key"
    private_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    private_path.chmod(0o600)
    document = {
        "schemaVersion": 1, "status": "CONFIGURED",
        "artifactPolicyAuthority": {
            "authorityId": "fixture-policy-authority", "keyId": "fixture-key",
            "publicKeyPath": "contracts/authority.pub",
            "publicKeySha256": "sha256:" + hashlib.sha256(public).hexdigest(),
            "publicKeyAlgorithm": "ed25519", "signatureAlgorithm": "ed25519",
            "grantSchema": "ndnsf-di-key-grant-v1",
            "acceptedModelFamilies": ["YOLO26n"], "protectionEpochs": ["epoch-1"],
        },
    }
    path = contracts / "trust-root-registry-v1.json"
    path.write_text(json.dumps(document))
    return path, document, key, tmp_path


def load(registry, **kwargs):
    return registry_keys.load_artifact_policy_authority_registry(
        registry[0], model_family=kwargs.get("model_family", "YOLO26n"),
        protection_epoch=kwargs.get("protection_epoch", "epoch-1"))


def test_registry_public_key_and_private_key_match(registry):
    policy = load(registry)
    assert policy.authority_id == "fixture-policy-authority"
    assert policy.key_id == "fixture-key"
    key = registry_keys.load_artifact_policy_authority_private_key(
        registry[3], expected_public_key=policy.public_key)
    assert key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw) == policy.public_key.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


@pytest.mark.parametrize("field,value", [
    ("publicKeySha256", "sha256:" + "0" * 64),
    ("publicKeyAlgorithm", "rsa"), ("signatureAlgorithm", "rsa"),
    ("grantSchema", "unknown"), ("authorityId", ""), ("keyId", ""),
    ("publicKeyPath", "../authority.pub"),
])
def test_registry_rejects_invalid_policy(registry, field, value):
    path, document, _, _ = registry
    document["artifactPolicyAuthority"][field] = value
    path.write_text(json.dumps(document))
    with pytest.raises(ValueError):
        load(registry)


@pytest.mark.parametrize("kwargs", [
    {"model_family": "OtherModel"}, {"protection_epoch": "epoch-2"},
])
def test_registry_rejects_outside_scope(registry, kwargs):
    with pytest.raises(ValueError):
        load(registry, **kwargs)


def test_private_key_must_match_pinned_public_key(registry):
    other = ed25519.Ed25519PrivateKey.generate().public_key()
    with pytest.raises(ValueError, match="public key"):
        registry_keys.load_artifact_policy_authority_private_key(
            registry[3], expected_public_key=other)


def test_registry_pipeline_helper_uses_pinned_identity(registry):
    from ndnsf_distributed_inference.security.requester_grant_pipeline import load_authority_from_registry
    identity, _ = load_authority_from_registry(
        registry_path=str(registry[0]), config_root=str(registry[3]),
        model_family="YOLO26n", protection_epoch="epoch-1")
    assert identity == "fixture-policy-authority"


def test_private_key_symlink_rejected(registry):
    root = registry[3]
    private = root / "artifact-policy-authority.key"
    destination = root / "other.key"
    private.rename(destination)
    private.symlink_to(destination)
    with pytest.raises(ValueError, match="regular"):
        registry_keys.load_artifact_policy_authority_private_key(root)
