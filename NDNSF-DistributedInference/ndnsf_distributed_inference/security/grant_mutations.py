"""Real KeyGrantV1 mutations for the Y-N-E negative matrix (spec181 T006).

Each mutation is a REAL grant whose wire bytes pass parsing and reach the
verifier — the rejection must come from the verifier's own decision
(expiry, envelope authentication, authority signature), never from a
synthetic epoch exception.  These are the three mutations the Y-N-E
subcase feeds through both the Python and the native verifier.
"""

from __future__ import annotations

from dataclasses import replace
from cryptography.hazmat.primitives.asymmetric import ed25519

from ..core.protected_artifacts import KeyGrantV1, wrap_content_key

GRANT_MUTATION_VARIANTS = ("EXPIRED", "WRONG_RECIPIENT", "FORGED_AUTHORITY")


def mutate_for_provider_publication(grant: KeyGrantV1, *, variant: str,
                                    authority_private_key, content_key: bytes,
                                    now_ms: int) -> KeyGrantV1:
    """Construct experiment wire data after normal requester verification.

    The caller seals the resulting digest and publishes via the ordinary
    signed APP Data owner. This function makes no rejection/PASS claim.
    """
    if variant not in GRANT_MUTATION_VARIANTS:
        raise ValueError("GRANT_MUTATION_INVALID")
    if variant == "FORGED_AUTHORITY":
        return mutate_forged_authority(grant, ed25519.Ed25519PrivateKey.generate())
    if variant == "EXPIRED":
        return mutate_expired(
            replace(grant, issued_at_ms=now_ms - 2000),
            authority_private_key, expires_at_ms=now_ms - 1000)
    envelope = wrap_content_key(
        ed25519.Ed25519PrivateKey.generate().public_key(),
        provider_identity=grant.provider_identity, request_id=grant.request_id,
        attempt=grant.attempt, plan_core_digest=grant.plan_core_digest,
        model_manifest_digest=grant.model_manifest_digest,
        protection_epoch=grant.protection_epoch, content_key=content_key)
    changed = replace(grant, wrapped_content_key=envelope,
                      grant_digest="", authority_signature="")
    return replace(changed, grant_digest=changed.computed_grant_digest(),
                   authority_signature=authority_private_key.sign(
                       changed.signing_bytes()).hex())


def mutate_expired(grant: KeyGrantV1, authority_private_key:
                   ed25519.Ed25519PrivateKey,
                   *, expires_at_ms: int) -> KeyGrantV1:
    """Expired mutation: same payload, expired timestamp, digest and
    authority signature rebuilt (a real, verifiable, expired grant)."""
    expired = KeyGrantV1(
        policy_authority=grant.policy_authority,
        provider_identity=grant.provider_identity,
        request_id=grant.request_id, attempt=grant.attempt,
        plan_core_digest=grant.plan_core_digest,
        model_manifest_digest=grant.model_manifest_digest,
        protection_epoch=grant.protection_epoch,
        key_id=grant.key_id,
        wrapped_content_key=grant.wrapped_content_key,
        allowed_residency_tiers=grant.allowed_residency_tiers,
        issued_at_ms=grant.issued_at_ms,
        expires_at_ms=expires_at_ms,
        revocation_sequence=grant.revocation_sequence,
        grant_digest="",
        authority_signature="",
    )
    return KeyGrantV1(
        policy_authority=expired.policy_authority,
        provider_identity=expired.provider_identity,
        request_id=expired.request_id, attempt=expired.attempt,
        plan_core_digest=expired.plan_core_digest,
        model_manifest_digest=expired.model_manifest_digest,
        protection_epoch=expired.protection_epoch,
        key_id=expired.key_id,
        wrapped_content_key=expired.wrapped_content_key,
        allowed_residency_tiers=expired.allowed_residency_tiers,
        issued_at_ms=expired.issued_at_ms,
        expires_at_ms=expired.expires_at_ms,
        revocation_sequence=expired.revocation_sequence,
        grant_digest=expired.computed_grant_digest(),
        authority_signature=authority_private_key.sign(
            expired.signing_bytes()).hex(),
    )


def mutate_forged_authority(grant: KeyGrantV1, evil_private_key:
                            ed25519.Ed25519PrivateKey) -> KeyGrantV1:
    """Forged-authority mutation: a valid-looking grant signed by a key that
    is NOT the configured authority (the verifier must reject the
    signature, not any synthetic path)."""
    forged = KeyGrantV1(
        policy_authority=grant.policy_authority,
        provider_identity=grant.provider_identity,
        request_id=grant.request_id, attempt=grant.attempt,
        plan_core_digest=grant.plan_core_digest,
        model_manifest_digest=grant.model_manifest_digest,
        protection_epoch=grant.protection_epoch,
        key_id=grant.key_id,
        wrapped_content_key=grant.wrapped_content_key,
        allowed_residency_tiers=grant.allowed_residency_tiers,
        issued_at_ms=grant.issued_at_ms,
        expires_at_ms=grant.expires_at_ms,
        revocation_sequence=grant.revocation_sequence,
        grant_digest=grant.computed_grant_digest(),
        authority_signature=evil_private_key.sign(
            grant.signing_bytes()).hex(),
    )
    return forged


def verify_mutation_rejected(verifier, mutation: KeyGrantV1,
                             **verifier_kwargs) -> str:
    """Run one mutation through a real verifier; return the rejection reason.

    Raises AssertionError if the verifier accepts the mutation (a
    Y-N-E failure) or rejects it with a reason outside the registered
    DI_PROTECTED_GRANT_REJECTED / binding-mismatch families.
    """
    try:
        verifier(mutation, **verifier_kwargs)
    except ValueError as exc:
        reason = str(exc)
        if ("grant" not in reason.lower() and "binding" not in reason.lower()
                and "envelope" not in reason.lower()
                and "expired" not in reason.lower()
                and "signature" not in reason.lower()
                and "authentication" not in reason.lower()
                and "digest" not in reason.lower()):
            raise AssertionError(
                f"mutation rejected with unregistered reason: {reason}") from exc
        return reason
    raise AssertionError("grant mutation was accepted by the verifier")


__all__ = [
    "GRANT_MUTATION_VARIANTS",
    "mutate_for_provider_publication",
    "mutate_expired",
    "mutate_forged_authority",
    "verify_mutation_rejected",
]
