"""Plan security: signing, verification, namespace validation, expiry.

Plans live under the creator's NDN namespace.  They are signed with the
creator's NDN certificate.  Deployments bind to a specific plan digest.
Expired or revoked plans render deployments DEGRADED.
"""

from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass, field
from typing import Any


def now_ms() -> int:
    return int(time.time() * 1000)


def plan_content_digest(plan: dict[str, Any]) -> str:
    """Compute a stable SHA256 digest of the plan content.

    Only includes fields that must not be tampered with:
    roles, dependencies, fragments, constraints, artifacts.
    """
    content = {
        "planId": plan.get("planId", plan.get("plan_id", "")),
        "service": plan.get("service", plan.get("serviceName", "")),
        "roles": plan.get("roles", []),
        "dependencies": plan.get("dependencies", []),
        "fragments": plan.get("fragments", []),
        "constraints": plan.get("constraints", {}),
        "artifactDigests": plan.get("artifactDigests", []),
    }
    payload = json.dumps(content, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(payload.encode()).hexdigest()


def validate_plan_namespace(plan_id: str, creator: str) -> bool:
    """Verify the plan_id is under the creator's namespace."""
    return plan_id.startswith(creator.rstrip("/") + "/")


def make_plan_id(creator: str, plan_name: str) -> str:
    """Create a properly-namespaced plan ID."""
    return creator.rstrip("/") + "/NDNSF-DI/plans/" + plan_name.lstrip("/")


@dataclass
class PlanSignature:
    plan_id: str = ""
    creator: str = ""
    created_at_ms: int = field(default_factory=now_ms)
    content_digest: str = ""
    signature_bytes: bytes = b""

    def sign(self, content: dict[str, Any], sign_fn) -> None:
        """Sign the plan content using the provided signing function."""
        self.content_digest = plan_content_digest(content)
        payload = json.dumps({
            "planId": self.plan_id,
            "creator": self.creator,
            "createdAtMs": self.created_at_ms,
            "contentDigest": self.content_digest,
        }, sort_keys=True).encode()
        self.signature_bytes = sign_fn(payload)

    def verify(self, content: dict[str, Any], verify_fn) -> tuple[bool, str]:
        """Verify the signature against the plan content."""
        if plan_content_digest(content) != self.content_digest:
            return False, "CONTENT_DIGEST_MISMATCH"
        payload = json.dumps({
            "planId": self.plan_id,
            "creator": self.creator,
            "createdAtMs": self.created_at_ms,
            "contentDigest": self.content_digest,
        }, sort_keys=True).encode()
        return verify_fn(payload, self.signature_bytes)

    def to_dict(self) -> dict[str, Any]:
        import base64
        return {
            "planId": self.plan_id,
            "creator": self.creator,
            "createdAtMs": self.created_at_ms,
            "contentDigest": self.content_digest,
            "signature": base64.b64encode(self.signature_bytes).decode() if self.signature_bytes else "",
        }

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PlanSignature":
        import base64
        sig = payload.get("signature", "")
        sig_bytes = base64.b64decode(sig) if sig else b""
        return cls(
            plan_id=str(payload.get("planId", payload.get("plan_id", ""))),
            creator=str(payload.get("creator", "")),
            created_at_ms=int(payload.get("createdAtMs", payload.get("created_at_ms", 0)) or 0),
            content_digest=str(payload.get("contentDigest", payload.get("content_digest", ""))),
            signature_bytes=sig_bytes,
        )


@dataclass
class PlanState:
    """Tracks plan lifecycle state."""
    plan_id: str
    creator: str = ""
    status: str = "ACTIVE"  # ACTIVE | REVOKED | EXPIRED | SUPERSEDED
    superseded_by: str = ""
    content_digest: str = ""
    created_at_ms: int = field(default_factory=now_ms)
    expires_at_ms: int = 0
    revoked_at_ms: int = 0

    @property
    def is_valid(self) -> bool:
        if self.status == "REVOKED":
            return False
        if self.expires_at_ms and now_ms() > self.expires_at_ms:
            return False
        return True

    def revoke(self) -> None:
        self.status = "REVOKED"
        self.revoked_at_ms = now_ms()

    def supersede(self, new_plan_id: str) -> None:
        self.status = "SUPERSEDED"
        self.superseded_by = new_plan_id

    def to_dict(self) -> dict[str, Any]:
        return {
            "planId": self.plan_id,
            "creator": self.creator,
            "status": self.status,
            "supersededBy": self.superseded_by,
            "contentDigest": self.content_digest,
            "createdAtMs": self.created_at_ms,
            "expiresAtMs": self.expires_at_ms,
            "revokedAtMs": self.revoked_at_ms,
        }

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PlanState":
        return cls(
            plan_id=str(payload.get("planId", payload.get("plan_id", ""))),
            creator=str(payload.get("creator", "")),
            status=str(payload.get("status", "ACTIVE")),
            superseded_by=str(payload.get("supersededBy", payload.get("superseded_by", ""))),
            content_digest=str(payload.get("contentDigest", payload.get("content_digest", ""))),
            created_at_ms=int(payload.get("createdAtMs", payload.get("created_at_ms", 0)) or 0),
            expires_at_ms=int(payload.get("expiresAtMs", payload.get("expires_at_ms", 0)) or 0),
            revoked_at_ms=int(payload.get("revokedAtMs", payload.get("revoked_at_ms", 0)) or 0),
        )
