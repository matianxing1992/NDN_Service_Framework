"""Fault-injected real helper processes, owned by the production C++ assembler."""

from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

from test_spec181_assembly_parity import CASES, ROOT
from ndnsf_distributed_inference.core.protected_artifacts import GrantRequestV1, grant_to_wire
from ndnsf_distributed_inference.security.artifact_policy_authority import ArtifactPolicyAuthority


def protected_case(case, ttl_ms):
    now = int(time.time() * 1000)
    authority, requester, recipient = [ed25519.Ed25519PrivateKey.generate() for _ in range(3)]
    epoch = "spec181-lifecycle-v1"
    model = case["role"]["model_manifest_digest"]
    core = "sha256:" + "d" * 64
    request = GrantRequestV1(provider_identity="/spec181/provider",
        request_id="/spec181/assembly/request", attempt=1, plan_core_digest=core,
        grant_view_digest="sha256:" + "e" * 64, model_manifest_digest=model,
        protection_epoch=epoch, requester_identity="/spec181/requester", issued_at_ms=now).sign(requester)
    grant = ArtifactPolicyAuthority("/spec181/authority", authority,
        protection_epoch=epoch, allowed_model_manifests=frozenset({model})).issue(
        request, requester_public_key=requester.public_key(), recipient_public_key=recipient.public_key(),
        content_key=bytes(range(32)), key_id="fixture", expires_at_ms=now + ttl_ms, now_ms=now)
    case["role"]["protection_epoch"] = epoch
    case["grant"] = dict(planCoreDigest=core, grantDigest=grant.grant_digest,
        wire=grant_to_wire(grant).decode(),
        authorityPublicKeyHex=authority.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex(),
        recipientSeedHex=recipient.private_bytes(serialization.Encoding.Raw,
            serialization.PrivateFormat.Raw, serialization.NoEncryption()).hex())


def invoke(case, tmp_path, body):
    binary = os.environ.get("SPEC181_ASSEMBLY_PARITY_BINARY", "")
    assert binary and Path(binary).is_file()
    interpreter = tmp_path / "helper"
    pid_file = tmp_path / "helper.pid"
    interpreter.write_text("#!" + sys.executable + "\nimport os,sys,time,json,hashlib\n"
        + "from pathlib import Path\n"
        + f"Path({str(pid_file)!r}).write_text(str(os.getpid()))\n" + body)
    interpreter.chmod(0o700)
    request = tmp_path / "case.json"
    request.write_text(json.dumps(case))
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = os.pathsep.join([
        str(Path(binary).resolve().parent), env.get("LD_LIBRARY_PATH", "")])
    env["PYTHONPATH"] = os.pathsep.join([str(ROOT / "NDNSF-DistributedInference"),
        str(ROOT / "pythonWrapper"), str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
        env.get("PYTHONPATH", "")])
    started = time.monotonic()
    result = subprocess.run([binary, str(request), str(tmp_path / "cache"), str(interpreter)],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=8)
    elapsed = time.monotonic() - started
    records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    assert records, result.stderr
    if pid_file.exists():
        # No helper PID, including an unreaped zombie, may survive return.
        assert not Path("/proc", pid_file.read_text()).exists()
    return result.returncode, records[-1], elapsed, pid_file


PAUSE_THEN_ASSEMBLE = "time.sleep(1.5)\nos.execv(sys.executable,[sys.executable,*sys.argv[1:]])\n"


@pytest.mark.parametrize("mode", ["helper-timeout", "request-deadline", "cancel", "external-cancel", "grant-expiry"])
def test_live_helper_stops_and_cleans_before_slow_work_finishes(tmp_path, mode):
    case = deepcopy(CASES[0])
    case["controls"] = {}
    if mode == "helper-timeout": case["controls"]["helperTimeoutMs"] = 150
    if mode == "request-deadline": case["controls"]["deadlineMs"] = int(time.time() * 1000) + 200
    if mode == "cancel":
        case["controls"]["cancelAfterMs"] = 150
        protected_case(case, 5000)
    if mode == "grant-expiry": protected_case(case, 450)
    if mode == "external-cancel":
        case["controls"]["runtimeCancelAfterMs"] = 150
        protected_case(case, 5000)
    code, result, elapsed, pid = invoke(case, tmp_path, PAUSE_THEN_ASSEMBLE)
    assert pid.exists(), "the test must reach an actual running helper"
    assert code == 2, result
    assert elapsed < 1.2, (result, elapsed)
    assert "expired" in result["reason"] or "TIMEOUT" in result["reason"] or "CANCELLED" in result["reason"]
    if mode in {"cancel", "external-cancel", "grant-expiry"}: assert result["runtimeState"] == "5"
    assert not list((tmp_path / "cache").rglob("model.onnx"))
    assert not list((tmp_path / "cache" / ".staging").glob("assembly-*"))


def test_expired_request_never_starts_helper(tmp_path):
    case = deepcopy(CASES[0]); case["controls"] = {"deadlineMs": 1}
    code, result, elapsed, pid = invoke(case, tmp_path, PAUSE_THEN_ASSEMBLE)
    assert code == 2, result
    assert not pid.exists()
    assert elapsed < 1


def test_cancel_during_source_fetch_cannot_recreate_plaintext_directory(tmp_path):
    case = deepcopy(CASES[0]); case["controls"] = {"cancelOnSourceFetch": True}
    protected_case(case, 5000)
    code, result, _, pid = invoke(case, tmp_path, PAUSE_THEN_ASSEMBLE)
    assert code == 2, result
    assert not pid.exists()
    assert result["runtimeState"] == "5"
    assert not list((tmp_path / "cache").rglob("canonical.onnx"))
    assert not list((tmp_path / "cache" / ".staging").glob("assembly-*"))


def test_helper_cannot_activate_model_larger_than_role_envelope(tmp_path):
    case = deepcopy(CASES[0])
    body = """
out=Path(sys.argv[sys.argv.index('--output-dir')+1])
model=out/'model.onnx'; manifest=out/'manifest.json'
model.write_bytes(b'x'*131072); manifest.write_bytes(b'{}')
sha=lambda p:'sha256:'+hashlib.sha256(p.read_bytes()).hexdigest()
print(json.dumps(dict(schema='ndnsf-di-native-assembly-result-v1',model_path=str(model),
    manifest_path=str(manifest),model_digest=sha(model),manifest_digest=sha(manifest))))
"""
    code, result, _, pid = invoke(case, tmp_path, body)
    assert pid.exists()
    assert code == 2, result
    assert not list((tmp_path / "cache").rglob("model.onnx"))
    assert not list((tmp_path / "cache").rglob("manifest.signature"))
