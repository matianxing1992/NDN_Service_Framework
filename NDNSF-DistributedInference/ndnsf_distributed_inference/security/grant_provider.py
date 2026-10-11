"""Production grant-binding provider for the Spec170 protected plan path.

Implements the placement ``grant_binding_provider`` seam: turn one sealed
``ProviderGrantViewV1`` into an authority-issued, Provider-bound
``GrantBindingV1``. The requester signs a canonical ``GrantRequestV1``; the
authority (an ``ArtifactPolicyAuthority``, in-process for the functional
slice or a fetched signed Data over NDN) verifies it, wraps the content key
to the Provider certificate, and returns a signed ``KeyGrantV1`` whose
non-secret name/digest reference enters the final plan.

Grant Data is named by the shared native C08 codec under its actual
publication actor. The logical policy authority remains authenticated in
the signed grant payload; it does not determine the Data producer prefix.
"""

from __future__ import annotations

import time
from typing import Callable

from cryptography.hazmat.primitives.asymmetric import ed25519

from ..core.protected_artifacts import GrantRequestV1, KeyGrantV1
from ..sdk.placement import GrantBindingV1, ProviderGrantViewV1

def canonical_grant_name(*, publication_identity: str, provider_identity: str,
                         request_id: str, attempt: int, plan_core_digest: str,
                         model_manifest_digest: str, protection_epoch: str,
                         grant_digest: str) -> str:
    """Use the native producer-owned C08 grammar without duplicating its codec."""
    from ndnsf import _ndnsf
    return str(_ndnsf.canonical_native_grant_name(
        publication_identity, provider_identity, request_id, attempt,
        plan_core_digest, model_manifest_digest, protection_epoch, grant_digest))


class AuthorityBackedGrantProvider:
    """Placement grant seam backed by a real ArtifactPolicyAuthority."""

    def __init__(
        self,
        *,
        requester_identity: str,
        requester_private_key: ed25519.Ed25519PrivateKey,
        authority_public_key: ed25519.Ed25519PublicKey,
        authority_issue: Callable[[GrantRequestV1], KeyGrantV1],
        authority_identity: str,
        publication_identity: str | None = None,
        content_key_owner: Callable[[str, str], bytes] | None = None,
        clock: Callable[[], int] | None = None,
    ) -> None:
        """Build the seam.

        ``authority_issue`` is the authority endpoint: an in-process
        ``ArtifactPolicyAuthority.issue`` closure for the local functional
        slice, or a network-backed callable that exchanges the signed request
        for the signed grant Data.
        """
        if not requester_identity or not authority_identity:
            raise ValueError("grant provider identities are incomplete")
        self.requester_identity = requester_identity
        self.requester_private_key = requester_private_key
        self.authority_public_key = authority_public_key
        self.authority_identity = authority_identity
        self.publication_identity = publication_identity or requester_identity
        self.authority_issue = authority_issue
        self.content_key_owner = content_key_owner
        self._clock = clock or (lambda: int(time.time() * 1000))

    def __call__(self, grant_view: ProviderGrantViewV1,
                 deadline_ms: int) -> GrantBindingV1:
        if grant_view.protection_epoch == "plaintext-v1":
            raise ValueError(
                "plaintext roles must not enter the grant path")
        if not grant_view.model_manifest_digest:
            raise ValueError(
                "protected grant view lacks the model-manifest digest")
        request = GrantRequestV1(
            provider_identity=grant_view.provider,
            request_id=grant_view.request_id,
            attempt=grant_view.attempt,
            plan_core_digest=grant_view.plan_core_digest,
            grant_view_digest=grant_view.digest(),
            model_manifest_digest=grant_view.model_manifest_digest,
            protection_epoch=grant_view.protection_epoch,
            requester_identity=self.requester_identity,
            issued_at_ms=self._clock(),
        ).sign(self.requester_private_key)
        if deadline_ms and int(deadline_ms) <= self._clock():
            raise TimeoutError("grant acquisition deadline expired")
        grant = self.authority_issue(request)
        if not isinstance(grant, KeyGrantV1):
            raise TypeError("authority did not return KeyGrantV1")
        grant.verify(self.authority_public_key, now_ms=self._clock())
        if (grant.policy_authority != self.authority_identity
                or grant.provider_identity != grant_view.provider
                or grant.request_id != grant_view.request_id
                or grant.attempt != grant_view.attempt
                or grant.plan_core_digest != grant_view.plan_core_digest
                or grant.model_manifest_digest != grant_view.model_manifest_digest
                or grant.protection_epoch != grant_view.protection_epoch):
            raise ValueError("authority grant does not cover the sealed view")
        grant_name = canonical_grant_name(
            publication_identity=self.publication_identity,
            provider_identity=grant_view.provider,
            request_id=grant_view.request_id,
            attempt=grant_view.attempt,
            plan_core_digest=grant_view.plan_core_digest,
            model_manifest_digest=grant_view.model_manifest_digest,
            protection_epoch=grant_view.protection_epoch,
            grant_digest=grant.grant_digest,
        )
        return GrantBindingV1(
            provider=grant_view.provider,
            grant_name=grant_name,
            grant_digest=grant.grant_digest,
            request_id=grant_view.request_id,
            attempt=grant_view.attempt,
            plan_core_digest=grant_view.plan_core_digest,
            security_policy_snapshot_digest=(
                grant_view.security_policy_snapshot_digest),
            protection_epoch=grant_view.protection_epoch,
        )


__all__ = ["AuthorityBackedGrantProvider", "canonical_grant_name"]
