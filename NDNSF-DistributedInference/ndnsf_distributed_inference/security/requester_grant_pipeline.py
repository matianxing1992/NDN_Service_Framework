"""Requester-side in-process authority grant pipeline (spec181 T001).

The functional slice hosts the policy authority inside the requester
process (spec.md FR-002; the standalone authority service is a deferred
production form).  This module assembles the production pieces that
already exist separately:

- ``ArtifactPolicyAuthority`` issues and signs KeyGrantV1 records after
  verifying the requester signature and the model/epoch/residency policy;
- ``AuthorityBackedGrantProvider`` implements the placement
  ``grant_binding_provider`` seam (signed GrantRequestV1 in, GrantBindingV1
  out) and verifies the authority's answer;
- the requester's ``publish_signed_app_data`` path publishes the grant
  Data under the canonical Spec170 KEY-GRANT/v1 name, which the Provider
  later fetches with an exact-name fetch.

The authority private key loads from the Spec180 operator config registry
(``~/.config/ndnsf/spec180/``, mode 0600).  Content keys are owned by the
requester side and provided per model-manifest digest.
"""

from __future__ import annotations

import time
from typing import Callable

from cryptography.hazmat.primitives.asymmetric import ed25519

from ..core.protected_artifacts import (
    GrantRequestV1, KeyGrantV1, RecipientPublicKey, grant_to_wire)
from .artifact_policy_authority import ArtifactPolicyAuthority
from .grant_provider import (
    AuthorityBackedGrantProvider, canonical_grant_name)
from .registry_keys import load_artifact_policy_authority_private_key


def build_in_process_grant_provider(
    *,
    requester_identity: str,
    requester_private_key: ed25519.Ed25519PrivateKey,
    authority_identity: str,
    authority_private_key: ed25519.Ed25519PrivateKey,
    protection_epoch: str,
    allowed_model_manifests: frozenset[str],
    recipient_public_keys: Callable[[str], RecipientPublicKey | None],
    content_key_owner: Callable[[str, str], bytes] | None = None,
    publisher: Callable[[str, bytes], None],
    grant_ttl_ms: int = 60 * 60 * 1000,
    clock: Callable[[], int] | None = None,
) -> AuthorityBackedGrantProvider:
    """Assemble the production grant seam with an in-process authority.

    ``recipient_public_keys`` maps a provider identity to its recipient
    public key (the authority wraps each content key to the selected
    Provider).  ``publisher`` is the requester's signed-APP-Data
    publication path (``ServiceUser.publish_signed_app_data``); the grant
    Data lands under the canonical KEY-GRANT/v1 name the Provider fetches.
    """
    now = clock or (lambda: int(time.time() * 1000))
    authority = ArtifactPolicyAuthority(
        authority_identity, authority_private_key,
        protection_epoch=protection_epoch,
        allowed_model_manifests=frozenset(allowed_model_manifests),
    )

    def issue_and_publish(request: GrantRequestV1) -> KeyGrantV1:
        recipient_public_key = recipient_public_keys(
            request.provider_identity)
        if recipient_public_key is None:
            raise ValueError(
                f"no recipient key configured for {request.provider_identity}")
        content_key = (
            content_key_owner(request.model_manifest_digest,
                              request.protection_epoch)
            if content_key_owner is not None else None)
        if content_key is None:
            raise ValueError("no content key owned for this model manifest")
        issue_ms = now()
        grant = authority.issue(
            request,
            requester_public_key=requester_private_key.public_key(),
            recipient_public_key=recipient_public_key,
            content_key=content_key,
            key_id=f"spec180-epoch-{request.protection_epoch}-{issue_ms}",
            expires_at_ms=issue_ms + grant_ttl_ms,
            now_ms=issue_ms,
        )
        data_name = canonical_grant_name(
            authority=authority_identity,
            provider_identity=grant.provider_identity,
            request_id=grant.request_id,
            attempt=grant.attempt,
            plan_core_digest=grant.plan_core_digest,
            model_manifest_digest=grant.model_manifest_digest,
            protection_epoch=grant.protection_epoch,
            grant_digest=grant.grant_digest,
        )
        publisher(data_name, grant_to_wire(grant))
        return grant

    return AuthorityBackedGrantProvider(
        requester_identity=requester_identity,
        requester_private_key=requester_private_key,
        authority_public_key=authority_private_key.public_key(),
        authority_identity=authority_identity,
        authority_issue=issue_and_publish,
        content_key_owner=content_key_owner,
        clock=now,
    )


def load_authority_from_registry(
    authority_identity: str,
    config_root: str | None = None,
) -> tuple[str, ed25519.Ed25519PrivateKey]:
    """Load the operator authority key from the Spec180 config registry."""
    return (
        authority_identity,
        load_artifact_policy_authority_private_key(config_root),
    )


__all__ = [
    "build_in_process_grant_provider",
    "load_authority_from_registry",
]
