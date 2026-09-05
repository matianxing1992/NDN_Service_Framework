"""Operator-configured ArtifactPolicyAuthority for protected artifact grants.

Issues Spec170 ``artifact-assembly-v1`` KeyGrantV1 records: it verifies the
requester signature and requester identity, enforces the configured model/
epoch/residency policy, wraps the content key to the selected Provider
identity certificate, signs the grant with the authority Ed25519 key, and
maintains a signed revocation ledger.

The authority identity, endpoint, and trust rule are operator configuration
(Spec180: an entry in ``contracts/trust-root-registry-v1.json``); the private
key lives outside Git like every other Spec180 signing key.
"""

from __future__ import annotations

from typing import Mapping

from cryptography.hazmat.primitives.asymmetric import ed25519

from ..core.protected_artifacts import (
    GrantRequestV1,
    KeyGrantV1,
    RecipientEnvelopeV1,
    RecipientPublicKey,
    RevocationStateV1,
    wrap_content_key,
)


class ArtifactPolicyAuthority:
    """Policy-checking, recipient-encrypting, signing grant authority."""

    def __init__(
        self,
        identity: str,
        private_key: ed25519.Ed25519PrivateKey,
        *,
        protection_epoch: str,
        allowed_model_manifests: frozenset[str] = frozenset(),
        allowed_residency_tiers: tuple[str, ...] = ("DISK_CIPHERTEXT_ASSEMBLED",),
        revocation_sequence: int = 1,
    ) -> None:
        if (not identity or not protection_epoch or not allowed_residency_tiers
                or revocation_sequence <= 0):
            raise ValueError("artifact policy authority is incomplete")
        self.identity = identity
        self.private_key = private_key
        self.public_key = private_key.public_key()
        self.protection_epoch = protection_epoch
        self.allowed_model_manifests = frozenset(allowed_model_manifests)
        self.allowed_residency_tiers = frozenset(allowed_residency_tiers)
        self.revocation_sequence = revocation_sequence
        self._revoked: set[str] = set()

    def issue(
        self,
        request: GrantRequestV1,
        *,
        requester_public_key: ed25519.Ed25519PublicKey,
        recipient_public_key: RecipientPublicKey,
        content_key: bytes,
        key_id: str,
        expires_at_ms: int,
        now_ms: int,
    ) -> KeyGrantV1:
        """Verify the request, wrap the content key, and sign the grant."""
        request.verify(requester_public_key)
        if int(now_ms) >= expires_at_ms:
            raise ValueError("grant expiry must be in the future")
        if request.provider_identity == request.requester_identity:
            raise ValueError("grant requester cannot be the selected Provider")
        if request.protection_epoch != self.protection_epoch:
            raise ValueError(
                "grant request protection epoch does not match the authority")
        if (self.allowed_model_manifests
                and request.model_manifest_digest
                not in self.allowed_model_manifests):
            raise ValueError(
                "model manifest is not authorized by the grant policy")
        if not set(request.allowed_residency_tiers).issubset(
                self.allowed_residency_tiers):
            raise ValueError(
                "grant request residency tier is not authorized")
        envelope = wrap_content_key(
            recipient_public_key,
            provider_identity=request.provider_identity,
            request_id=request.request_id,
            attempt=request.attempt,
            plan_core_digest=request.plan_core_digest,
            model_manifest_digest=request.model_manifest_digest,
            protection_epoch=request.protection_epoch,
            content_key=content_key,
        )
        unsigned = KeyGrantV1(
            policy_authority=self.identity,
            provider_identity=request.provider_identity,
            request_id=request.request_id,
            attempt=request.attempt,
            plan_core_digest=request.plan_core_digest,
            model_manifest_digest=request.model_manifest_digest,
            protection_epoch=request.protection_epoch,
            key_id=key_id,
            wrapped_content_key=envelope,
            allowed_residency_tiers=request.allowed_residency_tiers,
            issued_at_ms=int(now_ms),
            expires_at_ms=int(expires_at_ms),
            revocation_sequence=self.revocation_sequence,
        )
        grant_digest = unsigned.computed_grant_digest()
        signature = self.private_key.sign(unsigned.signing_bytes()).hex()
        return KeyGrantV1(
            policy_authority=unsigned.policy_authority,
            provider_identity=unsigned.provider_identity,
            request_id=unsigned.request_id,
            attempt=unsigned.attempt,
            plan_core_digest=unsigned.plan_core_digest,
            model_manifest_digest=unsigned.model_manifest_digest,
            protection_epoch=unsigned.protection_epoch,
            key_id=unsigned.key_id,
            wrapped_content_key=unsigned.wrapped_content_key,
            allowed_residency_tiers=unsigned.allowed_residency_tiers,
            issued_at_ms=unsigned.issued_at_ms,
            expires_at_ms=unsigned.expires_at_ms,
            revocation_sequence=unsigned.revocation_sequence,
            grant_digest=grant_digest,
            authority_signature=signature,
        )

    def revoke(self, grant_digests: frozenset[str], *, now_ms: int,
               next_check_at_ms: int = 0) -> RevocationStateV1:
        """Advance the revocation sequence and sign the new ledger state."""
        self._revoked.update(grant_digests)
        self.revocation_sequence += 1
        return self._sign_state(now_ms=now_ms, next_check_at_ms=next_check_at_ms)

    def revocation_state(self, *, now_ms: int,
                         next_check_at_ms: int = 0) -> RevocationStateV1:
        return self._sign_state(now_ms=now_ms, next_check_at_ms=next_check_at_ms)

    def _sign_state(self, *, now_ms: int,
                    next_check_at_ms: int) -> RevocationStateV1:
        unsigned = RevocationStateV1(
            policy_authority=self.identity,
            protection_epoch=self.protection_epoch,
            sequence=self.revocation_sequence,
            revoked_grant_digests=tuple(sorted(self._revoked)),
            issued_at_ms=int(now_ms),
            next_check_at_ms=int(next_check_at_ms),
        )
        signature = self.private_key.sign(unsigned.signing_bytes()).hex()
        return RevocationStateV1(
            policy_authority=unsigned.policy_authority,
            protection_epoch=unsigned.protection_epoch,
            sequence=unsigned.sequence,
            revoked_grant_digests=unsigned.revoked_grant_digests,
            issued_at_ms=unsigned.issued_at_ms,
            next_check_at_ms=unsigned.next_check_at_ms,
            authority_signature=signature,
        )


__all__ = ["ArtifactPolicyAuthority"]
