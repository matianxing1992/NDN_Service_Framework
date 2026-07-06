"""Generic NDNSF coordination envelopes.

These classes are service-neutral. Applications may put service-specific
planning or workflow payloads in ``payload`` while the NDNSF layer owns common
freshness, proof, nonce, and context-matching fields.
"""

from __future__ import annotations

import hashlib
import json
import time
from dataclasses import asdict, dataclass, field, is_dataclass, replace
from enum import Enum
from typing import Any, Callable, Iterable


class CoordinationMode(str, Enum):
    DISABLED = "disabled"
    ADVISORY = "advisory"


def coordination_now_ms() -> int:
    return int(time.time() * 1000)


def coordination_to_plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        return {key: coordination_to_plain(item) for key, item in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): coordination_to_plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [coordination_to_plain(item) for item in value]
    return value


def coordination_stable_json(payload: Any) -> str:
    return json.dumps(coordination_to_plain(payload), sort_keys=True, separators=(",", ":"))


def coordination_stable_digest(payload: Any, *, length: int = 16) -> str:
    return hashlib.sha256(coordination_stable_json(payload).encode("utf-8")).hexdigest()[:length]


@dataclass(frozen=True)
class CoordinationIntent:
    intent_id: str = ""
    request_id: str = ""
    requester_name: str = ""
    service_name: str = ""
    purpose: str = "advisory"
    utility_weight: float = 1.0
    deadline_ms: int = 0
    nonce: str = ""
    created_at_ms: int = field(default_factory=coordination_now_ms)
    expires_at_ms: int = 0
    payload_schema: str = ""
    payload: dict[str, Any] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "CoordinationIntent":
        return cls(
            intent_id=str(payload.get("intentId", payload.get("intent_id", ""))),
            request_id=str(payload.get("requestId", payload.get("request_id", ""))),
            requester_name=str(payload.get("requesterName", payload.get("requester_name", ""))),
            service_name=str(payload.get("serviceName", payload.get("service_name", ""))),
            purpose=str(payload.get("purpose", "advisory")),
            utility_weight=float(payload.get("utilityWeight", payload.get("utility_weight", 1.0)) or 1.0),
            deadline_ms=int(payload.get("deadlineMs", payload.get("deadline_ms", 0)) or 0),
            nonce=str(payload.get("nonce", "")),
            created_at_ms=int(payload.get("createdAtMs", payload.get("created_at_ms", coordination_now_ms())) or 0),
            expires_at_ms=int(payload.get("expiresAtMs", payload.get("expires_at_ms", 0)) or 0),
            payload_schema=str(payload.get("payloadSchema", payload.get("payload_schema", ""))),
            payload=dict(payload.get("payload", {}) or {}),
            metadata=dict(payload.get("metadata", {}) or {}),
        )

    def is_valid(self, *, now_ms_value: int | None = None) -> bool:
        current = coordination_now_ms() if now_ms_value is None else int(now_ms_value)
        if not self.intent_id or not self.request_id:
            return False
        return not self.expires_at_ms or current < self.expires_at_ms

    def digest(self) -> str:
        return coordination_stable_digest(self, length=24)


@dataclass(frozen=True)
class CoordinationWindow:
    window_id: str
    started_at_ms: int
    closes_at_ms: int
    intents: tuple[CoordinationIntent, ...] = ()

    @classmethod
    def open(cls, intents: tuple[CoordinationIntent, ...] | list[CoordinationIntent],
             *,
             now_ms_value: int | None = None,
             window_ms: int = 200) -> "CoordinationWindow":
        current = coordination_now_ms() if now_ms_value is None else int(now_ms_value)
        intent_tuple = tuple(intents)
        return cls(
            window_id=coordination_stable_digest({
                "schema": "ndnsf-coordination-window-v1",
                "startedAtMs": current,
                "intentDigests": [intent.digest() for intent in intent_tuple],
            }, length=20),
            started_at_ms=current,
            closes_at_ms=current + max(0, int(window_ms)),
            intents=intent_tuple,
        )


