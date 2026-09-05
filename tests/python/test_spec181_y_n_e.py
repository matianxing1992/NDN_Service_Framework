"""spec181 T006: Y-N-E runs REAL grant mutations through the verifier.

The Y-N-E subcase constructs three real KeyGrantV1 mutations (expired,
wrong recipient, forged authority signature) and asserts that the
implemented verifier (Python and native, parity-locked) rejects each at
the authorization boundary.  The R001 UNAVAILABLE regime is absorbed: no
synthetic epoch exception may stand in for a rejection, and the runner
records a registered PASS (DI_PROTECTED_GRANT_REJECTED) only after every
mutation is verifier-rejected.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import time
from types import SimpleNamespace

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519
import pytest

from ndnsf_distributed_inference.core.protected_artifacts import (
    GrantRequestV1, grant_from_wire, grant_to_wire,
    verify_and_unwrap_grant)
from ndnsf_distributed_inference.security.artifact_policy_authority import (
    ArtifactPolicyAuthority)
from ndnsf_distributed_inference.security.grant_mutations import (
    mutate_expired, mutate_forged_authority, verify_mutation_rejected)

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("spec181_yolo_y_n_e", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _marker_spec_and_log(module, tmp_path):
    """A User spec whose command carries --request-id, plus a journal that
    reaches ARTIFACTS_READY (the Y-N-E authorization boundary)."""
    journal = module.LifecycleJournal(tmp_path, "Y-N-E",
                                      require_protocol_binding=False)
    journal.bind_protocol_identity(request_id="/request",
                                   attempt_id="attempt-1")
    for milestone in module.MILESTONES:
        journal.append(milestone, **({"planDigest": "sha256:" + "a" * 64}
                                     if milestone == "PLAN_SEALED" else {}))
        if milestone == "ARTIFACTS_READY":
            break
    user_log = tmp_path / "user.log"
    user_log.write_text("")
    user_spec = SimpleNamespace(
        name="user", node="node",
        command="python user.py --request-id /request",
        log=str(user_log), kind="user")
    return user_spec, user_log


# ---------------------------------------------------------------------------
# Mutation construction fixtures (unit layer)


def _seed(label: str) -> bytes:
    return hashlib.sha256(
        ("spec181-y-n-e-test:" + label).encode("utf-8")).digest()


class MutationConstructionTest:
    @pytest.fixture(autouse=True)
    def _fixture(self):
        self.authority_key = ed25519.Ed25519PrivateKey.from_private_bytes(
            _seed("authority"))
        self.recipient_key = ed25519.Ed25519PrivateKey.from_private_bytes(
            _seed("recipient"))
        self.wrong_recipient_key = ed25519.Ed25519PrivateKey.from_private_bytes(
            _seed("wrong-recipient"))
        self.requester_key = ed25519.Ed25519PrivateKey.from_private_bytes(
            _seed("requester"))
        self.evil_key = ed25519.Ed25519PrivateKey.from_private_bytes(
            _seed("evil"))
        self.content_key = _seed("content-key")[:32]
        self.now_ms = int(time.time() * 1000)
        self.authority = ArtifactPolicyAuthority(
            "/authority/artifact-policy", self.authority_key,
            protection_epoch="spec180-yolo-protected-v1",
            allowed_model_manifests=frozenset({"sha256:" + "11" * 32}),
        )

    def _issue(self, recipient):
        request = GrantRequestV1(
            provider_identity="/provider/p0", request_id="req-y-n-e-1",
            attempt=1, plan_core_digest="sha256:" + "cd" * 32,
            grant_view_digest="sha256:" + "55" * 32,
            model_manifest_digest="sha256:" + "11" * 32,
            protection_epoch="spec180-yolo-protected-v1",
            requester_identity="/user/u0", issued_at_ms=self.now_ms,
        ).sign(self.requester_key)
        return self.authority.issue(
            request,
            requester_public_key=self.requester_key.public_key(),
            recipient_public_key=recipient.public_key(),
            content_key=self.content_key, key_id="key-y-n-e-1",
            expires_at_ms=self.now_ms + 60_000, now_ms=self.now_ms)

    def _verify_kwargs(self):
        return dict(
            authority_public_key=self.authority_key.public_key(),
            recipient_private_key=self.recipient_key,
            expected_provider_identity="/provider/p0",
            expected_request_id="req-y-n-e-1", expected_attempt=1,
            expected_plan_core_digest="sha256:" + "cd" * 32,
            expected_model_manifest_digest="sha256:" + "11" * 32,
            expected_protection_epoch="spec180-yolo-protected-v1",
            now_ms=self.now_ms + 1000)

    def test_expired_mutation_is_verifier_rejected(self):
        mutation = mutate_expired(
            self._issue(self.recipient_key), self.authority_key,
            expires_at_ms=self.now_ms - 1)
        reason = verify_mutation_rejected(
            verify_and_unwrap_grant, mutation, **self._verify_kwargs())
        assert "expired" in reason

    def test_wrong_recipient_mutation_is_verifier_rejected(self):
        mutation = self._issue(self.wrong_recipient_key)
        reason = verify_mutation_rejected(
            verify_and_unwrap_grant, mutation, **self._verify_kwargs())
        assert "authentication" in reason or "envelope" in reason

    def test_forged_authority_mutation_is_verifier_rejected(self):
        mutation = mutate_forged_authority(
            self._issue(self.recipient_key), self.evil_key)
        reason = verify_mutation_rejected(
            verify_and_unwrap_grant, mutation, **self._verify_kwargs())
        assert "signature" in reason

    def test_native_verifier_agrees_on_all_three_mutations(self):
        from ndnsf import _ndnsf
        mutations = {
            "EXPIRED": mutate_expired(
                self._issue(self.recipient_key), self.authority_key,
                expires_at_ms=self.now_ms - 1),
            "WRONG_RECIPIENT": self._issue(self.wrong_recipient_key),
            "FORGED_AUTHORITY": mutate_forged_authority(
                self._issue(self.recipient_key), self.evil_key),
        }
        authority_raw = self.authority_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
        recipient_seed = self.recipient_key.private_bytes(
            serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
            serialization.NoEncryption()).hex()
        for name, mutation in mutations.items():
            result = _ndnsf.verify_and_unwrap_native_grant(
                grant_to_wire(mutation).decode("utf-8"),
                authority_raw, recipient_seed,
                "/provider/p0", "req-y-n-e-1", 1,
                "sha256:" + "cd" * 32, "sha256:" + "11" * 32,
                "spec180-yolo-protected-v1", self.now_ms + 1000)
            assert not bool(result["verified"]), name
            assert "DI_PROTECTED_" in str(result["reason"]), name

    def test_positive_grant_still_unwraps(self):
        grant = self._issue(self.recipient_key)
        key = verify_and_unwrap_grant(grant, **self._verify_kwargs())
        assert key == self.content_key

    def test_mutation_accepted_by_verifier_fails_closed(self):
        grant = self._issue(self.recipient_key)
        with pytest.raises(AssertionError):
            verify_mutation_rejected(
                verify_and_unwrap_grant, grant, **self._verify_kwargs())


# ---------------------------------------------------------------------------
# Runner semantics (the absorbed R001 regime)


def test_runner_registers_di_protected_grant_rejected_reason():
    module = load_runner()
    assert module.YN_NEGATIVE_REASONS["Y-N-E"] == "DI_PROTECTED_GRANT_REJECTED"
    assert not hasattr(module, "YN_E_UNAVAILABLE_REASON")


def test_runner_accepts_y_n_e_pass_marker_with_mutation_reason(tmp_path):
    module = load_runner()
    marker = ("SPEC180_YN_NEGATIVE_RESULT status=PASS subcase=Y-N-E"
              " boundary=ARTIFACTS_READY"
              " reason=DI_PROTECTED_GRANT_REJECTED"
              " requestId=/request attemptId=attempt-1"
              " observedPhase=ARTIFACTS_READY")
    user_spec, user_log = _marker_spec_and_log(module, tmp_path)
    module._validate_negative_marker(
        marker, SimpleNamespace(name="user"),
        "Y-N-E", user_spec, user_log)


def test_runner_rejects_y_n_e_synthetic_reason(tmp_path):
    module = load_runner()
    marker = ("SPEC180_YN_NEGATIVE_RESULT status=PASS subcase=Y-N-E"
              " boundary=ARTIFACTS_READY"
              " reason=PROTECTION_EPOCH_REJECTED"
              " requestId=/request attemptId=attempt-1"
              " observedPhase=ARTIFACTS_READY")
    user_spec, user_log = _marker_spec_and_log(module, tmp_path)
    with pytest.raises(module.RunnerError):
        module._validate_negative_marker(
            marker, SimpleNamespace(name="user"),
            "Y-N-E", user_spec, user_log)


def test_runner_rejects_y_n_e_unavailable_marker(tmp_path):
    module = load_runner()
    marker = ("SPEC180_YN_NEGATIVE_RESULT status=UNAVAILABLE subcase=Y-N-E"
              " boundary=ARTIFACTS_READY"
              " reason=Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED"
              " requestId=/request attemptId=attempt-1"
              " observedPhase=ARTIFACTS_READY")
    user_spec, user_log = _marker_spec_and_log(module, tmp_path)
    with pytest.raises(module.RunnerError):
        module._validate_negative_marker(
            marker, SimpleNamespace(name="user"),
            "Y-N-E", user_spec, user_log)


def test_focused_y_n_e_probe_rejects_all_mutations():
    """The focused probe itself must complete without RunnerError: every
    mutation reaches the verifier and is rejected at the boundary."""
    module = load_runner()
    module._run_y_n_e_mutations()


def test_focused_y_n_e_probe_returns_registered_pass(tmp_path):
    module = load_runner()
    result = module._run_focused_y_n_negative("Y-N-E", tmp_path, {})
    assert result["status"] == "PASS"
    assert result["outcome"] == "FAIL_CLOSED"
    assert result["reason"] == "DI_PROTECTED_GRANT_REJECTED"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__]))
