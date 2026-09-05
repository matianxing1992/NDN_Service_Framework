"""Operator-configured ArtifactPolicyAuthority for protected artifact grants.

Issues Spec170 ``artifact-assembly-v1`` KeyGrantV1 records: it verifies the
requester signature and requester identity, enforces the configured model/
epoch/residency policy, wraps the content key to the selected Provider
identity certificate, and signs the grant with the authority Ed25519 key.

The revocation subsystem (ledger and network service) is intentionally NOT
implemented on this branch: the owner develops it on a separate
branch/machine. ``revocationSequence`` stays a passive wire field fixed at 1
so the later integration does not change the canonical grant bytes. Expiry
is the only time-bound rejection.

The authority identity, endpoint, and trust rule are operator configuration
(Spec180: an entry in ``contracts/trust-root-registry-v1.json``); the private
key lives outside Git like every other Spec180 signing key.
"""

from __future__ import annotations

from cryptography.hazmat.primitives.asymmetric import ed25519

from ..core.protected_artifacts import (
    GrantRequestV1,
    KeyGrantV1,
    RecipientPublicKey,
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
    ) -> None:
        if (not identity or not protection_epoch or not allowed_residency_tiers):
            raise ValueError("artifact policy authority is incomplete")
        self.identity = identity
        self.private_key = private_key
        self.public_key = private_key.public_key()
        self.protection_epoch = protection_epoch
        self.allowed_model_manifests = frozenset(allowed_model_manifests)
        self.allowed_residency_tiers = frozenset(allowed_residency_tiers)

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


__all__ = ["ArtifactPolicyAuthority"]
