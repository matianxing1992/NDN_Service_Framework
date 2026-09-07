"""Spec183 experiment authority: fixed key set, signing, gate interoperability.

The signed wire format must be verifiable by the shared Spec180 contract gate
(scripts/spec180_contract_gate.py), the verifier Spec183 T004/T005/T012/T017
reuse. Tests here sign with the issued keys and verify through that gate.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # TigerCluster
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))  # repo root
from tools import spec183_authority as authority

REPO = Path(__file__).resolve().parents[3]


@pytest.fixture()
def tmp_root(tmp_path):
    authority.issue(tmp_path)
    return tmp_path


def sample_manifest():
    return {"schema": "spec180-yolo-model-manifest-v1", "modelFamily": "YOLO26n",
            "modelVersion": "v1-test", "candidateDigest": "sha256:" + "a" * 64}


def _gate():
    from scripts import spec180_contract_gate as gate
    return gate


def _feature(tmp_root):
    return tmp_root / authority.CONTRACTS_REL.parent


def _registry(tmp_root):
    return json.loads(
        (tmp_root / authority.CONTRACTS_REL / authority.REGISTRY_FILENAME).read_text())


def _sign_and_verify(tmp_root, manifest=None, authority_name="modelManifest"):
    manifest = dict(manifest or sample_manifest())
    key = tmp_root / authority.KEY_DIR_REL / (
        authority.KEY_SLUGS[authority_name] + "-authority.key")
    registry = _registry(tmp_root)
    signed = authority.sign_manifest(manifest, key_path=key,
                                     key_id=registry[authority_name]["keyId"])
    ok, problems = _gate().verify_signed_manifest(
        signed, registry[authority_name], feature_dir=_feature(tmp_root))
    return ok, problems, signed


def _codes(problems):
    return [p.code for p in problems]


def test_issued_keys_verify_through_shared_gate(tmp_root):
    ok, problems, _ = _sign_and_verify(tmp_root)
    assert ok, problems


def test_catalogue_key_is_a_separate_authority(tmp_root):
    ok, problems, _ = _sign_and_verify(tmp_root, authority_name="catalogue")
    assert ok, problems


def test_gate_rejects_tampered_signed_manifest(tmp_root):
    ok, problems, signed = _sign_and_verify(tmp_root)
    assert ok
    signed["candidateDigest"] = "sha256:" + "b" * 64
    ok, problems = _gate().verify_signed_manifest(
        signed, _registry(tmp_root)["modelManifest"], feature_dir=_feature(tmp_root))
    assert not ok
    assert "MANIFEST_SIGNATURE_INVALID" in _codes(problems)


def test_gate_rejects_unsigned_manifest(tmp_root):
    ok, problems = _gate().verify_signed_manifest(
        sample_manifest(), _registry(tmp_root)["modelManifest"],
        feature_dir=_feature(tmp_root))
    assert not ok and _codes(problems) == ["MANIFEST_UNSIGNED"]


def test_gate_rejects_foreign_key_id(tmp_root):
    registry = _registry(tmp_root)
    key = tmp_root / authority.KEY_DIR_REL / "model-manifest-authority.key"
    signed = authority.sign_manifest(sample_manifest(), key_path=key,
                                     key_id="not-the-registered-key")
    ok, problems = _gate().verify_signed_manifest(
        signed, registry["modelManifest"], feature_dir=_feature(tmp_root))
    assert not ok and "MANIFEST_UNKNOWN_KEY" in _codes(problems)


def test_spec183_verifier_accepts_and_rejects(tmp_root):
    _, _, signed = _sign_and_verify(tmp_root)
    ok, problems = authority.verify_signed_manifest(
        signed, root=tmp_root, authority="modelManifest")
    assert ok, problems
    bad = dict(signed)
    bad["modelVersion"] = "tampered"
    ok, problems = authority.verify_signed_manifest(
        bad, root=tmp_root, authority="modelManifest")
    assert not ok and problems[0] == "MANIFEST_SIGNATURE_INVALID"


def test_issue_is_idempotent_and_fixed(tmp_path):
    authority.issue(tmp_path)
    first = {p.name: p.read_bytes() for p in
             (tmp_path / authority.KEY_DIR_REL).rglob("*.key")}
    pub_first = {p.name: p.read_bytes() for p in
                 (tmp_path / authority.CONTRACTS_REL).rglob("*.pub")}
    authority.issue(tmp_path)
    second = {p.name: p.read_bytes() for p in
              (tmp_path / authority.KEY_DIR_REL).rglob("*.key")}
    pub_second = {p.name: p.read_bytes() for p in
                  (tmp_path / authority.CONTRACTS_REL).rglob("*.pub")}
    assert first == second and pub_first == pub_second
    assert sorted(first) == ["BackboneNeck.key", "DetectShard0.key",
                             "DetectShard1.key", "Merge.key",
                             "artifact-policy-authority.key",
                             "catalogue-authority.key",
                             "model-manifest-authority.key"]


def test_private_key_modes_are_0600_and_directory_0700(tmp_path):
    authority.issue(tmp_path)
    key_dir = tmp_path / authority.KEY_DIR_REL
    assert key_dir.stat().st_mode & 0o777 == 0o700
    for key in key_dir.rglob("*.key"):
        assert key.stat().st_mode & 0o777 == 0o600, key


def test_issue_refuses_to_mix_foreign_registry_identity(tmp_path):
    authority.issue(tmp_path)
    registry = tmp_path / authority.CONTRACTS_REL / authority.REGISTRY_FILENAME
    doc = json.loads(registry.read_text())
    doc["modelManifest"]["keyId"] = "some-other-key"
    registry.write_text(json.dumps(doc))
    with pytest.raises(authority.AuthorityError, match="different modelManifest key"):
        authority.issue(tmp_path)


def test_sign_cli_writes_verifiable_manifest(tmp_root, tmp_path):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps(sample_manifest()))
    out = tmp_path / "signed.json"
    key = tmp_root / authority.KEY_DIR_REL / "model-manifest-authority.key"
    registry = json.loads((tmp_root / authority.CONTRACTS_REL /
                           authority.REGISTRY_FILENAME).read_text())
    code = authority.main(["sign", "--root", str(tmp_root), "--key", str(key),
                           "--key-id", registry["modelManifest"]["keyId"],
                           "--manifest", str(manifest), "--output", str(out)])
    assert code == 0
    signed = json.loads(out.read_text())
    ok, problems = _gate().verify_signed_manifest(
        signed, registry["modelManifest"], feature_dir=_feature(tmp_root))
    assert ok, problems


@pytest.mark.skipif(not (REPO / authority.KEY_DIR_REL / "model-manifest-authority.key").exists(),
                    reason="real key set not issued on this machine")
def test_committed_registry_and_real_keys_are_consistent():
    registry = json.loads((REPO / authority.CONTRACTS_REL /
                           authority.REGISTRY_FILENAME).read_text())
    assert registry.get("status") == "CONFIGURED"
    for name, entry in registry.items():
        if name in ("schemaVersion", "status"):
            continue
        pub = (REPO / authority.CONTRACTS_REL.parent /
               Path(entry["publicKeyPath"])).read_bytes()
        assert "sha256:" + hashlib.sha256(pub).hexdigest() == entry["publicKeySha256"]


@pytest.mark.skipif(not (REPO / authority.KEY_DIR_REL / "model-manifest-authority.key").exists(),
                    reason="real key set not issued on this machine")
def test_real_key_signs_manifest_the_shared_gate_accepts(tmp_path):
    registry = json.loads((REPO / authority.CONTRACTS_REL /
                           authority.REGISTRY_FILENAME).read_text())
    key = REPO / authority.KEY_DIR_REL / "model-manifest-authority.key"
    signed = authority.sign_manifest(sample_manifest(), key_path=key,
                                     key_id=registry["modelManifest"]["keyId"])
    ok, problems = _gate().verify_signed_manifest(
        signed, registry["modelManifest"], feature_dir=REPO / "specs/183-tiger-yolo-reusable-experiments")
    assert ok, problems


def test_gitignore_covers_private_key_dir():
    ignore = (REPO / "Experiments/TigerCluster/.gitignore").read_text()
    assert "/.keys/" in ignore
