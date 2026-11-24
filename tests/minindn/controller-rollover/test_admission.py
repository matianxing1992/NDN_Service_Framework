"""Offline evidence reader tests; each fixture represents one cold deployment."""

import importlib.util
import json
from pathlib import Path
import sys

import pytest

from admission import GRANT_REJECTION, configure, verify


NOW = 1000000000
REQUEST = "request-1"
ROLE = "/Stage/First/Tensor/Rank/0"
PLAN = "sha256:" + "1" * 64
DIGEST = "sha256:" + "2" * 64
SERVICE = "/example/infer"
CONTROL = "/example/usage"
USER = "/example/user"
TARGET = "/example/p0"
TARGET_INPUT = {"role": ROLE, "provider": TARGET}


def evidence(root, case):
    cold = case == "user-withdrawal"
    scope = SERVICE if cold else CONTROL
    actors = [dict(id="user", kind="requester", identity=USER),
              dict(id="ctl", kind="controller", identity="/example/ctl",
                   parameters={"revoke-identity": USER, "revoke-service": scope,
                               "revoke-attribute": "/PERMISSION" + scope})]
    actors.extend(dict(id=name, kind="provider" if name.startswith("p") else name,
                       identity="/example/" + name) for name in ("p0", "p1", "p2", "repo", "authority"))
    config = {"network": {"service": SERVICE}, "actors": actors, "requester": {"turns": [
        {"output_file": str(root / "output"), "checkpoint_output_file": str(root / "checkpoint")}]}}
    clean = [dict(id=a["id"], status="ZERO", forced=False, exitCode=int(a["id"] == "user"))
             for a in actors]
    (root / "operation.json").write_text(json.dumps({"status": "STOPPED", "failure": "REQUESTER_EXIT:1",
        "requester": {"exit_code": 1, "started_unix_seconds": NOW + 1}, "cleanup": clean}))
    (root / "ctl.log").write_text(
        f"{NOW}.500000 WARN: [ctl] NDNSF_CONTROLLER_REVOKED kind=3 identity={USER} service={scope} generation=3 epoch=1\n"
        f"NDNSF_REVOCATION_APPLIED success=1 kind=3 generation=3 epoch=1\n"
        f"NDNSF_POLICY_STATUS_PUBLISHED service={SERVICE} generation=3 epoch=1 revocations=1\n")
    terminal = {"requestId": REQUEST, "attemptEpoch": 1, "epochMs": (NOW + 3) * 1000,
                "code": "NATIVE_REQUEST_AUTHORITY_WITHDRAWN" if cold else "NATIVE_STREAM_FAILED",
                "boundary": "bootstrap" if cold else "stream",
                "message": "Controller withdrew Requester authority for " + SERVICE if cold else "stream failed"}
    data = "SPEC220_PREP_STAGE requester.user-prepare.complete\n"
    data += f"NATIVE_CONVERSATION_TURN_START index=0 request={REQUEST}\n"
    if cold:
        data += f"{NOW + 2}.000000 INFO: [user] Installed PolicyStatus service={SERVICE} generation=3 epoch=1\n"
    else:
        data += f"NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId={REQUEST} planDigest={PLAN} attemptEpoch=1\n"
    (root / "user.log").write_text(data + "NDNSF_DI_REQUEST_TERMINAL " + json.dumps(terminal) + "\n")
    provider_lines = ""
    if not cold:
        provider_lines += (f"NDNSF_DI_NATIVE_SELECTION_ACCEPTED requestId={REQUEST} attemptEpoch=1 "
                           f"role={ROLE} provider={TARGET} planDigest={PLAN}\n")
        rejection = dict(status="REJECTED", boundary="BEFORE_ASSEMBLY", reason=GRANT_REJECTION,
                         provider=TARGET, requestId=REQUEST, planDigest=PLAN, attemptId="attempt-1",
                         grantDigest=DIGEST, planCoreDigest=DIGEST)
        provider_lines += "NDNSF_DI_GRANT_VERIFICATION " + json.dumps(rejection) + "\n"
    for i in range(3):
        (root / f"p{i}.log").write_text(provider_lines if i == 0 else "")
    return config


@pytest.mark.parametrize("case", ["user-withdrawal", "invalid-grant"])
def test_matching_admission_receipt(tmp_path, case):
    config = evidence(tmp_path, case)
    actual = verify(config, tmp_path, case, TARGET_INPUT, NOW * 10**9)
    assert actual["requestTerminal"]["requestId"] == REQUEST and actual["actorExitCodes"]["user"] == 1
    assert ("grantVerification" in actual) == (case == "invalid-grant")


