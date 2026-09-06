"""Configured grant key algorithms; these focused tests do not start NFD."""
import time
from types import SimpleNamespace

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.backends import default_backend

from test_spec181_y_b_grant_seam import seam, build, DIGEST, EPOCH
from test_spec180_yolo_minindn import load_runner
from ndnsf_distributed_inference.core.protected_artifacts import (
    grant_from_wire, verify_and_unwrap_grant)


def replace_recipient(seam, curve):
    key = ec.generate_private_key(curve, default_backend())
    (seam.root / "recipient.key").write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    return key


def test_requester_issues_p256_recipient_envelope(seam):
    recipient = replace_recipient(seam, ec.SECP256R1())
    binding = build(seam)(seam.view, deadline_ms=int(time.time() * 1000) + 60000)
    name, wire = seam.published[0]
    assert name == binding.grant_name
    grant = grant_from_wire(wire)
    key = verify_and_unwrap_grant(
        grant, authority_public_key=seam.authority.public_key(),
        recipient_private_key=recipient,
        expected_provider_identity=seam.view.provider,
        expected_request_id=seam.view.request_id, expected_attempt=1,
        expected_plan_core_digest=seam.view.plan_core_digest,
        expected_model_manifest_digest=DIGEST, expected_protection_epoch=EPOCH,
        now_ms=int(time.time() * 1000))
    assert len(key) == 32
    assert b'ECDH-P256-AESGCM-SHA256' in wire


def test_python_provider_loads_p256_recipient(seam):
    expected = replace_recipient(seam, ec.SECP256R1())
    _, actual, identity = seam.provider_module._load_grant_keys("BackboneNeck")
    assert isinstance(actual, ec.EllipticCurvePrivateKey)
    assert actual.private_numbers() == expected.private_numbers()
    assert identity == "operator-policy-authority"


@pytest.mark.parametrize("owner", ["requester", "provider"])
def test_other_recipient_curve_is_rejected_before_publication(seam, owner):
    replace_recipient(seam, ec.SECP384R1())
    with pytest.raises(ValueError):
        if owner == "requester":
            build(seam)
        else:
            seam.provider_module._load_grant_keys("BackboneNeck")
    assert seam.published == []


@pytest.mark.parametrize("invalid", ["permissions", "symlink", "oversized"])
def test_recipient_file_rejected_before_publication(seam, invalid):
    path = seam.root / "recipient.key"
    if invalid == "permissions":
        path.chmod(0o644)
    elif invalid == "symlink":
        target = path.with_suffix(".original")
        path.rename(target)
        path.symlink_to(target)
    else:
        path.write_bytes(b"x" * 65537)
    with pytest.raises((ValueError, OSError)):
        build(seam)
    assert seam.published == []


def test_runner_preserves_separate_recipient_map(monkeypatch, tmp_path):
    module = load_runner()
    monkeypatch.setattr(module, "CaseRuntimeBinding", SimpleNamespace(
        from_inputs=lambda *args: SimpleNamespace(output=tmp_path)))
    monkeypatch.setattr(module, "build_runtime_publication_file", lambda *args: tmp_path)
    monkeypatch.setattr(module, "MiniNdnCaseRuntime", lambda *args: SimpleNamespace(
        process_specs=lambda: None,
        start_network=lambda: pytest.fail("configuration test started a network"),
        stop=lambda: None))
    monkeypatch.setattr(module, "_validate_native_library_closure", lambda: None)
    monkeypatch.setattr(module.Path, "home", lambda: tmp_path)
    authority = tmp_path / ".config/ndnsf/spec180/artifact-policy-authority.key"
    authority.parent.mkdir(parents=True)
    authority.write_text("not consumed: test stops before network startup")
    monkeypatch.setenv("SPEC181_PROTECTION_EPOCH", EPOCH)
    monkeypatch.setenv("SPEC181_PROVIDER_RECIPIENT_KEY_MAP", "/configured/recipients.json")
    monkeypatch.setenv("SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP", "/configured/offers.json")
    monkeypatch.setenv("NDNSF_DI_ENVELOPE_KEY_FILE", "/configured/requester.key")
    observed = {}

    def stop_at_build_check(env):
        observed.update(env)
        raise module.RunnerError("FIXTURE_BEFORE_NETWORK")

    monkeypatch.setattr(module, "_validate_local_native_build", stop_at_build_check)
    with pytest.raises(module.RunnerError, match="FIXTURE_BEFORE_NETWORK"):
        module._run_live_case_once("Y-B", tmp_path, {})
    assert observed["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"] == "/configured/recipients.json"
    assert observed["SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP"] == "/configured/offers.json"
