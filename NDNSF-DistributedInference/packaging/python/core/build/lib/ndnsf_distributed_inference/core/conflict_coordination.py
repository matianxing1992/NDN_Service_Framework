"""Workload-neutral Spec 130 conflict admission and evidence primitives.

Provider declarations and ledgers remain authoritative for physical ownership.
This module orders overlapping request attempts and never fabricates ownership.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import hashlib
import hmac
import json
from threading import RLock
from typing import Any, Iterable, Mapping


CAPABILITY = "DIConflictAdmissionV1"


def _text(value: Any, field_name: str) -> str:
    result = str(value).strip()
    if not result:
        raise ValueError(f"{field_name} must be non-empty")
    return result


def _digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


@dataclass(frozen=True, order=True)
class CanonicalResourceKey:
    provider_identity: str
    provider_boot_epoch: str
    resource_class: str
    resource_id: str
    exclusivity_domain: str
    capacity_unit: str
    declaration_version: str

    def __post_init__(self) -> None:
        for name, value in asdict(self).items():
            object.__setattr__(self, name, _text(value, name))

    @property
    def stable_id(self) -> str:
        return _digest(asdict(self))

    @property
    def physical_identity(self) -> tuple[str, str, str, str]:
        return (self.provider_identity, self.provider_boot_epoch,
                self.exclusivity_domain, self.resource_id)


@dataclass(frozen=True)
class ResourceDeclaration:
    key: CanonicalResourceKey
    capacity: int
    exclusive: bool
    resource_sequence: int
    authenticated: bool = True
    declaration_digest: str = ""

    def __post_init__(self) -> None:
        if self.capacity <= 0 or self.resource_sequence <= 0:
            raise ValueError("resource capacity and sequence must be positive")
        if not self.authenticated:
            raise PermissionError("resource declaration is not authenticated")
        canonical = {"key": asdict(self.key), "capacity": self.capacity,
                     "exclusive": self.exclusive,
                     "resource_sequence": self.resource_sequence}
        expected = _digest(canonical)
        if self.declaration_digest and self.declaration_digest != expected:
            raise ValueError("resource declaration digest mismatch")
        object.__setattr__(self, "declaration_digest", expected)


@dataclass(frozen=True)
class ResourceClaim:
    key: CanonicalResourceKey
    quantity: int = 1
    exclusive: bool = True

    def __post_init__(self) -> None:
        if self.quantity <= 0:
            raise ValueError("claim quantity must be positive")


@dataclass(frozen=True)
class RequestAttempt:
    requester_identity: str
    request_id: str
    attempt: int
    absolute_deadline: int
    claims: tuple[ResourceClaim, ...]
    capability: str = CAPABILITY
    authenticated: bool = True

    def __post_init__(self) -> None:
        object.__setattr__(self, "requester_identity",
                           _text(self.requester_identity, "requester_identity"))
        object.__setattr__(self, "request_id", _text(self.request_id, "request_id"))
        object.__setattr__(self, "claims", tuple(self.claims))
        if self.attempt <= 0 or self.absolute_deadline <= 0 or not self.claims:
            raise ValueError("invalid request attempt")
        if len({claim.key for claim in self.claims}) != len(self.claims):
            raise ValueError("duplicate or aliased resource claim")
        if not self.authenticated:
            raise PermissionError("request attempt is not authenticated")

    @property
    def identity(self) -> tuple[str, str, int]:
        return self.requester_identity, self.request_id, self.attempt

    @property
    def digest(self) -> str:
        return _digest({"requester": self.requester_identity,
                        "request_id": self.request_id, "attempt": self.attempt,
                        "deadline": self.absolute_deadline,
                        "capability": self.capability,
                        "claims": [{"key": asdict(c.key), "quantity": c.quantity,
                                    "exclusive": c.exclusive} for c in self.claims]})


@dataclass(frozen=True)
class AuthorityEpoch:
    scope: str
    authority_identity: str
    epoch: int
    boot_epoch: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "scope", _text(self.scope, "scope"))
        object.__setattr__(self, "authority_identity",
                           _text(self.authority_identity, "authority_identity"))
        object.__setattr__(self, "boot_epoch", _text(self.boot_epoch, "boot_epoch"))
        if self.epoch <= 0:
            raise ValueError("authority epoch must be positive")

    @property
    def digest(self) -> str:
        return _digest(asdict(self))


@dataclass(frozen=True)
class LedgerEvent:
    event_id: str
    sequence: int
    event_kind: str
    authority_digest: str
    request_identity: tuple[str, str, int] | None
    subject_digest: str
    causal_predecessor: str
    logical_time: int
    digest: str


@dataclass
class AdmissionPermit:
    permit_id: str
    authority: AuthorityEpoch
    request: RequestAttempt
    issued_at: int
    expires_at: int
    submission_sequence: int
    state: str = "GRANTED"
    activated_at: int | None = None
    released_at: int | None = None
    terminal_reason: str = ""
    reservation_bindings: dict[str, str] = field(default_factory=dict)

    @property
    def digest(self) -> str:
        return _digest({"permit_id": self.permit_id,
                        "authority": asdict(self.authority),
                        "request_digest": self.request.digest,
                        "issued_at": self.issued_at,
                        "expires_at": self.expires_at,
                        "submission_sequence": self.submission_sequence})


def declarations_conflict(left: ResourceClaim, right: ResourceClaim,
                          declaration: ResourceDeclaration) -> bool:
    if left.key != right.key:
        return False
    return (declaration.exclusive or left.exclusive or right.exclusive
            or left.quantity + right.quantity > declaration.capacity)


def issue_permit_envelope(permit: AdmissionPermit, signing_key: bytes) -> dict[str, Any]:
    """Create an application-level permit that Providers can verify offline."""
    if not signing_key:
        raise ValueError("permit signing key must be non-empty")
    body = {
        "schema": "ndnsf-di-conflict-permit-v1",
        "permitId": permit.permit_id,
        "authority": asdict(permit.authority),
        "requesterIdentity": permit.request.requester_identity,
        "requestId": permit.request.request_id,
        "attempt": permit.request.attempt,
        "requestDigest": permit.request.digest,
        "issuedAt": permit.issued_at,
        "expiresAt": permit.expires_at,
        "claims": [{"resource": claim.key.stable_id,
                    "providerIdentity": claim.key.provider_identity,
                    "providerBootEpoch": claim.key.provider_boot_epoch,
                    "quantity": claim.quantity}
                   for claim in permit.request.claims],
    }
    signature = hmac.new(bytes(signing_key), json.dumps(
        body, sort_keys=True, separators=(",", ":")).encode(),
        hashlib.sha256).hexdigest()
    return {**body, "signature": signature}


def verify_permit_envelope(envelope: Mapping[str, Any], signing_key: bytes, *,
                           expected_authority: AuthorityEpoch,
                           expected_request_identity: tuple[str, str, int],
                           expected_resource: CanonicalResourceKey,
                           now: int) -> dict[str, Any]:
    """Validate one target claim without contacting the coordinator in ACK."""
    value = dict(envelope); signature = str(value.pop("signature", ""))
    expected_signature = hmac.new(bytes(signing_key), json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode(),
        hashlib.sha256).hexdigest()
    if not signature or not hmac.compare_digest(signature, expected_signature):
        raise PermissionError("conflict permit signature mismatch")
    if value.get("schema") != "ndnsf-di-conflict-permit-v1":
        raise ValueError("conflict permit schema mismatch")
    if dict(value.get("authority") or {}) != asdict(expected_authority):
        raise RuntimeError("conflict permit authority epoch mismatch")
    identity = (str(value.get("requesterIdentity", "")),
                str(value.get("requestId", "")), int(value.get("attempt", 0)))
    if identity != expected_request_identity:
        raise RuntimeError("conflict permit request binding mismatch")
    if now < int(value.get("issuedAt", 0)) or now >= int(value.get("expiresAt", 0)):
        raise RuntimeError("conflict permit is not live")
    claims = [dict(item) for item in value.get("claims", ())]
    matches = [item for item in claims
               if item.get("resource") == expected_resource.stable_id
               and item.get("providerIdentity") == expected_resource.provider_identity
               and item.get("providerBootEpoch") == expected_resource.provider_boot_epoch]
    if len(matches) != 1 or int(matches[0].get("quantity", 0)) <= 0:
        raise RuntimeError("conflict permit lacks exact target claim")
    return {"admitted": True, "permitId": str(value["permitId"]),
            "authorityEpoch": expected_authority.epoch,
            "authorityDigest": expected_authority.digest,
            "canonicalResourceId": expected_resource.stable_id,
            "resourceSequence": 1,
            "quantity": int(matches[0]["quantity"])}


class ConflictAdmissionCoordinator:
    """Epoch-fenced all-or-none admission over Provider declarations."""

    def __init__(self, authority: AuthorityEpoch, *, max_permit_ms: int = 60000) -> None:
        if max_permit_ms <= 0:
            raise ValueError("max_permit_ms must be positive")
        self._lock = RLock()
        self.authority = authority
        self.max_permit_ms = max_permit_ms
        self.available = True
        self.cleanup_only = False
        self._declarations: dict[CanonicalResourceKey, ResourceDeclaration] = {}
        self._physical_keys: dict[tuple[str, str, str, str], CanonicalResourceKey] = {}
        self._queued: dict[tuple[str, str, int], tuple[int, RequestAttempt]] = {}
        self._permits: dict[str, AdmissionPermit] = {}
        self._attempt_permits: dict[tuple[str, str, int], str] = {}
        self._events: list[LedgerEvent] = []
        self._event_digests: dict[str, str] = {}
        self._sequence = 0
        self._submission_sequence = 0

    @property
    def events(self) -> tuple[LedgerEvent, ...]:
        return tuple(self._events)

    @property
    def declarations(self) -> tuple[ResourceDeclaration, ...]:
        return tuple(self._declarations.values())

    @property
    def permits(self) -> tuple[AdmissionPermit, ...]:
        return tuple(self._permits.values())

    def _append(self, kind: str, *, logical_time: int, subject: Any,
                request_identity: tuple[str, str, int] | None = None,
                event_id: str = "") -> LedgerEvent:
        subject_digest = subject if isinstance(subject, str) else _digest(subject)
        predecessor = self._events[-1].digest if self._events else "GENESIS"
        sequence = self._sequence + 1
        eid = event_id or _digest({"scope": self.authority.scope,
                                   "sequence": sequence, "kind": kind,
                                   "subject": subject_digest})[:32]
        body = {"event_id": eid, "sequence": sequence, "event_kind": kind,
                "authority_digest": self.authority.digest,
                "request_identity": request_identity,
                "subject_digest": subject_digest,
                "causal_predecessor": predecessor, "logical_time": logical_time}
        digest = _digest(body)
        previous = self._event_digests.get(eid)
        if previous is not None:
            if previous != digest:
                raise RuntimeError("conflicting duplicate ledger event")
            return next(event for event in self._events if event.event_id == eid)
        event = LedgerEvent(digest=digest, **body)
        self._events.append(event)
        self._event_digests[eid] = digest
        self._sequence = sequence
        return event

    def register_declarations(self, declarations: Iterable[ResourceDeclaration],
                              *, logical_time: int = 0) -> None:
        with self._lock:
            for declaration in declarations:
                key = declaration.key
                if key.provider_boot_epoch == "":
                    raise ValueError("resource declaration lacks provider boot epoch")
                physical = key.physical_identity
                existing_key = self._physical_keys.get(physical)
                if existing_key is not None and existing_key != key:
                    raise ValueError("aliased physical resource declaration")
                existing = self._declarations.get(key)
                if existing is not None:
                    if existing.declaration_digest == declaration.declaration_digest:
                        continue
                    if declaration.resource_sequence <= existing.resource_sequence:
                        raise RuntimeError("stale or conflicting resource declaration")
                    if any(self._claim_live(key)):
                        raise RuntimeError("cannot reinterpret a live resource")
                self._declarations[key] = declaration
                self._physical_keys[physical] = key
                self._append("DECLARATION", logical_time=logical_time,
                             subject=declaration.declaration_digest)

    def _claim_live(self, key: CanonicalResourceKey) -> Iterable[AdmissionPermit]:
        return (permit for permit in self._permits.values()
                if permit.state in {"GRANTED", "ACTIVE"}
                and any(claim.key == key for claim in permit.request.claims))

    def _validate_attempt(self, request: RequestAttempt) -> None:
        if request.capability != CAPABILITY:
            raise RuntimeError("exclusive admission capability mismatch")
        for claim in request.claims:
            declaration = self._declarations.get(claim.key)
            if declaration is None:
                raise RuntimeError("unknown or unverifiable resource declaration")
            if claim.quantity > declaration.capacity:
                raise RuntimeError("resource claim exceeds authoritative capacity")
            if declaration.exclusive and not claim.exclusive:
                raise RuntimeError("requester cannot weaken Provider exclusivity")

    def submit(self, request: RequestAttempt, *, now: int) -> str:
        with self._lock:
            self._validate_attempt(request)
            if now >= request.absolute_deadline:
                return "DEADLINE_EXPIRED"
            existing_permit = self._attempt_permits.get(request.identity)
            if existing_permit:
                permit = self._permits[existing_permit]
                if permit.request.digest != request.digest:
                    raise RuntimeError("conflicting duplicate request attempt")
                return permit.state
            queued = self._queued.get(request.identity)
            if queued:
                if queued[1].digest != request.digest:
                    raise RuntimeError("conflicting duplicate request attempt")
                return "QUEUED"
            self._submission_sequence += 1
            self._queued[request.identity] = (self._submission_sequence, request)
            self._append("SUBMIT", logical_time=now, subject=request.digest,
                         request_identity=request.identity)
            return "QUEUED"

    def _requests_conflict(self, left: RequestAttempt, right: RequestAttempt) -> bool:
        right_by_key = {claim.key: claim for claim in right.claims}
        for claim in left.claims:
            other = right_by_key.get(claim.key)
            if other is not None and declarations_conflict(
                    claim, other, self._declarations[claim.key]):
                return True
        return False

    def grant_next(self, authority: AuthorityEpoch, *, now: int,
                   permit_ms: int) -> tuple[AdmissionPermit, ...]:
        with self._lock:
            if authority != self.authority:
                raise RuntimeError("stale or competing authority epoch")
            if not self.available or self.cleanup_only:
                raise RuntimeError("authority unavailable for new admission")
            if permit_ms <= 0:
                raise ValueError("permit_ms must be positive")
            self.expire(now=now)
            selected: list[tuple[int, RequestAttempt]] = []
            live = [permit.request for permit in self._permits.values()
                    if permit.state in {"GRANTED", "ACTIVE"}]
            for sequence, request in sorted(
                    self._queued.values(), key=lambda item: (
                        item[0], item[1].requester_identity,
                        item[1].request_id, item[1].attempt)):
                if now >= request.absolute_deadline:
                    self._queued.pop(request.identity, None)
                    self._append("DEADLINE_EXPIRED", logical_time=now,
                                 subject=request.digest,
                                 request_identity=request.identity)
                    continue
                if any(self._requests_conflict(request, other)
                       for other in live + [item[1] for item in selected]):
                    continue
                selected.append((sequence, request))
            result = []
            for sequence, request in selected:
                permit_id = _digest({"authority": authority.digest,
                                     "request": request.digest,
                                     "submission": sequence})[:32]
                expires = min(request.absolute_deadline,
                              now + min(permit_ms, self.max_permit_ms))
                permit = AdmissionPermit(permit_id, authority, request, now,
                                          expires, sequence)
                self._permits[permit_id] = permit
                self._attempt_permits[request.identity] = permit_id
                self._queued.pop(request.identity, None)
                self._append("GRANT", logical_time=now, subject=permit.digest,
                             request_identity=request.identity)
                result.append(permit)
            return tuple(result)

    def activate(self, permit_id: str, bindings: Mapping[str, Mapping[str, Any]],
                 *, now: int) -> AdmissionPermit:
        with self._lock:
            permit = self._permits.get(permit_id)
            if permit is None or permit.state not in {"GRANTED", "ACTIVE"}:
                raise RuntimeError("permit is not activatable")
            if permit.state == "ACTIVE":
                return permit
            if now >= permit.expires_at:
                self.expire(now=now)
                raise RuntimeError("permit expired before activation")
            validated: dict[str, str] = {}
            for claim in permit.request.claims:
                binding = bindings.get(claim.key.stable_id)
                if not binding:
                    raise RuntimeError("partial reservation set cannot execute")
                if (tuple(binding.get("request_identity", ())) != permit.request.identity
                        or binding.get("provider_boot_epoch") != claim.key.provider_boot_epoch
                        or not binding.get("live")
                        or int(binding.get("quantity", 0)) < claim.quantity):
                    raise RuntimeError("reservation binding does not authorize claim")
                validated[claim.key.stable_id] = _text(
                    binding.get("reservation_id"), "reservation_id")
            permit.reservation_bindings = validated
            permit.state = "ACTIVE"
            permit.activated_at = now
            self._append("ACTIVATE", logical_time=now, subject=permit.digest,
                         request_identity=permit.request.identity)
            return permit

    def release(self, permit_id: str, *, now: int, reason: str) -> bool:
        with self._lock:
            permit = self._permits.get(permit_id)
            if permit is None or permit.state in {"RELEASED", "REJECTED",
                                                  "DEADLINE_EXPIRED"}:
                return False
            permit.state = "RELEASED"
            permit.released_at = now
            permit.terminal_reason = _text(reason, "reason")
            self._append("RELEASE", logical_time=now,
                         subject={"permit": permit.digest, "reason": reason},
                         request_identity=permit.request.identity)
            return True

    def expire(self, *, now: int) -> int:
        expired = 0
        with self._lock:
            for permit in self._permits.values():
                if permit.state in {"GRANTED", "ACTIVE"} and now >= permit.expires_at:
                    permit.state = "RELEASED"
                    permit.released_at = now
                    permit.terminal_reason = "LEASE_EXPIRED"
                    self._append("LEASE_EXPIRED", logical_time=now,
                                 subject=permit.digest,
                                 request_identity=permit.request.identity)
                    expired += 1
            return expired

    def set_available(self, available: bool, *, cleanup_only: bool = False,
                      now: int = 0) -> None:
        with self._lock:
            self.available = bool(available)
            self.cleanup_only = bool(cleanup_only)
            self._append("AUTHORITY_MODE", logical_time=now,
                         subject={"available": self.available,
                                  "cleanup_only": self.cleanup_only})

    def rotate_authority(self, authority: AuthorityEpoch, *, now: int) -> None:
        with self._lock:
            if authority.scope != self.authority.scope:
                raise ValueError("authority scope changed")
            if authority.epoch <= self.authority.epoch:
                raise RuntimeError("stale or competing authority epoch")
            if any(p.state in {"GRANTED", "ACTIVE"} for p in self._permits.values()):
                raise RuntimeError("cannot rotate authority with live permits")
            self.authority = authority
            self._append("AUTHORITY_ROTATE", logical_time=now,
                         subject=authority.digest)

    def ownership_intervals(self) -> list[dict[str, Any]]:
        intervals = []
        for permit in self._permits.values():
            if permit.activated_at is None:
                continue
            end = permit.released_at if permit.released_at is not None else permit.expires_at
            for claim in permit.request.claims:
                intervals.append({"resource": claim.key.stable_id,
                                  "request_identity": permit.request.identity,
                                  "start": permit.activated_at, "end": end,
                                  "half_open": True, "quantity": claim.quantity})
        return intervals

    def assert_safe(self) -> None:
        by_resource: dict[str, list[dict[str, Any]]] = {}
        for interval in self.ownership_intervals():
            by_resource.setdefault(interval["resource"], []).append(interval)
        for intervals in by_resource.values():
            for index, left in enumerate(intervals):
                for right in intervals[index + 1:]:
                    if max(left["start"], right["start"]) < min(left["end"], right["end"]):
                        raise AssertionError("overlapping exclusive ownership intervals")

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "schema": "spec130-conflict-coordinator-v1",
                "authority": asdict(self.authority),
                "max_permit_ms": self.max_permit_ms,
                "available": self.available,
                "cleanup_only": self.cleanup_only,
                "sequence": self._sequence,
                "submission_sequence": self._submission_sequence,
                "declarations": [{"key": asdict(d.key), "capacity": d.capacity,
                                  "exclusive": d.exclusive,
                                  "resource_sequence": d.resource_sequence,
                                  "authenticated": d.authenticated,
                                  "declaration_digest": d.declaration_digest}
                                 for d in self._declarations.values()],
                "queued": [{"sequence": sequence, "request": _request_dict(request)}
                           for sequence, request in self._queued.values()],
                "permits": [_permit_dict(permit) for permit in self._permits.values()],
                "events": [asdict(event) for event in self._events],
            }

    @classmethod
    def restore(cls, snapshot: Mapping[str, Any]) -> "ConflictAdmissionCoordinator":
        if snapshot.get("schema") != "spec130-conflict-coordinator-v1":
            raise ValueError("unsupported coordinator snapshot")
        coordinator = cls(AuthorityEpoch(**dict(snapshot["authority"])),
                          max_permit_ms=int(snapshot["max_permit_ms"]))
        coordinator.available = bool(snapshot.get("available", True))
        coordinator.cleanup_only = bool(snapshot.get("cleanup_only", False))
        for raw in snapshot.get("declarations", []):
            value = dict(raw)
            value["key"] = CanonicalResourceKey(**value["key"])
            declaration = ResourceDeclaration(**value)
            coordinator._declarations[declaration.key] = declaration
            coordinator._physical_keys[declaration.key.physical_identity] = declaration.key
        for raw in snapshot.get("queued", []):
            request = _request_from_dict(raw["request"])
            coordinator._queued[request.identity] = (int(raw["sequence"]), request)
        for raw in snapshot.get("permits", []):
            permit = _permit_from_dict(raw)
            coordinator._permits[permit.permit_id] = permit
            coordinator._attempt_permits[permit.request.identity] = permit.permit_id
        events = [LedgerEvent(**{**dict(raw),
                                "request_identity": tuple(raw["request_identity"])
                                if raw.get("request_identity") else None})
                  for raw in snapshot.get("events", [])]
        previous = "GENESIS"
        for expected, event in enumerate(events, 1):
            if event.sequence != expected or event.causal_predecessor != previous:
                raise RuntimeError("coordinator event log gap or fork")
            body = asdict(event); digest = body.pop("digest")
            if _digest(body) != digest:
                raise RuntimeError("coordinator event digest mismatch")
            if event.authority_digest != coordinator.authority.digest and event.event_kind != "AUTHORITY_ROTATE":
                raise RuntimeError("coordinator event authority mismatch")
            coordinator._events.append(event)
            coordinator._event_digests[event.event_id] = event.digest
            previous = event.digest
        coordinator._sequence = int(snapshot.get("sequence", 0))
        coordinator._submission_sequence = int(snapshot.get("submission_sequence", 0))
        if coordinator._sequence != len(events):
            raise RuntimeError("coordinator snapshot sequence mismatch")
        coordinator.assert_safe()
        return coordinator


def _request_dict(request: RequestAttempt) -> dict[str, Any]:
    return {"requester_identity": request.requester_identity,
            "request_id": request.request_id, "attempt": request.attempt,
            "absolute_deadline": request.absolute_deadline,
            "capability": request.capability, "authenticated": request.authenticated,
            "claims": [{"key": asdict(claim.key), "quantity": claim.quantity,
                        "exclusive": claim.exclusive} for claim in request.claims]}


def _request_from_dict(raw: Mapping[str, Any]) -> RequestAttempt:
    value = dict(raw)
    value["claims"] = tuple(ResourceClaim(CanonicalResourceKey(**claim["key"]),
                                          int(claim["quantity"]),
                                          bool(claim["exclusive"]))
                            for claim in value["claims"])
    return RequestAttempt(**value)


def _permit_dict(permit: AdmissionPermit) -> dict[str, Any]:
    return {"permit_id": permit.permit_id, "authority": asdict(permit.authority),
            "request": _request_dict(permit.request), "issued_at": permit.issued_at,
            "expires_at": permit.expires_at,
            "submission_sequence": permit.submission_sequence,
            "state": permit.state, "activated_at": permit.activated_at,
            "released_at": permit.released_at,
            "terminal_reason": permit.terminal_reason,
            "reservation_bindings": dict(permit.reservation_bindings)}


def _permit_from_dict(raw: Mapping[str, Any]) -> AdmissionPermit:
    value = dict(raw)
    value["authority"] = AuthorityEpoch(**value["authority"])
    value["request"] = _request_from_dict(value["request"])
    return AdmissionPermit(**value)
