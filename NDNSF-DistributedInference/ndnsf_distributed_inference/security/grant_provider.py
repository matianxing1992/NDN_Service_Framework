"""Production grant-binding provider for the Spec170 protected plan path.

Implements the placement ``grant_binding_provider`` seam: turn one sealed
``ProviderGrantViewV1`` into an authority-issued, Provider-bound
``GrantBindingV1``. The requester signs a canonical ``GrantRequestV1``; the
authority (an ``ArtifactPolicyAuthority``, in-process for the functional
slice or a fetched signed Data over NDN) verifies it, wraps the content key
to the Provider certificate, and returns a signed ``KeyGrantV1`` whose
non-secret name/digest reference enters the final plan.

The canonical grant Data name is the Spec170 ``artifact-assembly-v1``
grammar. Name components carry bare hex digests (no ``sha256:`` prefix):

    /<authority>/NDNSF-DI/KEY-GRANT/v1
      /PROVIDER/<sha256(provider-identity)>
      /REQ/<request-id>/ATTEMPT/<attempt>/PLAN-CORE/<plan-core-digest>
      /MODEL/<model-manifest-digest>/EPOCH/<protection-epoch>
      /GRANT/<grant-digest>
"""

from __future__ import annotations

import hashlib
import re
import time
import urllib.parse
from typing import Callable

from cryptography.hazmat.primitives.asymmetric import ed25519

from ..core.protected_artifacts import GrantRequestV1, KeyGrantV1
from ..sdk.placement import GrantBindingV1, ProviderGrantViewV1

_NAME_SAFE = re.compile(r"^[A-Za-z0-9._~-]+$")
_AUTHORITY_NAME_SAFE = re.compile(r"^/[A-Za-z0-9._~/:-]+$")


def _bare_hex_digest(value: str) -> str:
    if not value.startswith("sha256:"):
        raise ValueError("digest is not canonical")
    raw = value[len("sha256:"):]
    if len(raw) != 64:
        raise ValueError("digest length is invalid")
    return raw


def canonical_grant_name(*, authority: str, provider_identity: str,
                         request_id: str, attempt: int, plan_core_digest: str,
                         model_manifest_digest: str, protection_epoch: str,
                         grant_digest: str) -> str:
    """Spec170 KEY-GRANT/v1 Data name for one grant.

    Request identifiers in live NDNSF flows are slash-delimited NDN names
    (e.g. /spec180-y-b-<nonce>); NDN name components cannot contain ``/``,
    so request/epoch strings are percent-encoded component-safely.  The
    Provider fetches the grant by the exact binding name, so both sides
    stay consistent without an extra channel.
    """
    if (not _AUTHORITY_NAME_SAFE.fullmatch(authority)
            or not authority.startswith("/")):
        raise ValueError("authority identity is not name-safe")
    if not request_id or attempt <= 0:
        raise ValueError("request identity is not name-safe")
    if not protection_epoch:
        raise ValueError("protection epoch is not name-safe")
    request_id = urllib.parse.quote(request_id, safe="")
    protection_epoch = urllib.parse.quote(protection_epoch, safe="")
    provider_hex = hashlib.sha256(
        provider_identity.encode("utf-8")).hexdigest()
    return (
        f"{authority}/NDNSF-DI/KEY-GRANT/v1"
        f"/PROVIDER/{provider_hex}"
        f"/REQ/{request_id}/ATTEMPT/{attempt}"
        f"/PLAN-CORE/{_bare_hex_digest(plan_core_digest)}"
        f"/MODEL/{_bare_hex_digest(model_manifest_digest)}"
        f"/EPOCH/{protection_epoch}"
        f"/GRANT/{_bare_hex_digest(grant_digest)}"
    )


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
        if (grant.provider_identity != grant_view.provider
                or grant.request_id != grant_view.request_id
                or grant.attempt != grant_view.attempt
                or grant.plan_core_digest != grant_view.plan_core_digest
                or grant.model_manifest_digest != grant_view.model_manifest_digest
                or grant.protection_epoch != grant_view.protection_epoch):
            raise ValueError("authority grant does not cover the sealed view")
        grant_name = canonical_grant_name(
            authority=self.authority_identity,
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
