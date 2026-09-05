"""Focused Spec180 grant-seam tests: ProviderGrantViewV1 -> GrantBindingV1."""

from __future__ import annotations

from pathlib import Path
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from cryptography.hazmat.primitives.asymmetric import ed25519  # noqa: E402
from ndnsf_distributed_inference.core.protected_artifacts import (  # noqa: E402
    GrantRequestV1,
)
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    GrantBindingV1, ProviderGrantViewV1,
)
from ndnsf_distributed_inference.security import (  # noqa: E402
    ArtifactPolicyAuthority, AuthorityBackedGrantProvider, canonical_grant_name,
)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _digest(char: str) -> str:
    return "sha256:" + char * 64


class GrantProviderSeamTest(unittest.TestCase):
    def setUp(self):
        self.requester_key = ed25519.Ed25519PrivateKey.generate()
        self.recipient_key = ed25519.Ed25519PrivateKey.generate()
        self.authority = ArtifactPolicyAuthority(
            "/authority/artifact-policy",
            ed25519.Ed25519PrivateKey.generate(),
            protection_epoch="spec180-yolo-protected-v1",
            allowed_model_manifests=frozenset({_digest("c")}),
        )
        self.content_key = b"content-key-32-bytes-000000000000"
        self.seam = AuthorityBackedGrantProvider(
            requester_identity="/user/u0",
            requester_private_key=self.requester_key,
            authority_public_key=self.authority.public_key,
            authority_identity="/authority/artifact-policy",
            authority_issue=lambda request: self.authority.issue(
                request,
                requester_public_key=self.requester_key.public_key(),
                recipient_public_key=self.recipient_key.public_key(),
                content_key=self.content_key,
                key_id="key-1",
                expires_at_ms=_now_ms() + 60_000,
                now_ms=_now_ms(),
            ),
        )

    def view(self, **overrides):
        fields = dict(
            provider="/provider/p0",
            request_id="req-1",
            attempt=1,
            plan_core_digest=_digest("a"),
            offer_digest=_digest("e"),
            role_digests=(_digest("f"),),
            security_policy_snapshot_digest=_digest("b"),
            model_manifest_digest=_digest("c"),
            protection_epoch="spec180-yolo-protected-v1",
        )
        fields.update(overrides)
        return ProviderGrantViewV1(**fields)

    def test_seam_returns_a_covered_grant_binding(self):
        binding = self.seam(self.view(), deadline_ms=_now_ms() + 30_000)
        self.assertIsInstance(binding, GrantBindingV1)
        self.assertEqual(binding.provider, "/provider/p0")
        self.assertEqual(binding.request_id, "req-1")
        self.assertEqual(binding.attempt, 1)
        self.assertEqual(binding.plan_core_digest, _digest("a"))
        self.assertEqual(
            binding.protection_epoch, "spec180-yolo-protected-v1")
        self.assertTrue(binding.grant_name.startswith(
            "/authority/artifact-policy/NDNSF-DI/KEY-GRANT/v1/PROVIDER/"))
        self.assertTrue(binding.grant_name.endswith(
            "/GRANT/" + binding.grant_digest[len("sha256:"):]))

    def test_grant_name_matches_the_contract_grammar(self):
        name = canonical_grant_name(
            authority="/authority/artifact-policy",
            provider_identity="/provider/p0",
            request_id="req-1", attempt=2,
            plan_core_digest=_digest("a"),
            model_manifest_digest=_digest("c"),
            protection_epoch="spec180-yolo-protected-v1",
            grant_digest=_digest("d"),
        )
        expected_prefix = (
            "/authority/artifact-policy/NDNSF-DI/KEY-GRANT/v1"
            "/PROVIDER/")
        self.assertTrue(name.startswith(expected_prefix))
        self.assertIn("/REQ/req-1/ATTEMPT/2/PLAN-CORE/" + "a" * 64, name)
        self.assertIn("/MODEL/" + "c" * 64
                      + "/EPOCH/spec180-yolo-protected-v1", name)
        self.assertIn("/GRANT/" + "d" * 64, name)
        self.assertNotIn("sha256:", name)

    def test_plaintext_view_is_rejected_by_the_seam(self):
        with self.assertRaises(ValueError):
            self.seam(self.view(protection_epoch="plaintext-v1"),
                      deadline_ms=_now_ms() + 30_000)

    def test_view_without_model_manifest_is_rejected(self):
        with self.assertRaises(ValueError):
            self.seam(self.view(model_manifest_digest=""),
                      deadline_ms=_now_ms() + 30_000)

    def test_expired_deadline_fails_before_the_authority_call(self):
        with self.assertRaises(TimeoutError):
            self.seam(self.view(), deadline_ms=_now_ms() - 1)

    def test_unauthorized_model_manifest_fails_through_the_authority(self):
        with self.assertRaises(ValueError):
            self.seam(self.view(model_manifest_digest=_digest("d")),
                      deadline_ms=_now_ms() + 30_000)

    def test_authority_grant_mismatching_the_view_is_rejected(self):
        seam = AuthorityBackedGrantProvider(
            requester_identity="/user/u0",
            requester_private_key=self.requester_key,
            authority_public_key=self.authority.public_key,
            authority_identity="/authority/artifact-policy",
            authority_issue=lambda request: self.authority.issue(
                GrantRequestV1(
                    provider_identity="/provider/p9",  # wrong provider
                    request_id=request.request_id,
                    attempt=request.attempt,
                    plan_core_digest=request.plan_core_digest,
                    grant_view_digest=request.grant_view_digest,
                    model_manifest_digest=request.model_manifest_digest,
                    protection_epoch=request.protection_epoch,
                    requester_identity=request.requester_identity,
                    issued_at_ms=request.issued_at_ms,
                ).sign(self.requester_key),
                requester_public_key=self.requester_key.public_key(),
                recipient_public_key=self.recipient_key.public_key(),
                content_key=self.content_key,
                key_id="key-1",
                expires_at_ms=_now_ms() + 60_000,
                now_ms=_now_ms(),
            ),
        )
        with self.assertRaises(ValueError):
            seam(self.view(), deadline_ms=_now_ms() + 30_000)


if __name__ == "__main__":
    unittest.main()
