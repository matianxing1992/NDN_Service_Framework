"""Small operator-installed authority for protected artifact grants."""

from __future__ import annotations

import hashlib
import hmac

from ..core.protected_artifacts import GrantRequestV1, KeyGrantV1


class ArtifactPolicyAuthority:
    def __init__(self, identity: str, key: bytes, *, policy_epoch: str = "epoch-1"):
        if not identity or len(key) < 16 or not policy_epoch:
            raise ValueError("artifact policy authority is incomplete")
        self.identity = identity
        self.key = bytes(key)
        self.policy_epoch = policy_epoch

    def issue(self, request: GrantRequestV1, *, wrapped_key: bytes,
              expires_at_ms: int) -> KeyGrantV1:
        if request.recipient != request.provider or not wrapped_key:
            raise ValueError("grant recipient/provider mismatch")
        unsigned = KeyGrantV1(
            request_digest=request.digest(), provider=request.provider,
            recipient=request.recipient, policy_epoch=self.policy_epoch,
            expires_at_ms=int(expires_at_ms), wrapped_key=bytes(wrapped_key),
            authority=self.identity, signature="pending")
        signature = hmac.new(self.key, unsigned.signing_bytes(), hashlib.sha256).hexdigest()
        return KeyGrantV1(
            request_digest=unsigned.request_digest, provider=unsigned.provider,
            recipient=unsigned.recipient, policy_epoch=unsigned.policy_epoch,
            expires_at_ms=unsigned.expires_at_ms, wrapped_key=unsigned.wrapped_key,
            authority=unsigned.authority, signature=signature)


__all__ = ["ArtifactPolicyAuthority"]