@dataclass(frozen=True)
class CoordinationSuggestion:
    suggestion_id: str = ""
    intent_id: str = ""
    request_id: str = ""
    service_name: str = ""
    coordinator_name: str = ""
    window_id: str = ""
    created_at_ms: int = field(default_factory=coordination_now_ms)
    expires_at_ms: int = 0
    proof: str = ""
    payload_schema: str = ""
    payload: dict[str, Any] = field(default_factory=dict)
    score_breakdown: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "CoordinationSuggestion":
        return cls(
            suggestion_id=str(payload.get("suggestionId", payload.get("suggestion_id", ""))),
            intent_id=str(payload.get("intentId", payload.get("intent_id", ""))),
            request_id=str(payload.get("requestId", payload.get("request_id", ""))),
            service_name=str(payload.get("serviceName", payload.get("service_name", ""))),
            coordinator_name=str(payload.get("coordinatorName", payload.get("coordinator_name", ""))),
            window_id=str(payload.get("windowId", payload.get("window_id", ""))),
            created_at_ms=int(payload.get("createdAtMs", payload.get("created_at_ms", coordination_now_ms())) or 0),
            expires_at_ms=int(payload.get("expiresAtMs", payload.get("expires_at_ms", 0)) or 0),
            proof=str(payload.get("proof", "")),
            payload_schema=str(payload.get("payloadSchema", payload.get("payload_schema", ""))),
            payload=dict(payload.get("payload", {}) or {}),
            score_breakdown=dict(payload.get("scoreBreakdown", payload.get("score_breakdown", {})) or {}),
        )

    def is_fresh(self, *, now_ms_value: int | None = None) -> bool:
        current = coordination_now_ms() if now_ms_value is None else int(now_ms_value)
        return bool(self.suggestion_id and (not self.expires_at_ms or current < self.expires_at_ms))

    def proof_payload(self) -> dict[str, Any]:
        payload = coordination_to_plain(self)
        payload.pop("proof", None)
        return payload


def coordination_suggestion_proof(suggestion: CoordinationSuggestion, *,
                                  secret: str = "") -> str:
    return coordination_stable_digest({
        "schema": "ndnsf-coordination-proof-v1",
        "secret": secret,
        "suggestion": suggestion.proof_payload(),
    }, length=32)


def verify_coordination_suggestion(suggestion: CoordinationSuggestion, *,
                                   secret: str = "",
                                   now_ms_value: int | None = None) -> tuple[bool, str]:
    if not suggestion.is_fresh(now_ms_value=now_ms_value):
        return False, "STALE_SUGGESTION"
    if secret or suggestion.proof:
        expected = coordination_suggestion_proof(replace(suggestion, proof=""), secret=secret)
        if suggestion.proof != expected:
            return False, "COORDINATION_PROOF_INVALID"
    return True, "OK"


COORDINATION_ADVISORY_SERVICE = "/NDNSF/Coordination/Advisory"


@dataclass(frozen=True)
class CoordinationRequest:
    """A service-neutral coordination request carried as an NDNSF payload.

    The request is intentionally an application payload, not a new NDNSF wire
    protocol. Freshness is represented by each intent's nonce and expiry fields;
    the normal NDNSF service path still provides permissions, tokens, and
    request/response delivery.
    """

    intents: tuple[CoordinationIntent, ...]
    request_schema: str = "ndnsf-coordination-request-v1"
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "CoordinationRequest":
        return cls(
            request_schema=str(payload.get("requestSchema", payload.get("request_schema",
                                                                         "ndnsf-coordination-request-v1"))),
            intents=tuple(
                item if isinstance(item, CoordinationIntent) else CoordinationIntent.from_dict(dict(item))
                for item in payload.get("intents", [])
            ),
            metadata=dict(payload.get("metadata", {}) or {}),
        )


