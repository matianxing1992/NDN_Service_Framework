"""Focused Spec180 FR-008 protected-grant positive and negative tests.

Covers the canonical KeyGrantV1 round trip: requester-signed GrantRequestV1,
policy-checked authority issuance with a recipient-encrypted content-key
envelope, non-circular grant digest, Provider-side verify/unwrap, expiry,
revocation, and cross-binding failures.
"""

from __future__ import annotations

from pathlib import Path
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from cryptography.hazmat.primitives.asymmetric import ed25519  # noqa: E402
from ndnsf_distributed_inference.core import (  # noqa: E402
    GrantRequestV1, KeyGrantV1, RecipientEnvelopeV1, RevocationStateV1,
    verify_and_unwrap_grant, wrap_content_key, unwrap_content_key,
)
from ndnsf_distributed_inference.security import ArtifactPolicyAuthority  # noqa: E402


def _now_ms() -> int:
    return int(time.time() * 1000)


def _digest(char: str) -> str:
    return "sha256:" + char * 64


class ProtectedGrantRoundTripTest(unittest.TestCase):
    def setUp(self):
        self.requester_key = ed25519.Ed25519PrivateKey.generate()
        self.recipient_key = ed25519.Ed25519PrivateKey.generate()
        self.authority_key = ed25519.Ed25519PrivateKey.generate()
        self.authority = ArtifactPolicyAuthority(
            "/authority/artifact-policy", self.authority_key,
            protection_epoch="epoch-1",
            allowed_model_manifests=frozenset({_digest("c")}),
        )
        self.content_key = b"content-key-32-bytes-000000000000"

    def signed_request(self, **overrides):
        fields = dict(
            provider_identity="/provider/p0",
            request_id="req-1",
            attempt=1,
            plan_core_digest=_digest("a"),
            grant_view_digest=_digest("b"),
            model_manifest_digest=_digest("c"),
            protection_epoch="epoch-1",
            requester_identity="/user/u0",
            issued_at_ms=_now_ms(),
        )
        fields.update(overrides)
        return GrantRequestV1(**fields).sign(self.requester_key)

    def issue(self, request):
        return self.authority.issue(
            request,
            requester_public_key=self.requester_key.public_key(),
            recipient_public_key=self.recipient_key.public_key(),
            content_key=self.content_key,
            key_id="key-1",
            expires_at_ms=_now_ms() + 60_000,
            now_ms=_now_ms(),
        )

    def unwrap(self, grant):
        return verify_and_unwrap_grant(
            grant,
            authority_public_key=self.authority.public_key,
            recipient_private_key=self.recipient_key,
            expected_provider_identity="/provider/p0",
            expected_request_id="req-1",
            expected_attempt=1,
            expected_plan_core_digest=_digest("a"),
            expected_model_manifest_digest=_digest("c"),
            expected_protection_epoch="epoch-1",
            now_ms=_now_ms(),
        )

    def test_round_trip_returns_the_original_content_key(self):
        grant = self.issue(self.signed_request())
        self.assertEqual(grant.grant_digest, grant.computed_grant_digest())
        grant.verify(self.authority.public_key, now_ms=_now_ms())
        self.assertEqual(self.unwrap(grant), self.content_key)

    def test_grant_digest_is_non_circular_and_covers_every_field(self):
        grant = self.issue(self.signed_request())
        self.assertNotIn(grant.grant_digest, grant.signing_bytes().decode())
        tampered = KeyGrantV1(
            policy_authority=grant.policy_authority,
            provider_identity=grant.provider_identity,
            request_id="req-2",  # any bound-field change
            attempt=grant.attempt,
            plan_core_digest=grant.plan_core_digest,
            model_manifest_digest=grant.model_manifest_digest,
            protection_epoch=grant.protection_epoch,
            key_id=grant.key_id,
            wrapped_content_key=grant.wrapped_content_key,
            allowed_residency_tiers=grant.allowed_residency_tiers,
            issued_at_ms=grant.issued_at_ms,
            expires_at_ms=grant.expires_at_ms,
            revocation_sequence=grant.revocation_sequence,
            grant_digest=grant.grant_digest,
            authority_signature=grant.authority_signature,
        )
        with self.assertRaises(ValueError):
            tampered.verify(self.authority.public_key, now_ms=_now_ms())

    def test_wrong_requester_signature_is_rejected(self):
        request = self.signed_request()
        forged = GrantRequestV1(
            provider_identity=request.provider_identity,
            request_id=request.request_id,
            attempt=request.attempt,
            plan_core_digest=request.plan_core_digest,
            grant_view_digest=request.grant_view_digest,
            model_manifest_digest=request.model_manifest_digest,
            protection_epoch=request.protection_epoch,
            requester_identity=request.requester_identity,
            issued_at_ms=request.issued_at_ms,
            requester_signature=request.requester_signature,
        )
        other = ed25519.Ed25519PrivateKey.generate()
        with self.assertRaises(ValueError):
            self.authority.issue(
                forged,
                requester_public_key=other.public_key(),
                recipient_public_key=self.recipient_key.public_key(),
                content_key=self.content_key, key_id="key-1",
                expires_at_ms=_now_ms() + 60_000, now_ms=_now_ms())

    def test_unauthorized_model_manifest_is_rejected(self):
        request = self.signed_request(model_manifest_digest=_digest("d"))
        with self.assertRaises(ValueError):
            self.issue(request)

    def test_wrong_protection_epoch_is_rejected(self):
        request = self.signed_request(protection_epoch="epoch-2")
        with self.assertRaises(ValueError):
            self.issue(request)

    def test_expired_grant_fails_verification(self):
        grant = self.issue(self.signed_request())
        with self.assertRaises(ValueError):
            grant.verify(self.authority.public_key,
                         now_ms=grant.expires_at_ms + 1)

    def test_wrong_recipient_key_cannot_unwrap(self):
        grant = self.issue(self.signed_request())
        wrong_key = ed25519.Ed25519PrivateKey.generate()
        with self.assertRaises(ValueError):
            verify_and_unwrap_grant(
                grant,
                authority_public_key=self.authority.public_key,
                recipient_private_key=wrong_key,
                expected_provider_identity="/provider/p0",
                expected_request_id="req-1",
                expected_attempt=1,
                expected_plan_core_digest=_digest("a"),
                expected_model_manifest_digest=_digest("c"),
                expected_protection_epoch="epoch-1",
                now_ms=_now_ms())

    def test_cross_request_binding_fails_closed(self):
        grant = self.issue(self.signed_request())
        with self.assertRaises(ValueError):
            verify_and_unwrap_grant(
                grant,
                authority_public_key=self.authority.public_key,
                recipient_private_key=self.recipient_key,
                expected_provider_identity="/provider/p0",
                expected_request_id="req-OTHER",
                expected_attempt=1,
                expected_plan_core_digest=_digest("a"),
                expected_model_manifest_digest=_digest("c"),
                expected_protection_epoch="epoch-1",
                now_ms=_now_ms())

    def test_revoked_grant_is_rejected_and_stale_state_fails_closed(self):
        grant = self.issue(self.signed_request())
        state = self.authority.revoke(
            frozenset({grant.grant_digest}), now_ms=_now_ms(),
            next_check_at_ms=_now_ms() + 60_000)
        self.assertTrue(state.is_revoked(grant.grant_digest, now_ms=_now_ms()))
        with self.assertRaises(ValueError):
            verify_and_unwrap_grant(
                grant,
                authority_public_key=self.authority.public_key,
                recipient_private_key=self.recipient_key,
                expected_provider_identity="/provider/p0",
                expected_request_id="req-1",
                expected_attempt=1,
                expected_plan_core_digest=_digest("a"),
                expected_model_manifest_digest=_digest("c"),
                expected_protection_epoch="epoch-1",
                revocation_state=state,
                now_ms=_now_ms())
        with self.assertRaises(ValueError):
            state.is_revoked(grant.grant_digest,
                             now_ms=state.next_check_at_ms + 1)

    def test_envelope_aad_binds_request_context(self):
        envelope = wrap_content_key(
            self.recipient_key.public_key(),
            provider_identity="/provider/p0", request_id="req-1", attempt=1,
            plan_core_digest=_digest("a"), model_manifest_digest=_digest("c"),
            protection_epoch="epoch-1", content_key=self.content_key)
        with self.assertRaises(ValueError):
            unwrap_content_key(
                self.recipient_key, envelope,
                provider_identity="/provider/p0", request_id="req-1",
                attempt=2, plan_core_digest=_digest("a"),
                model_manifest_digest=_digest("c"),
                protection_epoch="epoch-1")

    def test_envelope_serialization_round_trip(self):
        envelope = wrap_content_key(
            self.recipient_key.public_key(),
            provider_identity="/provider/p0", request_id="req-1", attempt=1,
            plan_core_digest=_digest("a"), model_manifest_digest=_digest("c"),
            protection_epoch="epoch-1", content_key=self.content_key)
        parsed = RecipientEnvelopeV1.from_dict(envelope.to_dict())
        self.assertEqual(
            unwrap_content_key(
                self.recipient_key, parsed,
                provider_identity="/provider/p0", request_id="req-1",
                attempt=1, plan_core_digest=_digest("a"),
                model_manifest_digest=_digest("c"),
                protection_epoch="epoch-1"),
            self.content_key)

    def test_requester_cannot_be_the_selected_provider(self):
        request = self.signed_request(requester_identity="/provider/p0")
        with self.assertRaises(ValueError):
            self.issue(request)


if __name__ == "__main__":
    unittest.main()
