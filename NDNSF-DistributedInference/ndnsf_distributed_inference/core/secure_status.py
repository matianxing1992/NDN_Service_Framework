"""Requester-confidential, pull-only Spec 129 status primitives."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import secrets
from typing import Callable

from .contracts import canonical_json


TERMINAL_STATES = frozenset({"COMPLETED", "FAILED", "CANCELED", "EXPIRED", "RELEASED"})


def _aesgcm(key: bytes):
    # Keep the core profile importable for schema/introspection-only tooling;
    # cryptography remains an explicit runtime dependency for status crypto.
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    return AESGCM(key)


@dataclass(frozen=True)
class StatusHandleBinding:
    handle: str
    requester: str
    provider: str
    request_id: str
    attempt: int
    selection_digest: str
    instance_id: str
    role: str
    recipient_key_id: str
    expires_at_ms: int

    @classmethod
    def create(cls, **kwargs):
        return cls(handle=secrets.token_hex(24), **kwargs)

    def __post_init__(self) -> None:
        if (len(self.handle) != 48 or any(ch not in "0123456789abcdef" for ch in self.handle)
                or not all((self.requester, self.provider, self.request_id,
                            self.selection_digest, self.instance_id, self.role,
                            self.recipient_key_id))
                or self.attempt <= 0 or self.expires_at_ms <= 0):
            raise ValueError("invalid StatusHandle binding")


@dataclass(frozen=True)
class StatusQuery:
    handle: str
    requester: str
    request_id: str
    attempt: int
    nonce: str
    issued_at_ms: int
    expires_at_ms: int
    signature: str


@dataclass(frozen=True)
class StatusEvent:
    sequence: int
    state: str
    progress: float | None
    reason: str
    observed_at_ms: int


@dataclass(frozen=True)
class EncryptedStatusSnapshot:
    handle: str
    provider: str
    recipient_key_id: str
    key_epoch: int
    nonce: bytes
    ciphertext: bytes
    sequence: int
    signature: str

    def signed_bytes(self) -> bytes:
        return b"|".join((self.handle.encode(), self.provider.encode(),
                          self.recipient_key_id.encode(), str(self.key_epoch).encode(),
                          self.nonce, self.ciphertext, str(self.sequence).encode()))


class SecureStatusProvider:
    def __init__(self, *, query_verifier: Callable[[StatusQuery], bool],
                 signer: Callable[[bytes], str], max_events: int = 128) -> None:
        self._query_verifier = query_verifier
        self._signer = signer
        self._max_events = max(1, int(max_events))
        self._bindings: dict[str, StatusHandleBinding] = {}
        self._keys: dict[str, tuple[bytes, int, int]] = {}
        self._events: dict[str, list[StatusEvent]] = {}
        self._nonces: set[tuple[str, str]] = set()
        self.query_count = 0
        self.unsolicited_status_count = 0

    def register(self, binding: StatusHandleBinding, key: bytes, *, key_epoch: int = 1) -> None:
        if len(key) != 32 or key_epoch <= 0:
            raise ValueError("status key must be a 32-byte independent epoch key")
        self._bindings[binding.handle] = binding
        self._keys[binding.handle] = (bytes(key), key_epoch, 0)
        self._events[binding.handle] = []

    def transition(self, handle: str, state: str, progress: float | None,
                   reason: str, *, observed_at_ms: int) -> StatusEvent:
        binding = self._bindings[handle]
        if observed_at_ms >= binding.expires_at_ms:
            raise ValueError("status handle expired")
        if progress is not None and not 0.0 <= progress <= 1.0:
            raise ValueError("status progress out of range")
        events = self._events[handle]
        event = StatusEvent((events[-1].sequence if events else 0) + 1,
                            state, progress, reason[:512], observed_at_ms)
        events.append(event)
        if len(events) > self._max_events:
            del events[:-self._max_events]
        # State changes only update local storage; they emit no wire traffic.
        return event

    def _aad(self, binding: StatusHandleBinding, query: StatusQuery,
             key_epoch: int) -> bytes:
        return canonical_json({
            "message": "SECURE-SELECTION-STATUS", "version": 1,
            "handle": binding.handle, "requester": binding.requester,
            "provider": binding.provider, "requestId": binding.request_id,
            "attempt": binding.attempt, "queryNonce": query.nonce,
            "recipientKeyId": binding.recipient_key_id, "keyEpoch": key_epoch,
        }).encode()

    def query(self, query: StatusQuery, *, now_ms: int) -> EncryptedStatusSnapshot:
        binding = self._bindings.get(query.handle)
        if binding is None:
            raise ValueError("unknown status handle")
        replay_key = (query.handle, query.nonce)
        if (query.requester != binding.requester or query.request_id != binding.request_id
                or query.attempt != binding.attempt or not query.nonce
                or query.issued_at_ms > now_ms or query.expires_at_ms <= now_ms
                or query.expires_at_ms > binding.expires_at_ms
                or replay_key in self._nonces or not self._query_verifier(query)):
            raise ValueError("status query authentication, binding, freshness, or replay failed")
        self._nonces.add(replay_key)
        self.query_count += 1
        events = self._events[query.handle]
        latest = events[-1] if events else StatusEvent(0, "UNKNOWN", None, "", now_ms)
        payload = canonical_json({
            "version": 1, "handle": binding.handle,
            "requester": binding.requester, "provider": binding.provider,
            "requestId": binding.request_id, "attempt": binding.attempt,
            "selectionDigest": binding.selection_digest,
            "instanceId": binding.instance_id, "role": binding.role,
            "sequence": latest.sequence, "state": latest.state,
            "progress": latest.progress, "reason": latest.reason,
            "observedAtMs": latest.observed_at_ms,
            "expiresAtMs": binding.expires_at_ms,
        }).encode()
        key, epoch, uses = self._keys[query.handle]
        if uses >= 10_000:
            raise RuntimeError("status key epoch use bound exhausted")
        nonce = secrets.token_bytes(12)
        ciphertext = _aesgcm(key).encrypt(nonce, payload, self._aad(binding, query, epoch))
        unsigned = EncryptedStatusSnapshot(
            binding.handle, binding.provider, binding.recipient_key_id,
            epoch, nonce, ciphertext, latest.sequence, "")
        self._keys[query.handle] = (key, epoch, uses + 1)
        return EncryptedStatusSnapshot(
            **{**unsigned.__dict__, "signature": self._signer(unsigned.signed_bytes())})

    def events_after(self, handle: str, cursor: int) -> tuple[tuple[StatusEvent, ...], bool]:
        events = self._events[handle]
        gap = bool(events and cursor < events[0].sequence - 1)
        return tuple(item for item in events if item.sequence > cursor), gap


class SecureStatusRequester:
    def __init__(self, *, signature_verifier: Callable[[bytes, str], bool]) -> None:
        self._signature_verifier = signature_verifier
        self._last_sequence: dict[str, int] = {}

    def decrypt(self, snapshot: EncryptedStatusSnapshot, binding: StatusHandleBinding,
                query: StatusQuery, key: bytes, *, now_ms: int) -> dict:
        # Signature validation intentionally precedes all decryption work.
        if not self._signature_verifier(snapshot.signed_bytes(), snapshot.signature):
            raise ValueError("Provider status signature invalid")
        if (snapshot.handle != binding.handle or snapshot.provider != binding.provider
                or snapshot.recipient_key_id != binding.recipient_key_id
                or now_ms >= binding.expires_at_ms):
            raise ValueError("encrypted status outer binding invalid")
        aad = canonical_json({
            "message": "SECURE-SELECTION-STATUS", "version": 1,
            "handle": binding.handle, "requester": binding.requester,
            "provider": binding.provider, "requestId": binding.request_id,
            "attempt": binding.attempt, "queryNonce": query.nonce,
            "recipientKeyId": binding.recipient_key_id,
            "keyEpoch": snapshot.key_epoch,
        }).encode()
        try:
            payload = json.loads(_aesgcm(key).decrypt(
                snapshot.nonce, snapshot.ciphertext, aad))
        except Exception as exc:
            raise ValueError("encrypted status authentication failed") from exc
        expected = (binding.handle, binding.requester, binding.provider,
                    binding.request_id, binding.attempt, binding.selection_digest,
                    binding.instance_id, binding.role)
        observed = (payload.get("handle"), payload.get("requester"), payload.get("provider"),
                    payload.get("requestId"), payload.get("attempt"),
                    payload.get("selectionDigest"), payload.get("instanceId"),
                    payload.get("role"))
        sequence = int(payload.get("sequence", -1))
        if observed != expected or sequence <= self._last_sequence.get(binding.handle, -1):
            raise ValueError("status inner binding or monotonic sequence invalid")
        self._last_sequence[binding.handle] = sequence
        return payload


class BoundedStatusPoller:
    def __init__(self, *, initial_interval_ms: int = 250,
                 max_interval_ms: int = 4000, max_queries: int = 32,
                 deadline_ms: int) -> None:
        if min(initial_interval_ms, max_interval_ms, max_queries, deadline_ms) <= 0:
            raise ValueError("polling bounds must be positive")
        self.interval_ms = initial_interval_ms
        self.max_interval_ms = max_interval_ms
        self.max_queries = max_queries
        self.deadline_ms = deadline_ms
        self.query_count = 0
        self.stopped = False

    def admit_query(self, *, now_ms: int) -> int:
        if self.stopped or now_ms >= self.deadline_ms or self.query_count >= self.max_queries:
            self.stopped = True
            raise RuntimeError("status polling stopped or exhausted")
        self.query_count += 1
        delay = self.interval_ms
        self.interval_ms = min(self.max_interval_ms, self.interval_ms * 2)
        return delay

    def observe(self, state: str) -> None:
        if state in TERMINAL_STATES:
            self.stopped = True

    def cancel(self) -> None:
        self.stopped = True