@dataclass(frozen=True)
class CoordinationResponse:
    """A service-neutral coordination response carried as an NDNSF payload.

    Suggestions are advisory hints. Callers must still verify freshness/proof
    and re-check local provider candidates or leases before using them.
    """

    suggestions: tuple[CoordinationSuggestion, ...]
    response_schema: str = "ndnsf-coordination-response-v1"
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "CoordinationResponse":
        return cls(
            response_schema=str(payload.get("responseSchema", payload.get("response_schema",
                                                                           "ndnsf-coordination-response-v1"))),
            suggestions=tuple(
                item if isinstance(item, CoordinationSuggestion) else CoordinationSuggestion.from_dict(dict(item))
                for item in payload.get("suggestions", [])
            ),
            metadata=dict(payload.get("metadata", {}) or {}),
        )


def encode_coordination_request(request: CoordinationRequest | Iterable[CoordinationIntent]) -> bytes:
    if isinstance(request, CoordinationRequest):
        payload = request
    else:
        payload = CoordinationRequest(tuple(request))
    return (coordination_stable_json(payload) + "\n").encode("utf-8")


def decode_coordination_request(payload: bytes | str) -> CoordinationRequest:
    text = payload.decode("utf-8") if isinstance(payload, bytes) else str(payload)
    return CoordinationRequest.from_dict(json.loads(text))


def encode_coordination_response(response: CoordinationResponse | Iterable[CoordinationSuggestion]) -> bytes:
    if isinstance(response, CoordinationResponse):
        payload = response
    else:
        payload = CoordinationResponse(tuple(response))
    return (coordination_stable_json(payload) + "\n").encode("utf-8")


def decode_coordination_response(payload: bytes | str) -> CoordinationResponse:
    text = payload.decode("utf-8") if isinstance(payload, bytes) else str(payload)
    return CoordinationResponse.from_dict(json.loads(text))


class CoordinationServiceProvider:
    """Register a generic coordination service on an NDNSF ServiceProvider.

    The wrapper only adapts JSON coordination payloads to the existing dynamic
    service API. It deliberately does not authorize execution; providers and
    users continue to rely on NDNSF permissions, UserToken/ProviderToken, and
    application-level suggestion validation.
    """

    def __init__(self,
                 provider: Any,
                 handler: Callable[[CoordinationRequest], CoordinationResponse | Iterable[CoordinationSuggestion]],
                 *,
                 service_name: str = COORDINATION_ADVISORY_SERVICE) -> None:
        self.provider = provider
        self.handler = handler
        self.service_name = service_name

    def register(self) -> None:
        def handle(payload: bytes) -> bytes:
            request = decode_coordination_request(payload)
            result = self.handler(request)
            response = result if isinstance(result, CoordinationResponse) else CoordinationResponse(tuple(result))
            return encode_coordination_response(response)

        self.provider.add_handler(self.service_name, handle)


class CoordinationServiceClient:
    """Call a generic coordination service through an NDNSF ServiceUser.

    The client sends a normal service request and decodes the response payload.
    It raises on service failure so the caller can fall back to pure user-side
    planning without treating missing advice as an execution failure.
    """

    def __init__(self, user: Any, *,
                 service_name: str = COORDINATION_ADVISORY_SERVICE,
                 ack_timeout_ms: int = 500,
                 timeout_ms: int = 5000) -> None:
        self.user = user
        self.service_name = service_name
        self.ack_timeout_ms = int(ack_timeout_ms)
        self.timeout_ms = int(timeout_ms)

    def request(self, intents: Iterable[CoordinationIntent],
                *,
                metadata: dict[str, Any] | None = None) -> CoordinationResponse:
        payload = encode_coordination_request(CoordinationRequest(
            tuple(intents),
            metadata=dict(metadata or {}),
        ))
        response = self.user.request_service(
            self.service_name,
            payload,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
        )
        if not response.status:
            raise RuntimeError(response.error or "coordination service request failed")
        return decode_coordination_response(response.payload)
