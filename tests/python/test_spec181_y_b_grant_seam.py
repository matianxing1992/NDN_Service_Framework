"""Provider grant key/registry checks; no NFD or process boundary."""
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519


ROOT = Path(__file__).resolve().parents[2]
EPOCH = "spec180-yolo-protected-v1"


@pytest.fixture
def seam(tmp_path, monkeypatch):
    example = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2"
    monkeypatch.syspath_prepend(str(example))
    provider_spec = importlib.util.spec_from_file_location(
        "spec181_provider_fixture", example / "provider.py")
    provider_module = importlib.util.module_from_spec(provider_spec)
    provider_spec.loader.exec_module(provider_module)
    authority_key = ed25519.Ed25519PrivateKey.generate()
    recipient_key = ed25519.Ed25519PrivateKey.generate()

    def private(name, key):
        path = tmp_path / name
        path.write_bytes(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        path.chmod(0o600)
        return path

    recipient = private("recipient.key", recipient_key)
    mapping = tmp_path / "map.json"
    mapping.write_text(json.dumps({"/example/provider/BackboneNeck": str(recipient)}))
    contracts = tmp_path / "contracts"
    contracts.mkdir()
    public = contracts / "authority.pub"
    payload = authority_key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    public.write_bytes(payload)
    registry = contracts / "trust-root-registry-v1.json"
    registry.write_text(json.dumps({
        "schemaVersion": 1, "status": "CONFIGURED",
        "artifactPolicyAuthority": {
            "authorityId": "operator-policy-authority", "keyId": "operator-key-1",
            "grantSchema": "ndnsf-di-key-grant-v1", "protectionEpochs": [EPOCH],
            "acceptedModelFamilies": ["YOLO26n"],
            "publicKeyAlgorithm": "ed25519", "signatureAlgorithm": "ed25519",
            "publicKeyPath": "contracts/authority.pub",
            "publicKeySha256": "sha256:" + hashlib.sha256(payload).hexdigest(),
        }}))
    for name, value in {
        "SPEC181_PROTECTION_EPOCH": EPOCH,
        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(mapping),
        "NDNSF_SPEC180_CONFIG_ROOT": str(tmp_path),
        "SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(public),
    }.items():
        monkeypatch.setenv(name, value)
    return SimpleNamespace(provider_module=provider_module, registry=registry,
                           root=tmp_path, public=public)


def test_provider_loads_pinned_authority_identity(seam):
    key, recipient, identity = seam.provider_module._load_grant_keys("BackboneNeck")
    assert identity == "operator-policy-authority"
    assert key is not None and recipient is not None


def test_provider_rejects_changed_registry_public_key(seam):
    seam.public.write_bytes(b"replaced public key")
    with pytest.raises(ValueError, match="digest"):
        seam.provider_module._load_grant_keys("BackboneNeck")


def test_provider_key_lookup_uses_configured_prefix(seam):
    mapping = seam.root / "map.json"
    entries = json.loads(mapping.read_text())
    entries["/custom/provider/BackboneNeck"] = entries.pop("/example/provider/BackboneNeck")
    mapping.write_text(json.dumps(entries))
    assert seam.provider_module._load_grant_keys(
        "BackboneNeck", provider_prefix="/custom/provider")[2] == "operator-policy-authority"
    with pytest.raises(ValueError, match="no grant recipient"):
        seam.provider_module._load_grant_keys("BackboneNeck")


def test_plaintext_never_loads_registry(seam, monkeypatch):
    monkeypatch.delenv("SPEC181_PROTECTION_EPOCH")
    seam.registry.unlink()
    assert seam.provider_module._load_grant_keys("BackboneNeck") == (None, None, "")