@pytest.mark.parametrize("case,filename,before,after", [
    ("invalid-grant", "p0.log", GRANT_REJECTION, "invalid Authority signature"),
    ("invalid-grant", "p0.log", "attempt-1", "attempt-2"),
    ("invalid-grant", "p0.log", "NDNSF_DI_NATIVE_SELECTION_ACCEPTED", "debug"),
    ("invalid-grant", "user.log", "NDNSF_DI_NATIVE_SELECTION_COMMITTED", "debug"),
    ("invalid-grant", "user.log", "NATIVE_STREAM_FAILED", "NATIVE_REQUEST_AUTHORITY_WITHDRAWN"),
    ("user-withdrawal", "ctl.log", f"identity={USER}", f"identity={TARGET}"),
    ("user-withdrawal", "user.log", "epoch=1", "epoch=2"),
    ("user-withdrawal", "user.log", "NATIVE_REQUEST_AUTHORITY_WITHDRAWN", "NATIVE_REQUEST_BOOTSTRAP_TIMEOUT"),
    ("user-withdrawal", "operation.json", str(NOW + 1), str(NOW - 1)),
])
def test_reject_wrong_boundary(tmp_path, case, filename, before, after):
    config = evidence(tmp_path, case)
    path = tmp_path / filename
    text = path.read_text()
    assert before in text
    path.write_text(text.replace(before, after))
    with pytest.raises(RuntimeError):
        verify(config, tmp_path, case, TARGET_INPUT, NOW * 10**9)


@pytest.mark.parametrize("case,actor", [("user-withdrawal", "p2"), ("invalid-grant", "p0")])
def test_assembly_on_affected_provider_fails(tmp_path, case, actor):
    config = evidence(tmp_path, case)
    with (tmp_path / (actor + ".log")).open("a") as stream:
        stream.write("NDNSF_DI_PROVIDER_STAGE stage=ASSEMBLY_STARTED status=observed\n")
    with pytest.raises(RuntimeError):
        verify(config, tmp_path, case, TARGET_INPUT, NOW * 10**9)


def test_only_issuer_recipient_binding_changes(tmp_path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

    config = evidence(tmp_path, "invalid-grant")
    config["requester"]["fixed_placement"] = {"role_providers": {ROLE: TARGET}}
    original = tmp_path / "original-public.pem"
    private = tmp_path / "recipient.pem"
    recipient = Ed25519PrivateKey.generate()
    original.write_bytes(recipient.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    private.write_bytes(recipient.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    key_map = tmp_path / "recipients.json"
    key_map.write_text(json.dumps({TARGET: str(private)}))
    next(a for a in config["actors"] if a["identity"] == TARGET)["parameters"] = {
        "recipient-key-map": str(key_map)}
    mapping = {TARGET: str(original), "/example/p1": "other.pem"}
    config["artifact_authority"] = {"authority": {"recipient_public_key_files": mapping}}
    baseline_actors = json.dumps(config["actors"])
    result = configure(tmp_path, config, "invalid-grant")
    assert result["originalPublicKey"] == str(original) and result["submittedPublicKey"] == mapping[TARGET]
    assert isinstance(serialization.load_pem_public_key((tmp_path / "invalid-recipient-public.pem").read_bytes()),
                      Ed25519PublicKey)
    assert mapping["/example/p1"] == "other.pem" and json.dumps(config["actors"]) == baseline_actors
    with pytest.raises(RuntimeError, match="unmodified Authority mapping"):
        configure(tmp_path, config, "invalid-grant")
    mapping[TARGET] = str(original)
    with pytest.raises(FileExistsError):
        configure(tmp_path, config, "invalid-grant")


def test_in_flight_mode_dispatches_after_reading_preparation_receipt(tmp_path, monkeypatch):
    tmp_path.chmod(0o700)
    script = Path(__file__).with_name("three-rank-continuity.py")
    spec = importlib.util.spec_from_file_location("rollover_main", script)
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    handle = tmp_path / "handle"
    source = tmp_path / "model"
    source.touch()
    manifest = tmp_path / "manifest"
    manifest.write_text("{}")
    repo = tmp_path / "repo"
    checkpoint = tmp_path / "controller.checkpoint"
    config = {"requester": {"model_input": {"private_handle_file": str(handle)}, "request": {},
                             "fixed_placement": {"role_providers": {ROLE: TARGET}}},
              "storage": {"repo_root": str(repo)}, "security": {"controller_recovery_checkpoint": str(checkpoint)},
              "release": {"manifest": str(manifest), "prefix": "/test"}}
    target = {"revoke-trigger-file": str(tmp_path / "trigger")}
    monkeypatch.setattr(driver, "documents", lambda root: ({}, config, source, target))

    def prepare(*args):
        handle.write_bytes(b"NDNSF-DI-STORED-MODEL-HANDLE-v2\n")
        checkpoint.touch()
        store = repo / "repo.sqlite.store" / "payloads" / "sha256"
        store.mkdir(parents=True)
        (store / "packet").write_bytes(b"wire")
        (tmp_path / "requester.log").write_text(json.dumps({
            "type": "ndnsf-di-stored-model-preparation", "status": "PUBLISHED",
            "model": {"rootWireDigest": DIGEST}}))
        return tmp_path

    monkeypatch.setattr(driver, "phase", prepare)
    monkeypatch.setattr(driver.withdrawal, "run", lambda root, stored, role, launcher: {"targetRole": role})
    monkeypatch.setattr(driver.os, "umask", lambda mask: None)
    monkeypatch.setattr(sys, "argv", [str(script), str(tmp_path), "--case", "withdrawal"])
    driver.main()
    assert json.loads((tmp_path / "withdrawal-result.json").read_text())["targetRole"] == ROLE
