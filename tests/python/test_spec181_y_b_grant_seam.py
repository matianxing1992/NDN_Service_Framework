"""T001 configured requester seam tests; no NFD or process boundary."""
from dataclasses import replace
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import time

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

from ndnsf_distributed_inference.core.protected_artifacts import (
    grant_from_wire, verify_and_unwrap_grant)
from ndnsf_distributed_inference.sdk.placement import ProviderGrantViewV1

ROOT = Path(__file__).resolve().parents[2]
DIGEST = "sha256:" + "1" * 64
EPOCH = "spec180-yolo-protected-v1"


@pytest.fixture
def seam(tmp_path, monkeypatch):
    example = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2"
    monkeypatch.syspath_prepend(str(example))
    spec = importlib.util.spec_from_file_location("spec181_user_fixture", example / "user.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    provider_spec = importlib.util.spec_from_file_location(
        "spec181_provider_fixture", example / "provider.py")
    provider_module = importlib.util.module_from_spec(provider_spec)
    provider_spec.loader.exec_module(provider_module)
    authority_key = ed25519.Ed25519PrivateKey.generate()
    recipient_key = ed25519.Ed25519PrivateKey.generate()
    requester_key = ed25519.Ed25519PrivateKey.generate()

    def private(name, key, raw=False):
        path = tmp_path / name
        path.write_bytes(key.private_bytes(
            serialization.Encoding.Raw if raw else serialization.Encoding.PEM,
            serialization.PrivateFormat.Raw if raw else serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        path.chmod(0o600)
        return path

    private("artifact-policy-authority.key", authority_key)
    recipient = private("recipient.key", recipient_key)
    requester = private("requester.key", requester_key, raw=True)
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
        "SPEC181_REQUESTER_PRIVATE_KEY": str(requester),
        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(mapping),
        "NDNSF_SPEC180_CONFIG_ROOT": str(tmp_path),
        "SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(public),
    }.items():
        monkeypatch.setenv(name, value)
    published = []
    result = SimpleNamespace(success=True, error="")

    def publish(name, payload, freshness_ms):
        published.append((name, payload))
        return result

    owner = SimpleNamespace(publish_signed_app_data=publish)
    client = SimpleNamespace(
        deployment=SimpleNamespace(user="/custom/requester"),
        _network_client=SimpleNamespace(service_user=owner))
    view = ProviderGrantViewV1(
        provider="/example/provider/BackboneNeck", request_id="req-yb-1", attempt=1,
        plan_core_digest="sha256:" + "c" * 64, offer_digest="sha256:" + "e" * 64,
        role_digests=("sha256:" + "a" * 64,),
        security_policy_snapshot_digest="sha256:" + "8" * 64,
        model_manifest_digest=DIGEST, protection_epoch=EPOCH)
    return SimpleNamespace(
        module=module, provider_module=provider_module,
        registry=registry, client=client, published=published, view=view,
        result=result, authority=authority_key, recipient=recipient_key,
        requester=requester_key, root=tmp_path, public=public)


def build(seam, **kwargs):
    return seam.module._build_grant_seam(
        seam.client, registry_path=seam.registry,
        model_manifest_digest=kwargs.get("manifest", DIGEST),
        model_family=kwargs.get("family", "YOLO26n"))[0]


def test_y_b_grant_seam_round_trip(seam):
    provider = build(seam)
    binding = provider(seam.view, deadline_ms=int(time.time() * 1000) + 60000)
    assert binding.grant_name.startswith("/custom/requester/NDNSF-DI/KEY-GRANT/v1/")
    name, wire = seam.published[0]
    assert name == binding.grant_name
    grant = grant_from_wire(wire)
    assert grant.policy_authority == "operator-policy-authority"
    assert grant.key_id == "operator-key-1"
    key = verify_and_unwrap_grant(
        grant, authority_public_key=seam.authority.public_key(),
        recipient_private_key=seam.recipient,
        expected_provider_identity=seam.view.provider,
        expected_request_id=seam.view.request_id, expected_attempt=1,
        expected_plan_core_digest=seam.view.plan_core_digest,
        expected_model_manifest_digest=DIGEST, expected_protection_epoch=EPOCH,
        now_ms=int(time.time() * 1000))
    assert len(key) == 32


def test_public_recipient_seam_without_provider_private_file(seam, monkeypatch):
    """The actual User grant seam must work with public-only peer material."""
    payload = seam.recipient.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    (seam.root / "recipient.pub").write_bytes(payload)
    mapping = seam.root / "public-recipients.json"
    mapping.write_text(json.dumps({seam.view.provider: {
        "path": "recipient.pub",
        "sha256": "sha256:" + hashlib.sha256(payload).hexdigest(),
    }}))
    (seam.root / "recipient.key").unlink()
    monkeypatch.delenv("SPEC181_PROVIDER_RECIPIENT_KEY_MAP")
    monkeypatch.setenv("NDNSF_DI_RECIPIENT_PUBLIC_KEY_MAP", str(mapping))
    test_y_b_grant_seam_round_trip(seam)


def test_recipient_seam_rejects_ambiguous_maps(seam, monkeypatch):
    monkeypatch.setenv("NDNSF_DI_RECIPIENT_PUBLIC_KEY_MAP", "unused-public-map.json")
    with pytest.raises(ValueError, match="only one recipient key map"):
        build(seam)
    assert seam.published == []


@pytest.mark.parametrize("changes", [
    {"model_manifest_digest": "sha256:" + "f" * 64},
    {"protection_epoch": "epoch-other"},
    {"provider": "/example/provider/unknown"},
])
def test_no_publication_outside_configured_scope(seam, changes):
    with pytest.raises(ValueError):
        build(seam)(replace(seam.view, **changes),
                    deadline_ms=int(time.time() * 1000) + 60000)
    assert seam.published == []


@pytest.mark.parametrize("kwargs", [{"family": "OtherModel"}, {"manifest": "bad"}])
def test_seam_rejects_invalid_model_policy(seam, kwargs):
    with pytest.raises(ValueError):
        build(seam, **kwargs)
    assert seam.published == []


def test_seam_rejects_unpinned_private_key(seam):
    other = ed25519.Ed25519PrivateKey.generate()
    (seam.root / "artifact-policy-authority.key").write_bytes(other.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    with pytest.raises(ValueError, match="public key"):
        build(seam)


def test_grant_policy_reads_final_published_root_digest(seam):
    trusted_binding = SimpleNamespace(model_manifest_digest="sha256:" + "f" * 64)
    provider = build(seam, manifest=lambda: trusted_binding.model_manifest_digest)
    trusted_binding.model_manifest_digest = DIGEST
    provider(seam.view, deadline_ms=int(time.time() * 1000) + 60000)
    assert grant_from_wire(seam.published[0][1]).model_manifest_digest == DIGEST
    with pytest.raises(ValueError, match="not authorized"):
        provider(replace(seam.view, model_manifest_digest="sha256:" + "f" * 64),
                 deadline_ms=int(time.time() * 1000) + 60000)
    assert len(seam.published) == 1


def test_publish_failure_returns_no_binding(seam):
    seam.result.success = False
    seam.result.error = "publication rejected"
    with pytest.raises(RuntimeError, match="publish failed"):
        build(seam)(seam.view, deadline_ms=int(time.time() * 1000) + 60000)


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
    assert build(seam) is None
    assert seam.provider_module._load_grant_keys("BackboneNeck") == (None, None, "")
