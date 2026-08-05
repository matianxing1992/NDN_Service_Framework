"""Fail-closed protected model artifact lifecycle for NDNSF-DI."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import hashlib
import hmac
from pathlib import Path
from threading import RLock
from typing import Mapping


def _digest(value) -> str:
    raw = value if isinstance(value, bytes) else str(value).encode()
    return "sha256:" + hashlib.sha256(raw).hexdigest()


class ProtectionState(str, Enum):
    NO_GRANT = "NO_GRANT"
    GRANTED = "GRANTED"
    MATERIALIZED = "MATERIALIZED"
    REVOKED = "REVOKED"
    ZEROIZED = "ZEROIZED"
    FAILED_CLOSED = "FAILED_CLOSED"


@dataclass(frozen=True)
class GrantRequestV1:
    provider: str
    request_id: str
    attempt: int
    plan_core_digest: str
    grant_view_digest: str
    artifact_digest: str
    recipient: str
    purpose: str = "DISK_CIPHERTEXT_ASSEMBLED"

    def __post_init__(self) -> None:
        if (not self.provider or not self.request_id or self.attempt <= 0
                or not self.recipient or self.purpose != "DISK_CIPHERTEXT_ASSEMBLED"):
            raise ValueError("invalid protected artifact grant request")
        for name in ("plan_core_digest", "grant_view_digest", "artifact_digest"):
            if not str(getattr(self, name)).startswith("sha256:"):
                raise ValueError(f"{name} is not canonical")

    def digest(self) -> str:
        return _digest(repr(self))


@dataclass(frozen=True)
class KeyGrantV1:
    request_digest: str
    provider: str
    recipient: str
    policy_epoch: str
    expires_at_ms: int
    wrapped_key: bytes
    authority: str
    signature: str

    def __post_init__(self) -> None:
        if (not self.request_digest.startswith("sha256:") or not self.provider
                or not self.recipient or not self.policy_epoch
                or self.expires_at_ms <= 0 or not self.wrapped_key
                or not self.authority or not self.signature):
            raise ValueError("invalid protected artifact key grant")

    def signing_bytes(self) -> bytes:
        return (self.request_digest + "|" + self.provider + "|"
                + self.recipient + "|" + self.policy_epoch + "|"
                + str(self.expires_at_ms) + "|" + self.wrapped_key.hex()).encode()

    def verify(self, *, authority: str, key: bytes, now_ms: int) -> None:
        if self.authority != authority or int(now_ms) >= self.expires_at_ms:
            raise ValueError("protected key grant is stale or untrusted")
        expected = hmac.new(key, self.signing_bytes(), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(expected, self.signature):
            raise ValueError("protected key grant signature is invalid")


@dataclass(frozen=True)
class RevocationStateV1:
    authority: str
    policy_epoch: str
    sequence: int
    revoked_grants: tuple[str, ...] = ()
    next_check_at_ms: int = 0

    def __post_init__(self) -> None:
        if (not self.authority or not self.policy_epoch or self.sequence <= 0
                or len(set(self.revoked_grants)) != len(self.revoked_grants)):
            raise ValueError("invalid revocation state")

    def is_revoked(self, grant: KeyGrantV1, *, now_ms: int) -> bool:
        if self.next_check_at_ms and int(now_ms) >= self.next_check_at_ms:
            raise ValueError("revocation state is stale")
        return grant.request_digest in set(self.revoked_grants)


class PlaintextLeaseRegistry:
    """Tracks every materialized plaintext path and zeroizes it on close."""

    def __init__(self) -> None:
        self._leases: dict[str, tuple[Path, bytearray]] = {}
        self._lock = RLock()

    def register(self, lease_id: str, path: str | Path, plaintext: bytes) -> None:
        if not lease_id or not plaintext:
            raise ValueError("plaintext lease is incomplete")
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        secret = bytearray(plaintext)
        target.write_bytes(secret)
        with self._lock:
            self._leases[lease_id] = (target, secret)

    def zeroize(self, lease_id: str) -> None:
        with self._lock:
            item = self._leases.pop(lease_id, None)
        if item is None:
            return
        path, secret = item
        for index in range(len(secret)):
            secret[index] = 0
        if path.exists():
            try:
                with path.open("r+b") as output:
                    output.write(b"\x00" * max(1, path.stat().st_size))
                    output.flush()
                path.unlink()
            except OSError as exc:
                raise RuntimeError("plaintext zeroization failed") from exc

    def zeroize_all(self) -> None:
        for lease_id in tuple(self._leases):
            self.zeroize(lease_id)


__all__ = [
    "ProtectionState", "GrantRequestV1", "KeyGrantV1", "RevocationStateV1",
    "PlaintextLeaseRegistry",
]
