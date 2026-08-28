"""Authenticated multi-turn conversation state contracts.

This module is deliberately independent from the network transport.  It owns
the user-side conversation/checkpoint transaction and the Provider-local
residency ledger, while request-local decode state remains owned by the
request/attempt runtime.  No serialized object contains tensors, pointers,
paths, or Provider selection input.
"""

from __future__ import annotations

import base64
from contextlib import ExitStack
from dataclasses import dataclass, field, replace
from enum import Enum
import hashlib
import hmac
import json
from concurrent.futures import (
    Future,
    ThreadPoolExecutor,
    TimeoutError as FutureTimeoutError,
)
from threading import RLock
import time
import uuid
from typing import Any, Callable, Iterable, Mapping


SCHEMA_CHECKPOINT = "ndnsf-di-conversation-checkpoint-v1"
SCHEMA_TRANSCRIPT = "ndnsf-di-conversation-transcript-v1"
SCHEMA_RECEIPT = "ndnsf-di-provider-conversation-receipt-v1"
SCHEMA_STATE_REFERENCE = "ndnsf-di-conversation-state-reference-v1"
MAX_CHECKPOINT_FINALIZE_TOKENS = 32
DEFAULT_RETENTION_MS = 300_000


class ConversationError(RuntimeError):
    """Base class for fail-closed continuation errors."""


class ConversationCheckpointInvalid(ConversationError):
    pass


class ConversationStateUnavailable(ConversationError):
    pass


class ConversationStateConflict(ConversationError):
    pass


class ConversationInputMode(str, Enum):
    FULL_CONTEXT = "full-context"
    APPEND_DELTA = "append-delta"


class ResidencyTier(str, Enum):
    GPU_RESIDENT = "GPU_RESIDENT"
    HOST_RESIDENT = "HOST_RESIDENT"
    EVICTED = "EVICTED"


class StateLifecycle(str, Enum):
    IDLE = "IDLE"
    PREFETCHING = "PREFETCHING"
    PINNED = "PINNED"
    COMMITTING = "COMMITTING"


def _digest(value: Any) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"),
                     ensure_ascii=False).encode("utf-8")
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def _bytes_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(bytes(value)).hexdigest()


def _require_digest(value: str, name: str) -> str:
    value = str(value)
    if (len(value) != 71 or not value.startswith("sha256:")
            or any(ch not in "0123456789abcdef" for ch in value[7:])):
        raise ValueError(f"{name} must be a canonical sha256 digest")
    return value


def _require_conversation_id(value: str) -> str:
    value = str(value or "")
    # Application identities are opaque, but a short predictable value is not
    # a safe conversation namespace.  UUID/32-hex IDs satisfy the V1 minimum.
    if len(value) < 16 or "/" in value or "\\" in value or value in {".", ".."}:
        raise ValueError("conversation_id must be a high-entropy opaque ID")
    return value


def _prefix_digest(token_ids: Iterable[int]) -> str:
    values = tuple(int(item) for item in token_ids)
    return _digest({"schema": "ndnsf-di-prefix-v1", "tokenIds": list(values)})


def _canonical_payload(value: Mapping[str, Any]) -> bytes:
    return json.dumps(dict(value), sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")


@dataclass(frozen=True)
class ConversationContinuation:
    """Additive per-turn option; absent options preserve full-context calls."""

    conversation_id: str
    mode: ConversationInputMode = ConversationInputMode.FULL_CONTEXT
    parent_checkpoint: bytes | None = None
    expected_parent_context_epoch: int | None = None
    turn_input_digest: str = ""
    allow_full_prefill_fallback: bool = False
    fallback_full_input: bytes | None = None
    request_contract_digest: str = ""

    def __post_init__(self) -> None:
        _require_conversation_id(self.conversation_id)
        mode = ConversationInputMode(self.mode)
        object.__setattr__(self, "mode", mode)
        if self.turn_input_digest:
            _require_digest(self.turn_input_digest, "turn_input_digest")
        if self.request_contract_digest:
            _require_digest(self.request_contract_digest,
                            "request_contract_digest")
        if self.parent_checkpoint is not None:
            object.__setattr__(self, "parent_checkpoint",
                               bytes(self.parent_checkpoint))
        if self.fallback_full_input is not None:
            object.__setattr__(self, "fallback_full_input",
                               bytes(self.fallback_full_input))
        if mode is ConversationInputMode.FULL_CONTEXT:
            if self.parent_checkpoint is not None or self.expected_parent_context_epoch is not None:
                raise ValueError("FULL_CONTEXT must not carry a parent checkpoint")
            if self.allow_full_prefill_fallback and self.fallback_full_input is None:
                raise ValueError("full-context fallback requires authenticated input")
        else:
            if self.expected_parent_context_epoch is None or self.expected_parent_context_epoch < 1:
                raise ValueError("APPEND_DELTA requires a positive parent epoch")
            if self.parent_checkpoint is None:
                raise ValueError("APPEND_DELTA requires a parent checkpoint")
            if self.allow_full_prefill_fallback and self.fallback_full_input is None:
                raise ValueError("full-context fallback requires authenticated input")

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": "ndnsf-di-conversation-continuation-v1",
            "conversationId": self.conversation_id,
            "mode": self.mode.value,
            "parentCheckpoint": (base64.b64encode(self.parent_checkpoint).decode()
                                  if self.parent_checkpoint is not None else None),
            "expectedParentContextEpoch": self.expected_parent_context_epoch,
            "turnInputDigest": self.turn_input_digest,
            "allowFullPrefillFallback": self.allow_full_prefill_fallback,
            "fallbackFullInputDigest": (_bytes_digest(self.fallback_full_input)
                                         if self.fallback_full_input is not None else None),
            "requestContractDigest": self.request_contract_digest,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ConversationContinuation":
        """Decode the canonical metadata carried in a fresh Request.

        The wire form deliberately carries only an opaque checkpoint and a
        digest for the optional fallback input.  It never carries Provider
        state, so a Provider can validate the continuation contract without
        confusing transcript metadata with a usable KV/recurrent state.
        """
        if not isinstance(value, Mapping):
            raise ConversationCheckpointInvalid(
                "conversation continuation must be an object")
        expected = {
            "schema", "conversationId", "mode", "parentCheckpoint",
            "expectedParentContextEpoch", "turnInputDigest",
            "allowFullPrefillFallback", "fallbackFullInputDigest",
            "requestContractDigest",
        }
        if set(value) != expected:
            raise ConversationCheckpointInvalid(
                "conversation continuation field set mismatch")
        if value.get("schema") != "ndnsf-di-conversation-continuation-v1":
            raise ConversationCheckpointInvalid(
                "conversation continuation schema mismatch")
        encoded = value.get("parentCheckpoint")
        parent = None
        if encoded is not None:
            if not isinstance(encoded, str):
                raise ConversationCheckpointInvalid(
                    "conversation parent checkpoint must be base64")
            try:
                parent = base64.b64decode(encoded.encode("ascii"), validate=True)
            except (UnicodeEncodeError, ValueError) as exc:
                raise ConversationCheckpointInvalid(
                    "conversation parent checkpoint is malformed") from exc
        fallback_digest = value.get("fallbackFullInputDigest")
        if fallback_digest is not None:
            _require_digest(str(fallback_digest), "fallback_full_input_digest")
        try:
            return cls(
                conversation_id=str(value["conversationId"]),
                mode=ConversationInputMode(value["mode"]),
                parent_checkpoint=parent,
                expected_parent_context_epoch=(
                    None if value["expectedParentContextEpoch"] is None
                    else int(value["expectedParentContextEpoch"])),
                turn_input_digest=str(value["turnInputDigest"] or ""),
                allow_full_prefill_fallback=bool(
                    value["allowFullPrefillFallback"]),
                # The fallback bytes are intentionally not on this wire form.
                fallback_full_input=None,
                request_contract_digest=str(
                    value["requestContractDigest"] or ""),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ConversationCheckpointInvalid(
                "conversation continuation metadata is invalid") from exc

    def request_contract(self, *, input_digest: str | None = None) -> str:
        digest = input_digest or self.turn_input_digest
        if not digest:
            raise ValueError("turn input digest is required")
        _require_digest(digest, "turn_input_digest")
        return _digest({
            "conversationId": self.conversation_id,
            "mode": self.mode.value,
            "expectedParentContextEpoch": self.expected_parent_context_epoch,
            "parentCheckpointDigest": (_bytes_digest(self.parent_checkpoint)
                                         if self.parent_checkpoint is not None else ""),
            "turnInputDigest": digest,
            "allowFullPrefillFallback": self.allow_full_prefill_fallback,
            "fallbackFullInputDigest": (_bytes_digest(self.fallback_full_input)
                                         if self.fallback_full_input is not None else ""),
        })


@dataclass(frozen=True)
class ProviderConversationStateReceiptV1:
    conversation_id: str
    parent_context_epoch: int
    successor_context_epoch: int
    origin_request_id: str
    origin_generation_id: str
    service_name: str
    requester_identity: str
    security_domain_digest: str
    model_digest: str
    graph_semantic_digest: str
    adapter_digest: str
    role_name: str
    role_split_digest: str
    layout_digest: str
    plan_role_map_digest: str
    provider_identity: str
    provider_boot_id: str
    cache_epoch: int
    prefix_digest: str
    prefix_token_count: int
    position_digest: str
    state_schema_digest: str
    state_component_digests: tuple[str, ...]
    expires_at_ms: int
    receipt_digest: str = ""
    signature: str = ""

    def __post_init__(self) -> None:
        _require_conversation_id(self.conversation_id)
        if (self.parent_context_epoch < 0
                or self.successor_context_epoch != self.parent_context_epoch + 1
                or not self.origin_request_id or not self.origin_generation_id
                or not self.service_name.startswith("/")
                or not self.requester_identity or not self.provider_identity
                or not self.provider_boot_id or self.cache_epoch < 0
                or self.prefix_token_count < 0 or self.expires_at_ms <= 0):
            raise ValueError("invalid Provider conversation receipt")
        for name in (
                "security_domain_digest", "model_digest", "graph_semantic_digest",
                "adapter_digest", "role_split_digest", "layout_digest",
                "plan_role_map_digest", "prefix_digest", "position_digest",
                "state_schema_digest"):
            _require_digest(getattr(self, name), name)
        components = tuple(str(item) for item in self.state_component_digests)
        if not components:
            raise ValueError("receipt must identify every state component")
        for digest in components:
            _require_digest(digest, "state_component_digest")
        object.__setattr__(self, "state_component_digests", components)
        expected = self.computed_digest()
        if self.receipt_digest and self.receipt_digest != expected:
            raise ValueError("Provider receipt digest mismatch")
        object.__setattr__(self, "receipt_digest", expected)

    def _unsigned_dict(self) -> dict[str, Any]:
        return {
            "schema": SCHEMA_RECEIPT,
            "conversationId": self.conversation_id,
            "parentContextEpoch": self.parent_context_epoch,
            "successorContextEpoch": self.successor_context_epoch,
            "originRequestId": self.origin_request_id,
            "originGenerationId": self.origin_generation_id,
            "serviceName": self.service_name,
            "requesterIdentity": self.requester_identity,
            "securityDomainDigest": self.security_domain_digest,
            "modelDigest": self.model_digest,
            "graphSemanticDigest": self.graph_semantic_digest,
            "adapterDigest": self.adapter_digest,
            "roleName": self.role_name,
            "roleSplitDigest": self.role_split_digest,
            "layoutDigest": self.layout_digest,
            "planRoleMapDigest": self.plan_role_map_digest,
            "providerIdentity": self.provider_identity,
            "providerBootId": self.provider_boot_id,
            "cacheEpoch": self.cache_epoch,
            "prefixDigest": self.prefix_digest,
            "prefixTokenCount": self.prefix_token_count,
            "positionDigest": self.position_digest,
            "stateSchemaDigest": self.state_schema_digest,
            "stateComponentDigests": list(self.state_component_digests),
            "expiresAtMs": self.expires_at_ms,
        }

    def computed_digest(self) -> str:
        return _digest(self._unsigned_dict())

    def sign(self, key: bytes) -> "ProviderConversationStateReceiptV1":
        signature = hmac.new(bytes(key), self.computed_digest().encode(),
                             hashlib.sha256).hexdigest()
        return replace(self, signature=signature)

    def verify(self, key: bytes) -> bool:
        expected = hmac.new(bytes(key), self.computed_digest().encode(),
                            hashlib.sha256).hexdigest()
        return bool(self.signature) and hmac.compare_digest(self.signature, expected)

    def to_dict(self) -> dict[str, Any]:
        return {**self._unsigned_dict(), "receiptDigest": self.receipt_digest,
                "signature": self.signature}

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ProviderConversationStateReceiptV1":
        """Decode one canonical Provider receipt from a verified wire payload.

        The surrounding NDN Data signature is verified by Core/User before
        this parser is called.  The receipt digest is still recomputed by the
        constructor so a signed envelope cannot smuggle altered metadata.
        """
        if not isinstance(value, Mapping):
            raise ConversationCheckpointInvalid("Provider receipt must be an object")
        expected = set(cls._wire_keys())
        if set(value) != expected:
            raise ConversationCheckpointInvalid("Provider receipt field set mismatch")
        try:
            return cls(
                conversation_id=str(value["conversationId"]),
                parent_context_epoch=int(value["parentContextEpoch"]),
                successor_context_epoch=int(value["successorContextEpoch"]),
                origin_request_id=str(value["originRequestId"]),
                origin_generation_id=str(value["originGenerationId"]),
                service_name=str(value["serviceName"]),
                requester_identity=str(value["requesterIdentity"]),
                security_domain_digest=str(value["securityDomainDigest"]),
                model_digest=str(value["modelDigest"]),
                graph_semantic_digest=str(value["graphSemanticDigest"]),
                adapter_digest=str(value["adapterDigest"]),
                role_name=str(value["roleName"]),
                role_split_digest=str(value["roleSplitDigest"]),
                layout_digest=str(value["layoutDigest"]),
                plan_role_map_digest=str(value["planRoleMapDigest"]),
                provider_identity=str(value["providerIdentity"]),
                provider_boot_id=str(value["providerBootId"]),
                cache_epoch=int(value["cacheEpoch"]),
                prefix_digest=str(value["prefixDigest"]),
                prefix_token_count=int(value["prefixTokenCount"]),
                position_digest=str(value["positionDigest"]),
                state_schema_digest=str(value["stateSchemaDigest"]),
                state_component_digests=tuple(
                    str(item) for item in value["stateComponentDigests"]),
                expires_at_ms=int(value["expiresAtMs"]),
                receipt_digest=str(value["receiptDigest"]),
                signature=str(value["signature"]),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ConversationCheckpointInvalid(
                "Provider receipt is malformed") from exc

    @staticmethod
    def _wire_keys() -> tuple[str, ...]:
        return (
            "schema", "conversationId", "parentContextEpoch",
            "successorContextEpoch", "originRequestId", "originGenerationId",
            "serviceName", "requesterIdentity", "securityDomainDigest",
            "modelDigest", "graphSemanticDigest", "adapterDigest", "roleName",
            "roleSplitDigest", "layoutDigest", "planRoleMapDigest",
            "providerIdentity", "providerBootId", "cacheEpoch", "prefixDigest",
            "prefixTokenCount", "positionDigest", "stateSchemaDigest",
            "stateComponentDigests", "expiresAtMs", "receiptDigest", "signature",
        )


@dataclass(frozen=True)
class ConversationStateReferenceV1:
    """Compact Selection binding for one Provider-local parent state.

    The aggregate checkpoint remains User-owned and the complete signed role
    receipt remains requester-encrypted. A per-Provider Selection carries only
    the commitments needed to resolve one retained local entry; it never
    carries tensors, paths, or another Provider's receipt.
    """

    conversation_id: str
    context_epoch: int
    service_name: str
    plan_role_map_digest: str
    checkpoint_digest: str
    role_name: str
    role_receipt_digest: str
    expires_at_ms: int
    schema: str = SCHEMA_STATE_REFERENCE
    version: int = 1

    def __post_init__(self) -> None:
        _require_conversation_id(self.conversation_id)
        if (self.version != 1 or self.schema != SCHEMA_STATE_REFERENCE
                or self.context_epoch <= 0
                or not self.service_name.startswith("/")
                or not self.role_name
                or self.expires_at_ms <= 0):
            raise ValueError("invalid conversation state reference")
        for name in (
                "plan_role_map_digest", "checkpoint_digest",
                "role_receipt_digest"):
            _require_digest(getattr(self, name), name)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "version": self.version,
            "conversationId": self.conversation_id,
            "contextEpoch": self.context_epoch,
            "serviceName": self.service_name,
            "planRoleMapDigest": self.plan_role_map_digest,
            "checkpointDigest": self.checkpoint_digest,
            "roleName": self.role_name,
            "roleReceiptDigest": self.role_receipt_digest,
            "expiresAtMs": self.expires_at_ms,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ConversationStateReferenceV1":
        if not isinstance(value, Mapping):
            raise ConversationCheckpointInvalid(
                "conversation state reference field set mismatch")
        camel_keys = {
            "schema", "version", "conversationId", "contextEpoch",
            "serviceName", "planRoleMapDigest", "checkpointDigest",
            "roleName", "roleReceiptDigest", "expiresAtMs"}
        snake_keys = {
            "schema", "version", "conversation_id", "context_epoch",
            "service_name", "plan_role_map_digest", "checkpoint_digest",
            "role_name", "role_receipt_digest", "expires_at_ms"}
        keys = set(value)
        if keys == camel_keys:
            normalized = {
                "conversation_id": value["conversationId"],
                "context_epoch": value["contextEpoch"],
                "service_name": value["serviceName"],
                "plan_role_map_digest": value["planRoleMapDigest"],
                "checkpoint_digest": value["checkpointDigest"],
                "role_name": value["roleName"],
                "role_receipt_digest": value["roleReceiptDigest"],
                "expires_at_ms": value["expiresAtMs"],
            }
        elif keys == snake_keys:
            normalized = dict(value)
        else:
            raise ConversationCheckpointInvalid(
                "conversation state reference field set mismatch")
        try:
            return cls(
                conversation_id=str(normalized["conversation_id"]),
                context_epoch=int(normalized["context_epoch"]),
                service_name=str(normalized["service_name"]),
                plan_role_map_digest=str(normalized["plan_role_map_digest"]),
                checkpoint_digest=str(normalized["checkpoint_digest"]),
                role_name=str(normalized["role_name"]),
                role_receipt_digest=str(normalized["role_receipt_digest"]),
                expires_at_ms=int(normalized["expires_at_ms"]),
                schema=str(value["schema"]),
                version=int(value["version"]),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ConversationCheckpointInvalid(
                "conversation state reference is invalid") from exc


@dataclass(frozen=True)
class ConversationTurnBindingV1:
    """Selection commitment used to create one successor conversation epoch.

    It is present for both the first FULL_CONTEXT turn and later APPEND_DELTA
    turns. It contains no state or Provider-controlled identity; the Provider
    combines it only with the exact decode-state identity finalized by its
    native epoch coordinator.
    """

    conversation_id: str
    parent_context_epoch: int
    successor_context_epoch: int
    service_name: str
    plan_role_map_digest: str
    request_contract_digest: str
    retention_deadline_ms: int
    parent_checkpoint_digest: str = ""
    schema: str = "ndnsf-di-conversation-turn-binding-v1"
    version: int = 1

    def __post_init__(self) -> None:
        _require_conversation_id(self.conversation_id)
        if (self.parent_context_epoch < 0
                or self.successor_context_epoch
                != self.parent_context_epoch + 1
                or not self.service_name.startswith("/")
                or self.retention_deadline_ms <= 0
                or self.schema != "ndnsf-di-conversation-turn-binding-v1"
                or self.version != 1):
            raise ValueError("invalid conversation turn binding")
        _require_digest(self.plan_role_map_digest, "plan_role_map_digest")
        _require_digest(self.request_contract_digest, "request_contract_digest")
        if self.parent_context_epoch == 0:
            if self.parent_checkpoint_digest:
                raise ValueError(
                    "initial conversation turn must not bind a parent checkpoint")
        else:
            _require_digest(
                self.parent_checkpoint_digest, "parent_checkpoint_digest")

    def to_dict(self) -> dict[str, Any]:
        """Return the canonical wire representation used in a request spec.

        The binding is serialized into the signed execution-plan metadata and
        later parsed by each Provider.  Keeping this beside ``from_dict``
        prevents callers from relying on dataclass field names (which would
        silently produce snake_case and fail the exact-field contract).
        """
        return {
            "schema": self.schema,
            "version": self.version,
            "conversationId": self.conversation_id,
            "parentContextEpoch": self.parent_context_epoch,
            "successorContextEpoch": self.successor_context_epoch,
            "serviceName": self.service_name,
            "planRoleMapDigest": self.plan_role_map_digest,
            "requestContractDigest": self.request_contract_digest,
            "retentionDeadlineMs": self.retention_deadline_ms,
            "parentCheckpointDigest": self.parent_checkpoint_digest,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ConversationTurnBindingV1":
        if not isinstance(value, Mapping):
            raise ConversationCheckpointInvalid(
                "conversation turn binding must be an object")
        aliases = {
            "conversationId": "conversation_id",
            "parentContextEpoch": "parent_context_epoch",
            "successorContextEpoch": "successor_context_epoch",
            "serviceName": "service_name",
            "planRoleMapDigest": "plan_role_map_digest",
            "requestContractDigest": "request_contract_digest",
            "retentionDeadlineMs": "retention_deadline_ms",
            "parentCheckpointDigest": "parent_checkpoint_digest",
        }
        normalized = {aliases.get(str(key), str(key)): item
                      for key, item in value.items()}
        expected = {
            "conversation_id", "parent_context_epoch",
            "successor_context_epoch", "service_name",
            "plan_role_map_digest", "request_contract_digest",
            "retention_deadline_ms", "parent_checkpoint_digest",
            "schema", "version",
        }
        if set(normalized) != expected:
            raise ConversationCheckpointInvalid(
                "conversation turn binding field set mismatch")
        try:
            return cls(
                conversation_id=str(normalized["conversation_id"]),
                parent_context_epoch=int(normalized["parent_context_epoch"]),
                successor_context_epoch=int(
                    normalized["successor_context_epoch"]),
                service_name=str(normalized["service_name"]),
                plan_role_map_digest=str(
                    normalized["plan_role_map_digest"]),
                request_contract_digest=str(
                    normalized["request_contract_digest"]),
                retention_deadline_ms=int(
                    normalized["retention_deadline_ms"]),
                parent_checkpoint_digest=str(
                    normalized["parent_checkpoint_digest"]),
                schema=str(normalized["schema"]),
                version=int(normalized["version"]),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ConversationCheckpointInvalid(
                "conversation turn binding is malformed") from exc


@dataclass(frozen=True)
class ConversationCheckpointV1:
    conversation_id: str
    parent_context_epoch: int
    context_epoch: int
    service_name: str
    requester_identity: str
    security_domain_digest: str
    model_contract_digest: str
    plan_role_map_digest: str
    logical_prefix_digest: str
    prefix_token_count: int
    role_receipt_digests: Mapping[str, str]
    issued_at_ms: int
    expires_at_ms: int
    checkpoint_digest: str = ""
    signature: str = ""
    version: int = 1

    def __post_init__(self) -> None:
        _require_conversation_id(self.conversation_id)
        if (self.version != 1 or self.parent_context_epoch < 0
                or self.context_epoch != self.parent_context_epoch + 1
                or not self.service_name.startswith("/")
                or not self.requester_identity or self.prefix_token_count < 0
                or self.issued_at_ms <= 0 or self.expires_at_ms <= self.issued_at_ms):
            raise ValueError("invalid conversation checkpoint")
        for name in (
                "security_domain_digest", "model_contract_digest",
                "plan_role_map_digest", "logical_prefix_digest"):
            _require_digest(getattr(self, name), name)
        receipts = {str(role): str(digest)
                    for role, digest in dict(self.role_receipt_digests).items()}
        if not receipts or any(not role.startswith("/") for role in receipts):
            raise ValueError("checkpoint requires a complete role receipt set")
        for digest in receipts.values():
            _require_digest(digest, "role_receipt_digest")
        object.__setattr__(self, "role_receipt_digests", dict(sorted(receipts.items())))
        expected = self.computed_digest()
        if self.checkpoint_digest and self.checkpoint_digest != expected:
            raise ValueError("conversation checkpoint digest mismatch")
        object.__setattr__(self, "checkpoint_digest", expected)

    def _unsigned_dict(self) -> dict[str, Any]:
        return {
            "schema": SCHEMA_CHECKPOINT,
            "version": self.version,
            "conversationId": self.conversation_id,
            "parentContextEpoch": self.parent_context_epoch,
            "contextEpoch": self.context_epoch,
            "serviceName": self.service_name,
            "requesterIdentity": self.requester_identity,
            "securityDomainDigest": self.security_domain_digest,
            "modelContractDigest": self.model_contract_digest,
            "planRoleMapDigest": self.plan_role_map_digest,
            "logicalPrefixDigest": self.logical_prefix_digest,
            "prefixTokenCount": self.prefix_token_count,
            "roleReceiptDigests": dict(self.role_receipt_digests),
            "issuedAtMs": self.issued_at_ms,
            "expiresAtMs": self.expires_at_ms,
        }

    def computed_digest(self) -> str:
        return _digest(self._unsigned_dict())

    def sign(self, key: bytes) -> "ConversationCheckpointV1":
        signature = hmac.new(bytes(key), self.computed_digest().encode(),
                             hashlib.sha256).hexdigest()
        return replace(self, signature=signature)

    def verify(self, key: bytes) -> bool:
        expected = hmac.new(bytes(key), self.computed_digest().encode(),
                            hashlib.sha256).hexdigest()
        return bool(self.signature) and hmac.compare_digest(self.signature, expected)

    def to_bytes(self) -> bytes:
        return _canonical_payload({**self._unsigned_dict(),
                                   "checkpointDigest": self.checkpoint_digest,
                                   "signature": self.signature})

    @classmethod
    def from_bytes(cls, payload: bytes) -> "ConversationCheckpointV1":
        try:
            value = json.loads(bytes(payload).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ConversationCheckpointInvalid("malformed checkpoint") from exc
        expected = set(cls._wire_keys())
        if not isinstance(value, dict) or set(value) != expected:
            raise ConversationCheckpointInvalid("checkpoint field set mismatch")
        if bytes(payload) != _canonical_payload(value):
            raise ConversationCheckpointInvalid("checkpoint is not canonical")
        if value.get("schema") != SCHEMA_CHECKPOINT:
            raise ConversationCheckpointInvalid("checkpoint schema mismatch")
        return cls(
            conversation_id=str(value["conversationId"]),
            parent_context_epoch=int(value["parentContextEpoch"]),
            context_epoch=int(value["contextEpoch"]),
            service_name=str(value["serviceName"]),
            requester_identity=str(value["requesterIdentity"]),
            security_domain_digest=str(value["securityDomainDigest"]),
            model_contract_digest=str(value["modelContractDigest"]),
            plan_role_map_digest=str(value["planRoleMapDigest"]),
            logical_prefix_digest=str(value["logicalPrefixDigest"]),
            prefix_token_count=int(value["prefixTokenCount"]),
            role_receipt_digests=dict(value["roleReceiptDigests"]),
            issued_at_ms=int(value["issuedAtMs"]),
            expires_at_ms=int(value["expiresAtMs"]),
            checkpoint_digest=str(value["checkpointDigest"]),
            signature=str(value["signature"]),
            version=int(value["version"]),
        )

    @staticmethod
    def _wire_keys() -> tuple[str, ...]:
        return (
            "schema", "version", "conversationId", "parentContextEpoch",
            "contextEpoch", "serviceName", "requesterIdentity",
            "securityDomainDigest", "modelContractDigest", "planRoleMapDigest",
            "logicalPrefixDigest", "prefixTokenCount", "roleReceiptDigests",
            "issuedAtMs", "expiresAtMs", "checkpointDigest", "signature",
        )


@dataclass(frozen=True)
class ConversationTranscriptRecordV1:
    conversation_id: str
    context_epoch: int
    requester_identity: str
    service_name: str
    security_domain_digest: str
    application_messages: bytes
    tokenizer_digest: str
    chat_template_digest: str
    canonical_token_ids: tuple[int, ...]
    prefix_digest: str
    prefix_token_count: int
    provider_role_receipts: tuple[bytes, ...]
    checkpoint_digest: str
    plan_role_map_digest: str
    created_at_ms: int
    expires_at_ms: int

    def __post_init__(self) -> None:
        _require_conversation_id(self.conversation_id)
        if (self.context_epoch < 1 or not self.requester_identity
                or not self.service_name.startswith("/")
                or not isinstance(self.application_messages, bytes)
                or not self.canonical_token_ids or self.prefix_token_count != len(self.canonical_token_ids)
                or self.created_at_ms <= 0 or self.expires_at_ms <= self.created_at_ms):
            raise ValueError("invalid conversation transcript record")
        for name in (
                "security_domain_digest", "tokenizer_digest", "chat_template_digest",
                "prefix_digest", "checkpoint_digest", "plan_role_map_digest"):
            _require_digest(getattr(self, name), name)
        if self.prefix_digest != _prefix_digest(self.canonical_token_ids):
            raise ValueError("transcript prefix digest mismatch")
        if any(int(token) < 0 for token in self.canonical_token_ids):
            raise ValueError("canonical token IDs must be non-negative")
        object.__setattr__(self, "canonical_token_ids",
                           tuple(int(token) for token in self.canonical_token_ids))
        object.__setattr__(self, "provider_role_receipts",
                           tuple(bytes(item) for item in self.provider_role_receipts))

    def to_dict(self) -> dict[str, Any]:
        # This representation is only for encrypted RuntimeJournal payloads;
        # callers must not print it or place it on NDN edges.
        return {
            "schema": SCHEMA_TRANSCRIPT,
            "conversationId": self.conversation_id,
            "contextEpoch": self.context_epoch,
            "requesterIdentity": self.requester_identity,
            "serviceName": self.service_name,
            "securityDomainDigest": self.security_domain_digest,
            "applicationMessages": base64.b64encode(self.application_messages).decode(),
            "tokenizerDigest": self.tokenizer_digest,
            "chatTemplateDigest": self.chat_template_digest,
            "canonicalTokenIds": list(self.canonical_token_ids),
            "prefixDigest": self.prefix_digest,
            "prefixTokenCount": self.prefix_token_count,
            "providerRoleReceipts": [base64.b64encode(item).decode()
                                     for item in self.provider_role_receipts],
            "checkpointDigest": self.checkpoint_digest,
            "planRoleMapDigest": self.plan_role_map_digest,
            "createdAtMs": self.created_at_ms,
            "expiresAtMs": self.expires_at_ms,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ConversationTranscriptRecordV1":
        """Rebuild one transcript only from an authenticated journal payload."""
        required = {
            "schema", "conversationId", "contextEpoch", "requesterIdentity",
            "serviceName", "securityDomainDigest", "applicationMessages",
            "tokenizerDigest", "chatTemplateDigest", "canonicalTokenIds",
            "prefixDigest", "prefixTokenCount", "providerRoleReceipts",
            "checkpointDigest", "planRoleMapDigest", "createdAtMs",
            "expiresAtMs",
        }
        if not isinstance(value, Mapping) or set(value) != required:
            raise ConversationCheckpointInvalid(
                "conversation transcript field set mismatch")
        if value.get("schema") != SCHEMA_TRANSCRIPT:
            raise ConversationCheckpointInvalid("conversation transcript schema mismatch")
        try:
            messages = base64.b64decode(
                str(value["applicationMessages"]), validate=True)
            receipts = tuple(
                base64.b64decode(str(item), validate=True)
                for item in value["providerRoleReceipts"]
            )
        except (TypeError, ValueError) as exc:
            raise ConversationCheckpointInvalid(
                "conversation transcript base64 field is malformed") from exc
        return cls(
            conversation_id=str(value["conversationId"]),
            context_epoch=int(value["contextEpoch"]),
            requester_identity=str(value["requesterIdentity"]),
            service_name=str(value["serviceName"]),
            security_domain_digest=str(value["securityDomainDigest"]),
            application_messages=messages,
            tokenizer_digest=str(value["tokenizerDigest"]),
            chat_template_digest=str(value["chatTemplateDigest"]),
            canonical_token_ids=tuple(int(item) for item in value["canonicalTokenIds"]),
            prefix_digest=str(value["prefixDigest"]),
            prefix_token_count=int(value["prefixTokenCount"]),
            provider_role_receipts=receipts,
            checkpoint_digest=str(value["checkpointDigest"]),
            plan_role_map_digest=str(value["planRoleMapDigest"]),
            created_at_ms=int(value["createdAtMs"]),
            expires_at_ms=int(value["expiresAtMs"]),
        )


@dataclass(frozen=True)
class ConversationStateEntryV1:
    receipt: ProviderConversationStateReceiptV1
    # The aggregate User checkpoint that authorized promotion.  It is absent
    # only for legacy local test entries; production committed entries bind it
    # before a later Selection can acquire the state.
    checkpoint_digest: str = ""
    residency_tier: ResidencyTier = ResidencyTier.GPU_RESIDENT
    lifecycle: StateLifecycle = StateLifecycle.IDLE
    logical_bytes: int = 0
    allocated_bytes: int = 0
    device_id: str = ""
    host_pool_id: str = ""
    pin_count: int = 0
    prefetch_generation: int = 0
    last_access_sequence: int = 0
    created_at_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    last_used_at_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    expires_at_ms: int = 0
    transfer_bytes: int = 0
    transfer_latency_ms: int = 0
    opaque_state: Any = field(default=None, repr=False, compare=False)

    def __post_init__(self) -> None:
        object.__setattr__(self, "residency_tier", ResidencyTier(self.residency_tier))
        object.__setattr__(self, "lifecycle", StateLifecycle(self.lifecycle))
        if self.checkpoint_digest:
            _require_digest(self.checkpoint_digest, "conversation checkpoint_digest")
        if (self.logical_bytes < 0 or self.allocated_bytes < self.logical_bytes
                or self.pin_count < 0 or self.prefetch_generation < 0
                or self.last_access_sequence < 0 or self.expires_at_ms < 0
                or self.transfer_bytes < 0 or self.transfer_latency_ms < 0):
            raise ValueError("invalid conversation state accounting")

    @property
    def key(self) -> tuple[str, int, str]:
        return (self.receipt.conversation_id, self.receipt.successor_context_epoch,
                self.receipt.role_name)

    def public_metadata(self) -> dict[str, Any]:
        metadata = {
            "conversationId": self.receipt.conversation_id,
            "contextEpoch": self.receipt.successor_context_epoch,
            "role": self.receipt.role_name,
            "residencyTier": self.residency_tier.value,
            "lifecycle": self.lifecycle.value,
            "logicalBytes": self.logical_bytes,
            "allocatedBytes": self.allocated_bytes,
            "pinCount": self.pin_count,
            "prefetchGeneration": self.prefetch_generation,
            "transferBytes": self.transfer_bytes,
            "transferLatencyMs": self.transfer_latency_ms,
            "expiresAtMs": self.expires_at_ms,
        }
        if self.checkpoint_digest:
            metadata["checkpointDigest"] = self.checkpoint_digest
        return metadata


@dataclass(frozen=True)
class ConversationStateMetrics:
    request_local_entries: int
    conversation_entries: int
    request_local_releases: int
    promotions: int
    conversation_hits: int
    conversation_misses: int
    evictions: int
    prefetched_entries: int
    prefetch_bytes: int
    prefetch_latency_ms: int


class ProviderConversationStateManager:
    """Provider-local owner for request and conversation state.

    The two stores are intentionally separate.  A later request can only find
    an exact receipt in ``_conversation``; it can never search ``_request`` by
    conversation ID or reuse a terminal request entry implicitly.
    """

    def __init__(
        self,
        *,
        provider_identity: str,
        provider_boot_id: str,
        cache_epoch: int = 1,
        gpu_byte_quota: int = 8 * 1024 * 1024 * 1024,
        gpu_entry_quota: int = 128,
        host_byte_quota: int = 8 * 1024 * 1024 * 1024,
        host_entry_quota: int = 128,
        retention_ms: int = DEFAULT_RETENTION_MS,
        prefetch_delay_ms: int = 0,
        executor: ThreadPoolExecutor | None = None,
        zeroize_state: Callable[[Any], None] | None = None,
    ) -> None:
        if (not provider_identity or not provider_boot_id or cache_epoch < 0
                or gpu_byte_quota < 0 or host_byte_quota < 0
                or gpu_entry_quota < 0 or host_entry_quota < 0
                or retention_ms <= 0 or retention_ms > DEFAULT_RETENTION_MS
                or prefetch_delay_ms < 0 or prefetch_delay_ms > 60_000):
            raise ValueError("invalid Provider conversation-state quotas")
        if zeroize_state is not None and not callable(zeroize_state):
            raise TypeError("zeroize_state must be callable")
        self.provider_identity = provider_identity
        self.provider_boot_id = provider_boot_id
        self.cache_epoch = int(cache_epoch)
        self.gpu_byte_quota = int(gpu_byte_quota)
        self.gpu_entry_quota = int(gpu_entry_quota)
        self.host_byte_quota = int(host_byte_quota)
        self.host_entry_quota = int(host_entry_quota)
        self.retention_ms = int(retention_ms)
        # Test/deployment controls may delay a transfer to exercise
        # cancellation.  Production callers leave this at zero; the delay is
        # intentionally inside the Provider-owned transfer worker, never on
        # the NDN wire or in the user-side coordinator.
        self.prefetch_delay_ms = int(prefetch_delay_ms)
        self._lock = RLock()
        self._request: dict[tuple[str, str], tuple[Any, int]] = {}
        self._conversation: dict[tuple[str, int, str], ConversationStateEntryV1] = {}
        # Promotion candidates remain separate from both committed conversation
        # state and request-local state until the aggregate coordinator commits
        # every selected role.  They are pinned/COMMITTING and therefore cannot
        # be evicted while a multi-role transaction is in flight.
        self._staged_promotions: dict[
            tuple[str, int, str], ConversationStateEntryV1] = {}
        self._prefetch: dict[tuple[str, int, str], Future] = {}
        # A single HOST->GPU transfer may be shared by several concurrent
        # requests that reference the same committed parent.  Cancellation is
        # waiter-scoped: one child must not revoke the shared flight needed by
        # another child that is still active.
        self._prefetch_waiters: dict[tuple[str, int, str], int] = {}
        self._sequence = 0
        self._request_releases = 0
        self._promotions = 0
        self._hits = 0
        self._misses = 0
        self._evictions = 0
        self._prefetched_entries = 0
        self._prefetch_bytes = 0
        self._prefetch_latency_ms = 0
        self._executor = executor or ThreadPoolExecutor(
            max_workers=2, thread_name_prefix="ndnsf-di-conversation-prefetch")
        # Native Providers own real tensor buffers.  The Python bridge still
        # needs a release boundary for adapter/test state, so integrations may
        # inject their allocator's scrubber; otherwise mutable containers are
        # cleared before their references are dropped.
        self._zeroize_state = zeroize_state

    @staticmethod
    def _builtin_zeroize(value: Any) -> None:
        """Best-effort clearing for bridge-owned mutable state containers."""
        if isinstance(value, bytearray):
            value[:] = b"\x00" * len(value)
        elif isinstance(value, memoryview):
            if not value.readonly:
                value[:] = b"\x00" * value.nbytes
        elif isinstance(value, dict):
            # Do not recurse: adapter copies can retain aliases to the
            # authoritative host object (for example ``{"gpu": state}``).
            # Clearing nested aliases here would destroy a still-usable host
            # entry when a cancelled copy is discarded.
            value.clear()
        elif isinstance(value, list):
            value.clear()

    @staticmethod
    def _contains_identity(root: Any, target: Any) -> bool:
        """Return whether a copy still aliases an authoritative state value."""
        if root is target:
            return True
        if not isinstance(root, (dict, list, tuple, set, frozenset)):
            return False
        pending = list(root.values()) if isinstance(root, dict) else list(root)
        seen: set[int] = set()
        while pending:
            value = pending.pop()
            if value is target:
                return True
            identity = id(value)
            if identity in seen:
                continue
            seen.add(identity)
            if isinstance(value, dict):
                pending.extend(value.values())
            elif isinstance(value, (list, tuple, set, frozenset)):
                pending.extend(value)
        return False

    def _release_state(self, value: Any) -> None:
        """Release one state value without breaking a teardown path."""
        if value is None:
            return
        try:
            if self._zeroize_state is not None:
                self._zeroize_state(value)
            else:
                self._builtin_zeroize(value)
        except Exception:
            # Ownership has already been removed.  An optional scrubber must
            # not leave a failed prefetch or Provider restart permanently
            # stuck in a transitional lifecycle.
            return

    def put_request_local(self, request_id: str, role: str, state: Any,
                          *, logical_bytes: int) -> None:
        if not request_id or not role.startswith("/") or logical_bytes < 0:
            raise ValueError("invalid request-local state")
        with self._lock:
            self._request[(request_id, role)] = (state, int(logical_bytes))

    def release_request_local(self, request_id: str, role: str) -> bool:
        with self._lock:
            item = self._request.pop((request_id, role), None)
            removed = item is not None
            if removed:
                self._request_releases += 1
        if item is not None:
            self._release_state(item[0])
        return removed

    def request_local_count(self) -> int:
        with self._lock:
            return len(self._request)

    def acquire_for_request(
        self,
        continuation: ConversationContinuation,
        *,
        request_id: str,
        role: str,
        receipt: ProviderConversationStateReceiptV1,
        now_ms: int | None = None,
        deadline_ms: int | None = None,
        cancelled: Callable[[], bool] | None = None,
    ) -> ConversationStateEntryV1:
        """Acquire an exact conversation state for a *new* Request.

        Request-local state is intentionally not consulted here.  The caller
        must present an APPEND_DELTA continuation whose expected parent epoch
        is the receipt's committed successor epoch, and its request identity
        must differ from the Request that originally produced the receipt.
        Host-resident state is made GPU-dispatchable through the existing
        single-flight prefetch path before the returned entry is pinned.
        """
        if not isinstance(continuation, ConversationContinuation):
            raise TypeError("continuation must be ConversationContinuation")
        if continuation.mode is not ConversationInputMode.APPEND_DELTA:
            raise ConversationCheckpointInvalid(
                "conversation state acquisition requires APPEND_DELTA")
        if not request_id or request_id == receipt.origin_request_id:
            raise ConversationCheckpointInvalid(
                "conversation state acquisition requires a fresh request")
        if not role or role != receipt.role_name:
            raise ConversationCheckpointInvalid(
                "conversation state acquisition role mismatch")
        if continuation.conversation_id != receipt.conversation_id:
            raise ConversationCheckpointInvalid(
                "conversation state acquisition conversation mismatch")
        if continuation.expected_parent_context_epoch != receipt.successor_context_epoch:
            raise ConversationCheckpointInvalid(
                "conversation state acquisition parent epoch mismatch")
        now = int(now_ms if now_ms is not None else time.time() * 1000)
        with self._lock:
            entry = self._peek_conversation_locked(receipt, now)
            if entry is None:
                self._misses += 1
                raise ConversationStateUnavailable(
                    "conversation state is unavailable")
            if entry.checkpoint_digest:
                # A committed entry is authorized by the exact aggregate
                # checkpoint that promoted it.  Do not let a valid receipt be
                # replayed with a different parent checkpoint.
                try:
                    parent = ConversationCheckpointV1.from_bytes(
                        continuation.parent_checkpoint or b"")
                except ConversationCheckpointInvalid as exc:
                    self._misses += 1
                    raise ConversationCheckpointInvalid(
                        "conversation state parent checkpoint is malformed") from exc
                if parent.checkpoint_digest != entry.checkpoint_digest:
                    self._misses += 1
                    raise ConversationCheckpointInvalid(
                        "conversation state parent checkpoint mismatch")
            host_resident = entry.residency_tier is ResidencyTier.HOST_RESIDENT
            if entry.residency_tier is ResidencyTier.EVICTED:
                self._misses += 1
                raise ConversationStateUnavailable("conversation state was evicted")

        if host_resident:
            if cancelled is not None and not callable(cancelled):
                raise TypeError("cancelled must be callable")
            future = self.prefetch_to_gpu(receipt)
            prefetch_key = (receipt.conversation_id,
                            receipt.successor_context_epoch,
                            receipt.role_name)
            self._add_prefetch_waiter(prefetch_key)
            timeout = None
            if deadline_ms is not None:
                remaining = int(deadline_ms) - int(time.time() * 1000)
                if remaining <= 0:
                    self._cancel_prefetch_for_waiter(receipt)
                    self._remove_prefetch_waiter(prefetch_key)
                    raise ConversationStateUnavailable(
                        "conversation state acquisition deadline expired")
                timeout = remaining / 1000.0
            # Poll a bounded transfer so Core cancellation can revoke an
            # in-flight Provider copy.  A single long ``Future.result`` would
            # otherwise leave the staged state alive until the deadline even
            # after the user had cancelled the stream.
            deadline_monotonic = (time.monotonic() + timeout
                                  if timeout is not None else None)
            try:
                while True:
                    if cancelled is not None and cancelled():
                        self._cancel_prefetch_for_waiter(receipt)
                        raise ConversationStateUnavailable(
                            "conversation state prefetch cancelled")
                    wait_timeout = 0.02
                    if deadline_monotonic is not None:
                        remaining = deadline_monotonic - time.monotonic()
                        if remaining <= 0:
                            self._cancel_prefetch_for_waiter(receipt)
                            raise ConversationStateUnavailable(
                                "conversation state acquisition deadline expired")
                        wait_timeout = min(wait_timeout, remaining)
                    try:
                        future.result(timeout=wait_timeout)
                    except FutureTimeoutError:
                        continue
                    except Exception as exc:
                        if ((cancelled is not None and cancelled()) or
                                (isinstance(exc, ConversationStateUnavailable)
                                 and "cancel" in str(exc).lower())):
                            raise ConversationStateUnavailable(
                                "conversation state prefetch cancelled") from exc
                        raise ConversationStateUnavailable(
                            "conversation state prefetch failed") from exc
                    else:
                        break
            finally:
                self._remove_prefetch_waiter(prefetch_key)

        with self._lock:
            entry = self._peek_conversation_locked(receipt, int(time.time() * 1000))
            if entry is None or entry.residency_tier is not ResidencyTier.GPU_RESIDENT:
                self._misses += 1
                raise ConversationStateUnavailable(
                    "conversation state is not GPU-ready")
            self._sequence += 1
            entry = replace(
                entry, lifecycle=StateLifecycle.PINNED,
                pin_count=entry.pin_count + 1,
                last_access_sequence=self._sequence,
                last_used_at_ms=int(time.time() * 1000),
            )
            self._conversation[entry.key] = entry
            self._hits += 1
            return entry

    def release_for_request(
        self,
        continuation: ConversationContinuation,
        *,
        request_id: str,
        receipt: ProviderConversationStateReceiptV1,
    ) -> bool:
        """Release a conversation-state pin acquired by a fresh Request."""
        if not isinstance(continuation, ConversationContinuation):
            raise TypeError("continuation must be ConversationContinuation")
        if continuation.mode is not ConversationInputMode.APPEND_DELTA:
            raise ConversationCheckpointInvalid(
                "conversation state release requires APPEND_DELTA")
        if not request_id or request_id == receipt.origin_request_id:
            raise ConversationCheckpointInvalid(
                "conversation state release requires a fresh request")
        if continuation.conversation_id != receipt.conversation_id:
            raise ConversationCheckpointInvalid(
                "conversation state release conversation mismatch")
        return self.unpin(receipt)

    def promote_request_local(
        self,
        *,
        request_id: str,
        role: str,
        receipt: ProviderConversationStateReceiptV1,
        logical_bytes: int,
        checkpoint_digest: str = "",
        now_ms: int | None = None,
    ) -> ConversationStateEntryV1:
        self.stage_request_local_promotion(
            request_id=request_id, role=role, receipt=receipt,
            logical_bytes=logical_bytes, now_ms=now_ms)
        try:
            return self.commit_staged_promotion(
                receipt, request_id=request_id, role=role,
                checkpoint_digest=checkpoint_digest)
        except BaseException:
            self.rollback_staged_promotion(receipt)
            raise

    def stage_request_local_promotion(
        self,
        *,
        request_id: str,
        role: str,
        receipt: ProviderConversationStateReceiptV1,
        logical_bytes: int,
        now_ms: int | None = None,
    ) -> ConversationStateEntryV1:
        """Stage one request-local state for an aggregate promotion.

        Staging does not remove the request-local owner or expose a usable
        conversation entry.  The caller must commit or roll back the returned
        candidate.  Capacity is reserved without evicting an existing entry,
        so rollback can always preserve the previously committed parent.
        """
        if (receipt.provider_identity != self.provider_identity
                or receipt.provider_boot_id != self.provider_boot_id
                or receipt.cache_epoch != self.cache_epoch):
            raise ConversationStateUnavailable(
                "receipt Provider/cache binding mismatch")
        if not request_id or not role.startswith("/") or logical_bytes < 0:
            raise ValueError("invalid conversation promotion candidate")
        with self._lock:
            request_key = (request_id, role)
            item = self._request.get(request_key)
            if item is None:
                raise ConversationStateUnavailable(
                    "request-local state is unavailable")
            state, _ = item
            now = int(now_ms if now_ms is not None else time.time() * 1000)
            if now >= receipt.expires_at_ms:
                raise ConversationStateUnavailable(
                    "conversation promotion candidate is expired")
            expiry = min(receipt.expires_at_ms, now + self.retention_ms)
            self._sequence += 1
            entry = ConversationStateEntryV1(
                receipt=receipt,
                residency_tier=ResidencyTier.GPU_RESIDENT,
                lifecycle=StateLifecycle.COMMITTING,
                logical_bytes=int(logical_bytes),
                allocated_bytes=int(logical_bytes),
                device_id="gpu:active",
                pin_count=1,
                last_access_sequence=self._sequence,
                created_at_ms=now,
                last_used_at_ms=now,
                expires_at_ms=expiry,
                opaque_state=state,
            )
            if entry.key in self._conversation or entry.key in self._staged_promotions:
                raise ConversationStateConflict(
                    "conversation promotion candidate already exists")
            if not self._can_reserve_gpu_locked(entry):
                raise ConversationStateUnavailable(
                    "GPU conversation-state quota exceeded")
            self._staged_promotions[entry.key] = entry
            return entry

    def commit_staged_promotion(
        self, receipt: ProviderConversationStateReceiptV1, *,
        request_id: str | None = None, role: str | None = None,
        checkpoint_digest: str = "",
    ) -> ConversationStateEntryV1:
        """Commit one previously staged candidate.

        Multi-role callers should use ``ConversationStatePromotionTransaction``
        so all candidates are validated while their manager locks are held.
        """
        if checkpoint_digest:
            _require_digest(checkpoint_digest, "checkpoint_digest")
        with self._lock:
            entry = self._staged_promotions.get((
                receipt.conversation_id, receipt.successor_context_epoch,
                receipt.role_name))
            if entry is None or entry.receipt != receipt:
                raise ConversationStateUnavailable(
                    "conversation promotion candidate is unavailable")
            if not self._can_reserve_gpu_locked(entry):
                raise ConversationStateUnavailable(
                    "GPU conversation-state quota changed")
            owner_request_id = str(request_id or receipt.origin_request_id)
            owner_role = str(role or receipt.role_name)
            self._staged_promotions.pop(entry.key, None)
            committed = replace(
                entry, checkpoint_digest=checkpoint_digest,
                lifecycle=StateLifecycle.IDLE, pin_count=0)
            self._conversation[entry.key] = committed
            self._request.pop((owner_request_id, owner_role), None)
            self._request_releases += 1
            self._promotions += 1
            return committed

    def rollback_staged_promotion(
        self, receipt: ProviderConversationStateReceiptV1,
    ) -> bool:
        """Discard a staged candidate while retaining its request-local owner."""
        with self._lock:
            key = (receipt.conversation_id, receipt.successor_context_epoch,
                   receipt.role_name)
            return self._staged_promotions.pop(key, None) is not None

    def _can_reserve_gpu_locked(self, entry: ConversationStateEntryV1) -> bool:
        """Check GPU quota with staged candidates included; never evict."""
        rows = [item for item in self._conversation.values()
                if item.key != entry.key and
                item.residency_tier is ResidencyTier.GPU_RESIDENT]
        rows.extend(item for item in self._staged_promotions.values()
                    if item.key != entry.key)
        used_bytes = sum(item.allocated_bytes for item in rows)
        used_entries = len(rows)
        return (entry.logical_bytes <= self.gpu_byte_quota
                and used_entries + 1 <= self.gpu_entry_quota
                and used_bytes + entry.allocated_bytes <= self.gpu_byte_quota)

    def _rollback_committed_promotion(
        self, receipt: ProviderConversationStateReceiptV1,
    ) -> bool:
        """Undo a transaction commit if protected journal append fails."""
        with self._lock:
            key = (receipt.conversation_id, receipt.successor_context_epoch,
                   receipt.role_name)
            entry = self._conversation.get(key)
            if (entry is None or entry.receipt != receipt
                    or entry.lifecycle is not StateLifecycle.IDLE
                    or entry.pin_count != 0):
                return False
            self._conversation.pop(key, None)
            self._request[(receipt.origin_request_id, receipt.role_name)] = (
                entry.opaque_state, entry.logical_bytes)
            self._request_releases = max(0, self._request_releases - 1)
            self._promotions = max(0, self._promotions - 1)
            return True

    def lookup(self, receipt: ProviderConversationStateReceiptV1,
               *, now_ms: int | None = None) -> ConversationStateEntryV1 | None:
        now = int(now_ms if now_ms is not None else time.time() * 1000)
        with self._lock:
            entry = self._peek_conversation_locked(receipt, now)
            if entry is None:
                self._misses += 1
                return None
            self._sequence += 1
            self._hits += 1
            updated = replace(entry, last_access_sequence=self._sequence,
                              last_used_at_ms=now)
            self._conversation[entry.key] = updated
            return updated

    def _peek_conversation_locked(
        self,
        receipt: ProviderConversationStateReceiptV1,
        now_ms: int,
    ) -> ConversationStateEntryV1 | None:
        """Return an exact entry without changing hit/miss accounting."""
        key = (receipt.conversation_id, receipt.successor_context_epoch,
               receipt.role_name)
        entry = self._conversation.get(key)
        if (entry is None or entry.receipt != receipt
                or entry.receipt.provider_boot_id != self.provider_boot_id
                or entry.receipt.cache_epoch != self.cache_epoch
                or (entry.expires_at_ms and now_ms >= entry.expires_at_ms)):
            return None
        return entry

    def pin(self, receipt: ProviderConversationStateReceiptV1) -> ConversationStateEntryV1:
        with self._lock:
            entry = self.lookup(receipt)
            if entry is None:
                raise ConversationStateUnavailable("conversation state is unavailable")
            updated = replace(entry, lifecycle=StateLifecycle.PINNED,
                              pin_count=entry.pin_count + 1)
            self._conversation[updated.key] = updated
            return updated

    def unpin(self, receipt: ProviderConversationStateReceiptV1) -> ConversationStateEntryV1:
        key = (receipt.conversation_id, receipt.successor_context_epoch,
               receipt.role_name)
        with self._lock:
            entry = self._conversation.get(key)
            if entry is None or entry.receipt != receipt or entry.pin_count < 1:
                raise ConversationStateUnavailable("state pin is not owned")
            remaining = entry.pin_count - 1
            updated = replace(
                entry,
                lifecycle=(StateLifecycle.PINNED if remaining
                            else StateLifecycle.IDLE),
                pin_count=remaining)
            self._conversation[key] = updated
            return updated

    def pause_to_host(self, receipt: ProviderConversationStateReceiptV1,
                      *, host_pool_id: str = "host:conversation") -> ConversationStateEntryV1:
        key = (receipt.conversation_id, receipt.successor_context_epoch,
               receipt.role_name)
        with self._lock:
            entry = self._conversation.get(key)
            if entry is None or entry.receipt != receipt:
                raise ConversationStateUnavailable("state is unavailable")
            if entry.lifecycle is not StateLifecycle.IDLE:
                raise ConversationStateUnavailable("active state cannot move tiers")
            # A new host transition invalidates any completed or in-flight
            # prefetch handle for the previous residency generation.
            self._prefetch.pop(key, None)
            self._prefetch_waiters.pop(key, None)
            self._ensure_host_capacity(entry)
            updated = replace(entry, residency_tier=ResidencyTier.HOST_RESIDENT,
                              device_id="", host_pool_id=host_pool_id,
                              last_access_sequence=self._next_sequence())
            self._conversation[key] = updated
            return updated

    def prefetch_to_gpu(
        self,
        receipt: ProviderConversationStateReceiptV1,
        *,
        copy_state: Callable[[Any], Any] | None = None,
    ) -> Future:
        key = (receipt.conversation_id, receipt.successor_context_epoch,
               receipt.role_name)
        with self._lock:
            existing = self._prefetch.get(key)
            entry = self._conversation.get(key)
            if entry is None or entry.receipt != receipt:
                self._misses += 1
                raise ConversationStateUnavailable("state is unavailable")
            if existing is not None:
                if not existing.done():
                    return existing
                # Keep a completed successful handle stable for this GPU
                # generation, making repeated callers observe one single
                # flight.  pause_to_host() removes it before a new transfer.
                if entry.residency_tier is ResidencyTier.GPU_RESIDENT:
                    try:
                        existing.result()
                    except BaseException:
                        self._prefetch.pop(key, None)
                    else:
                        return existing
            if entry.residency_tier is ResidencyTier.GPU_RESIDENT:
                future: Future = Future()
                future.set_result(entry)
                return future
            if entry.residency_tier is ResidencyTier.EVICTED:
                raise ConversationStateUnavailable("state was evicted")
            if entry.lifecycle is not StateLifecycle.IDLE:
                raise ConversationStateUnavailable("state is not dispatchable")
            self._sequence += 1
            staged = replace(entry, lifecycle=StateLifecycle.PREFETCHING,
                             prefetch_generation=entry.prefetch_generation + 1,
                             last_access_sequence=self._sequence)
            # Account for the incoming GPU copy before exposing staged state;
            # quota eviction may remove only inactive, unpinned GPU entries.
            self._ensure_capacity(staged, replacing_key=key)
            self._conversation[key] = staged
            started = time.perf_counter()

            def run() -> ConversationStateEntryV1:
                if self.prefetch_delay_ms:
                    time.sleep(self.prefetch_delay_ms / 1000.0)
                value = copy_state(staged.opaque_state) if copy_state else staged.opaque_state
                with self._lock:
                    current = self._conversation.get(key)
                    if (current is None
                            or current.lifecycle is not StateLifecycle.PREFETCHING
                            or current.prefetch_generation
                            != staged.prefetch_generation):
                        # A cancelled/replaced flight may have produced a
                        # distinct temporary device value.  Never scrub the
                        # still-authoritative host value when the copy function
                        # returned it by identity.
                        if (value is not staged.opaque_state
                                and not self._contains_identity(
                                    value, staged.opaque_state)):
                            self._release_state(value)
                        raise ConversationStateUnavailable("prefetch was cancelled")
                    old_state = current.opaque_state
                    elapsed = max(0, int((time.perf_counter() - started) * 1000))
                    updated = replace(current, residency_tier=ResidencyTier.GPU_RESIDENT,
                                      lifecycle=StateLifecycle.IDLE,
                                      opaque_state=value, device_id="gpu:active",
                                      host_pool_id="", transfer_bytes=current.logical_bytes,
                                      transfer_latency_ms=elapsed,
                                      last_used_at_ms=int(time.time() * 1000),
                                      last_access_sequence=self._next_sequence())
                    self._conversation[key] = updated
                    self._prefetched_entries += 1
                    self._prefetch_bytes += current.logical_bytes
                    self._prefetch_latency_ms += elapsed
                    if (value is not old_state
                            and not self._contains_identity(value, old_state)):
                        self._release_state(old_state)
                    return updated

            def run_with_failure_recovery() -> ConversationStateEntryV1:
                try:
                    return run()
                except BaseException:
                    with self._lock:
                        current = self._conversation.get(key)
                        if (current is not None
                                and current.prefetch_generation
                                == staged.prefetch_generation
                                and current.lifecycle is StateLifecycle.PREFETCHING):
                            self._conversation[key] = replace(
                                current,
                                residency_tier=ResidencyTier.HOST_RESIDENT,
                                lifecycle=StateLifecycle.IDLE,
                                device_id="",
                                host_pool_id=(current.host_pool_id
                                              or "host:conversation"),
                            )
                    raise

            future = self._executor.submit(run_with_failure_recovery)
            self._prefetch[key] = future
            return future

    def cancel_prefetch(self, receipt: ProviderConversationStateReceiptV1) -> bool:
        key = (receipt.conversation_id, receipt.successor_context_epoch,
               receipt.role_name)
        with self._lock:
            future = self._prefetch.pop(key, None)
            # ``Future.cancel()`` returns False once the worker has started.
            # The cancellation contract is about revoking the staged state,
            # not about winning a scheduler race: mark the entry idle so the
            # worker fails its generation/lifecycle check, and report that an
            # in-flight cancellation was accepted even when the thread is
            # already running.
            in_flight = future is not None and not future.done()
            entry = self._conversation.get(key)
            if entry is not None and entry.lifecycle is StateLifecycle.PREFETCHING:
                # Bump the generation as well as changing lifecycle.  A new
                # prefetch may become PREFETCHING before the cancelled worker
                # wakes; without this fence that old worker could commit stale
                # data into the new flight.
                self._conversation[key] = replace(
                    entry, lifecycle=StateLifecycle.IDLE,
                    prefetch_generation=entry.prefetch_generation + 1)
            cancelled = bool(future is not None and future.cancel())
            return bool(in_flight or cancelled)

    def _add_prefetch_waiter(self, key: tuple[str, int, str]) -> None:
        with self._lock:
            self._prefetch_waiters[key] = self._prefetch_waiters.get(key, 0) + 1

    def _remove_prefetch_waiter(self, key: tuple[str, int, str]) -> None:
        with self._lock:
            count = self._prefetch_waiters.get(key, 0)
            if count <= 1:
                self._prefetch_waiters.pop(key, None)
            else:
                self._prefetch_waiters[key] = count - 1

    def _cancel_prefetch_for_waiter(
            self, receipt: ProviderConversationStateReceiptV1) -> bool:
        key = (receipt.conversation_id, receipt.successor_context_epoch,
               receipt.role_name)
        with self._lock:
            # The current waiter is still counted here.  Keep the shared
            # transfer alive whenever another waiter remains.
            if self._prefetch_waiters.get(key, 0) > 1:
                return False
        return self.cancel_prefetch(receipt)

    def evict_inactive(self, *, now_ms: int | None = None) -> int:
        now = int(now_ms if now_ms is not None else time.time() * 1000)
        with self._lock:
            candidates = [entry for entry in self._conversation.values()
                          if entry.lifecycle is StateLifecycle.IDLE
                          and entry.pin_count == 0
                          and entry.expires_at_ms and now >= entry.expires_at_ms]
            candidates.sort(key=lambda item: item.last_access_sequence)
            for entry in candidates:
                self._conversation.pop(entry.key, None)
                self._evictions += 1
            return len(candidates) + self._evict_for_quota()

    def invalidate_provider_boot(self, provider_boot_id: str) -> int:
        with self._lock:
            if provider_boot_id == self.provider_boot_id:
                return 0
            count = len(self._conversation)
            values: list[Any] = []
            values.extend(state for state, _bytes in self._request.values())
            values.extend(entry.opaque_state
                          for entry in self._conversation.values())
            values.extend(entry.opaque_state
                          for entry in self._staged_promotions.values())
            for future in self._prefetch.values():
                future.cancel()
            self._request_releases += len(self._request)
            self._request.clear()
            self._conversation.clear()
            self._staged_promotions.clear()
            self._prefetch.clear()
            self._prefetch_waiters.clear()
            self._evictions += count
            self.provider_boot_id = provider_boot_id
            self.cache_epoch += 1
        # Request-local, staged, and committed views can temporarily alias the
        # same object; invoke the scrubber at most once per identity.
        seen: set[int] = set()
        for value in values:
            if value is None or id(value) in seen:
                continue
            seen.add(id(value))
            self._release_state(value)
        return count

    def metrics(self) -> ConversationStateMetrics:
        with self._lock:
            return ConversationStateMetrics(
                request_local_entries=len(self._request),
                conversation_entries=len(self._conversation),
                request_local_releases=self._request_releases,
                promotions=self._promotions,
                conversation_hits=self._hits,
                conversation_misses=self._misses,
                evictions=self._evictions,
                prefetched_entries=self._prefetched_entries,
                prefetch_bytes=self._prefetch_bytes,
                prefetch_latency_ms=self._prefetch_latency_ms,
            )

    def _next_sequence(self) -> int:
        self._sequence += 1
        return self._sequence

    def _ensure_capacity(self, entry: ConversationStateEntryV1,
                         *, replacing_key: tuple[str, int, str]) -> None:
        if entry.logical_bytes > self.gpu_byte_quota or self.gpu_entry_quota == 0:
            raise ConversationStateUnavailable("GPU conversation-state quota exceeded")
        self._evict_for_tier(ResidencyTier.GPU_RESIDENT,
                             incoming_bytes=entry.allocated_bytes,
                             incoming_entries=1, replacing_key=replacing_key)
        gpu_entries = sum(1 for key, item in self._conversation.items()
                          if key != replacing_key
                          and item.residency_tier is ResidencyTier.GPU_RESIDENT)
        gpu_bytes = sum(item.allocated_bytes for key, item in self._conversation.items()
                        if key != replacing_key
                        and item.residency_tier is ResidencyTier.GPU_RESIDENT)
        if gpu_entries + 1 > self.gpu_entry_quota or gpu_bytes + entry.allocated_bytes > self.gpu_byte_quota:
            raise ConversationStateUnavailable("GPU conversation-state quota exceeded")

    def _ensure_host_capacity(self, entry: ConversationStateEntryV1) -> None:
        if entry.logical_bytes > self.host_byte_quota or self.host_entry_quota == 0:
            raise ConversationStateUnavailable("host conversation-state quota exceeded")
        self._evict_for_tier(ResidencyTier.HOST_RESIDENT,
                             incoming_bytes=entry.allocated_bytes,
                             incoming_entries=1)
        host_entries = sum(1 for item in self._conversation.values()
                           if item.residency_tier is ResidencyTier.HOST_RESIDENT)
        host_bytes = sum(item.allocated_bytes for item in self._conversation.values()
                         if item.residency_tier is ResidencyTier.HOST_RESIDENT)
        if host_entries + 1 > self.host_entry_quota or host_bytes + entry.allocated_bytes > self.host_byte_quota:
            raise ConversationStateUnavailable("host conversation-state quota exceeded")

    def _evict_for_tier(self, tier: ResidencyTier, *, incoming_bytes: int = 0,
                        incoming_entries: int = 0,
                        replacing_key: tuple[str, int, str] | None = None) -> int:
        """Evict deterministic inactive LRU entries until a tier can fit."""
        if tier is ResidencyTier.GPU_RESIDENT:
            byte_quota, entry_quota = self.gpu_byte_quota, self.gpu_entry_quota
        else:
            byte_quota, entry_quota = self.host_byte_quota, self.host_entry_quota
        def usage() -> tuple[int, int]:
            rows = [item for key, item in self._conversation.items()
                    if key != replacing_key and item.residency_tier is tier]
            return sum(item.allocated_bytes for item in rows), len(rows)
        evicted = 0
        used_bytes, used_entries = usage()
        while ((used_bytes + incoming_bytes > byte_quota)
               or (used_entries + incoming_entries > entry_quota)):
            eligible = [item for key, item in self._conversation.items()
                        if key != replacing_key and item.residency_tier is tier
                        and item.lifecycle is StateLifecycle.IDLE
                        and item.pin_count == 0]
            if not eligible:
                break
            victim = min(eligible, key=lambda item: item.last_access_sequence)
            self._conversation.pop(victim.key, None)
            self._evictions += 1
            evicted += 1
            used_bytes, used_entries = usage()
        return evicted

    def _evict_for_quota(self) -> int:
        return (self._evict_for_tier(ResidencyTier.GPU_RESIDENT)
                + self._evict_for_tier(ResidencyTier.HOST_RESIDENT))


class ConversationStatePromotionTransaction:
    """Two-phase, all-role handoff from request-local to conversation state.

    Each candidate is staged while its request-local owner remains intact.
    ``commit`` acquires all participating manager locks in deterministic order,
    validates every candidate, and then moves them together.  A journal or
    coordinator failure can call ``rollback``; no existing conversation parent
    is evicted or replaced by staging.
    """

    def __init__(
        self,
        candidates: Iterable[tuple[ProviderConversationStateManager, str, str,
                                   ProviderConversationStateReceiptV1, int]],
        *,
        now_ms: int | None = None,
    ) -> None:
        normalized = tuple(candidates)
        if not normalized:
            raise ConversationStateUnavailable(
                "conversation promotion requires at least one role")
        self._candidates = normalized
        self._now_ms = now_ms
        self._lock = RLock()
        self._committed = False
        self._rolled_back = False
        self._committed_checkpoint_digest = ""
        seen: set[tuple[int, tuple[str, int, str]]] = set()
        staged: list[tuple[ProviderConversationStateManager,
                            ProviderConversationStateReceiptV1]] = []
        try:
            for manager, request_id, role, receipt, logical_bytes in normalized:
                if not isinstance(manager, ProviderConversationStateManager):
                    raise TypeError("promotion manager has an invalid type")
                if str(request_id) != receipt.origin_request_id \
                        or str(role) != receipt.role_name:
                    raise ConversationCheckpointInvalid(
                        "promotion candidate request/role does not match receipt")
                identity = (id(manager), (
                    receipt.conversation_id, receipt.successor_context_epoch,
                    receipt.role_name))
                if identity in seen:
                    raise ConversationStateConflict(
                        "duplicate conversation promotion candidate")
                seen.add(identity)
                manager.stage_request_local_promotion(
                    request_id=str(request_id), role=str(role), receipt=receipt,
                    logical_bytes=int(logical_bytes), now_ms=now_ms)
                staged.append((manager, receipt))
        except BaseException:
            for manager, receipt in reversed(staged):
                manager.rollback_staged_promotion(receipt)
            # Candidate construction owns the request-local records supplied
            # for this aggregate.  If one role cannot be staged (for example a
            # duplicate successor key), do not leave the other roles orphaned
            # behind a transaction that was never returned to the caller.
            for manager, request_id, role, _receipt, _logical_bytes in normalized:
                manager.release_request_local(str(request_id), str(role))
            raise

    @property
    def request_id(self) -> str:
        values = {receipt.origin_request_id for _, _, _, receipt, _ in self._candidates}
        if len(values) != 1:
            raise ConversationCheckpointInvalid(
                "promotion candidates do not share one origin request")
        return next(iter(values))

    @property
    def receipts(self) -> tuple[ProviderConversationStateReceiptV1, ...]:
        return tuple(item[3] for item in self._candidates)

    @property
    def committed(self) -> bool:
        with self._lock:
            return self._committed

    def commit(self, *, checkpoint_digest: str = "") -> None:
        if checkpoint_digest:
            _require_digest(checkpoint_digest, "checkpoint_digest")
        with self._lock:
            if self._rolled_back:
                raise ConversationStateConflict(
                    "conversation promotion transaction was rolled back")
            if self._committed:
                if (checkpoint_digest and
                        checkpoint_digest != self._committed_checkpoint_digest):
                    raise ConversationStateConflict(
                        "conversation promotion checkpoint binding changed")
                return
            managers = sorted(
                {item[0] for item in self._candidates}, key=id)
            with ExitStack() as stack:
                for manager in managers:
                    stack.enter_context(manager._lock)
                entries: list[tuple[ProviderConversationStateManager,
                                    ProviderConversationStateReceiptV1,
                                    ConversationStateEntryV1]] = []
                for manager, request_id, role, receipt, _logical_bytes in self._candidates:
                    key = (receipt.conversation_id,
                           receipt.successor_context_epoch, receipt.role_name)
                    entry = manager._staged_promotions.get(key)
                    if (entry is None or entry.receipt != receipt
                            or str(request_id) != receipt.origin_request_id
                            or str(role) != receipt.role_name
                            or not manager._can_reserve_gpu_locked(entry)):
                        raise ConversationStateUnavailable(
                            "conversation promotion candidate is no longer valid")
                    entries.append((manager, receipt, entry))
                # All validation is complete while all manager locks are held;
                # the following mutations cannot partially fail in normal use.
                for manager, receipt, entry in entries:
                    manager._staged_promotions.pop(entry.key, None)
                    manager._conversation[entry.key] = replace(
                        entry,
                        checkpoint_digest=checkpoint_digest,
                        lifecycle=StateLifecycle.IDLE,
                        pin_count=0)
                    manager._request.pop(
                        (receipt.origin_request_id, receipt.role_name), None)
                    manager._request_releases += 1
                    manager._promotions += 1
            self._committed = True
            self._committed_checkpoint_digest = checkpoint_digest

    def rollback(self, *, release_request_local: bool = False) -> None:
        with self._lock:
            if self._rolled_back:
                return
            if self._committed:
                for _manager, _request_id, _role, receipt, _logical_bytes in self._candidates:
                    # The inverse is best-effort but remains fail-closed: a
                    # caller must not claim rollback if an entry is externally
                    # pinned or changed after commit.
                    manager = next(
                        item[0] for item in self._candidates if item[3] == receipt)
                    if not manager._rollback_committed_promotion(receipt):
                        raise ConversationStateConflict(
                            "committed conversation promotion cannot be rolled back")
            else:
                for manager, request_id, role, receipt, _logical_bytes in self._candidates:
                    manager.rollback_staged_promotion(receipt)
                    if release_request_local:
                        manager.release_request_local(request_id, role)
            # A successful inverse returns the transaction to a non-committed
            # state.  This lets callers distinguish a durable promotion from
            # a journal failure that was safely rolled back.
            self._committed = False
            self._rolled_back = True
            self._committed_checkpoint_digest = ""


class ConversationCoordinator:
    """User-side transcript/checkpoint owner with linear CAS semantics."""

    def __init__(self, *, journal: Any | None = None, signer_key: bytes | None = None,
                 verification_keys: Iterable[bytes] = (),
                 requester_identity: str = "", service_name: str = "",
                 security_domain_digest: str = "") -> None:
        self.journal = journal
        if signer_key is None:
            key_source = getattr(journal, "authentication_key_ring", None)
            if key_source is None:
                raise ValueError(
                    "ConversationCoordinator requires an owner-injected "
                    "checkpoint authentication key")
            keys = tuple(bytes(item) for item in key_source(
                "conversation-checkpoint-v1"))
        else:
            keys = (bytes(signer_key), *(bytes(item)
                                        for item in verification_keys))
        if not keys or any(len(item) != 32 for item in keys):
            raise ValueError(
                "conversation checkpoint authentication keys must be 32 bytes")
        self.signer_key = keys[0]
        self._verification_keys = keys
        self.requester_identity = requester_identity
        self.service_name = service_name
        self.security_domain_digest = security_domain_digest
        self._lock = RLock()
        self._checkpoints: dict[str, ConversationCheckpointV1] = {}
        self._transcripts: dict[str, ConversationTranscriptRecordV1] = {}
        self._turns: dict[str, dict[str, Any]] = {}
        self._restore_journal_state()

    @staticmethod
    def _journal_envelope_id(conversation_id: str, context_epoch: int) -> str:
        # RuntimeJournal envelope IDs are local opaque spool names.  Never put
        # the caller's conversation ID, prompt, or checkpoint bytes in it.
        digest = hashlib.sha256(conversation_id.encode("utf-8")).hexdigest()
        return f"conversation-{digest}-{int(context_epoch)}"

    def _restore_journal_state(self) -> None:
        """Recover the latest authenticated checkpoint/transcript pair.

        Conversation records are stored as one encrypted RuntimeJournal
        envelope plus a small non-secret index record.  A malformed or
        undecryptable committed record is a fail-closed startup error rather
        than a silent empty conversation, since silently dropping a parent
        would make a later request unsafe.
        """
        if self.journal is None:
            return
        if not getattr(self.journal, "has_envelope_key", False):
            # APPClient rejects this earlier; keep direct coordinator use
            # explicit instead of ever falling back to plaintext persistence.
            if any(record.get("kind") == "conversation-checkpoint"
                   for record in self.journal.records()):
                raise ConversationCheckpointInvalid(
                    "conversation journal requires an envelope key")
            return
        latest: dict[str, tuple[int, ConversationCheckpointV1,
                                ConversationTranscriptRecordV1]] = {}
        for record in self.journal.records():
            if record.get("kind") != "conversation-checkpoint":
                continue
            metadata = record.get("payload", {})
            try:
                conversation_id = _require_conversation_id(
                    str(metadata["conversationId"]))
                epoch = int(metadata["contextEpoch"])
                envelope_id = str(metadata["envelopeId"])
                expected_wire_digest = str(metadata["wireDigest"])
                raw = self.journal.read_envelope(envelope_id)
                expected_payload_digest = str(metadata.get("payloadDigest", ""))
                if expected_payload_digest:
                    digest_matches = ("sha256:" + hashlib.sha256(raw).hexdigest()
                                      == expected_payload_digest)
                else:
                    # Older development records used wireDigest for the
                    # encrypted envelope.  RuntimeJournal already verifies
                    # that digest while locating/decrypting the envelope.
                    digest_matches = bool(expected_wire_digest)
                if not digest_matches:
                    raise ConversationCheckpointInvalid(
                        "conversation journal envelope digest mismatch")
                body = json.loads(raw.decode("utf-8"))
                if not isinstance(body, Mapping) \
                        or set(body) != {"checkpoint", "transcript"}:
                    raise ConversationCheckpointInvalid(
                        "conversation journal payload shape mismatch")
                checkpoint_bytes = base64.b64decode(
                    str(body["checkpoint"]), validate=True)
                checkpoint = ConversationCheckpointV1.from_bytes(checkpoint_bytes)
                transcript = ConversationTranscriptRecordV1.from_dict(
                    body["transcript"])
                if (checkpoint.conversation_id != conversation_id
                        or checkpoint.context_epoch != epoch
                        or transcript.conversation_id != conversation_id
                        or transcript.context_epoch != epoch
                        or transcript.checkpoint_digest != checkpoint.checkpoint_digest):
                    raise ConversationCheckpointInvalid(
                        "conversation journal identity mismatch")
                if not any(checkpoint.verify(key)
                           for key in self._verification_keys):
                    raise ConversationCheckpointInvalid(
                        "conversation checkpoint authentication failed")
                if checkpoint.expires_at_ms <= int(time.time() * 1000):
                    continue
            except ConversationCheckpointInvalid:
                raise
            except Exception as exc:
                raise ConversationCheckpointInvalid(
                    "conversation journal recovery failed") from exc
            previous = latest.get(conversation_id)
            if previous is None or epoch > previous[0]:
                latest[conversation_id] = (epoch, checkpoint, transcript)
        for conversation_id, (_epoch, checkpoint, transcript) in latest.items():
            self._checkpoints[conversation_id] = checkpoint
            self._transcripts[conversation_id] = transcript

    def begin_turn(
        self,
        continuation: ConversationContinuation,
        *,
        input_payload: bytes,
        canonical_token_ids: Iterable[int] | None = None,
        request_id: str | None = None,
        generation_id: str | None = None,
    ) -> dict[str, Any]:
        continuation = ConversationContinuation(**{
            **continuation.__dict__,
            "turn_input_digest": continuation.turn_input_digest or _bytes_digest(input_payload),
            "request_contract_digest": continuation.request_contract_digest or
            continuation.request_contract(input_digest=continuation.turn_input_digest or _bytes_digest(input_payload)),
        })
        with self._lock:
            parent = None
            if continuation.mode is ConversationInputMode.APPEND_DELTA:
                parent = ConversationCheckpointV1.from_bytes(
                    continuation.parent_checkpoint or b"")
                self._validate_parent(parent, continuation)
                current = self._checkpoints.get(continuation.conversation_id)
                if current is not None and current.checkpoint_digest != parent.checkpoint_digest:
                    raise ConversationStateConflict("parent checkpoint is not current")
                record = self._transcripts.get(continuation.conversation_id)
                if record is None:
                    raise ConversationStateUnavailable("conversation transcript is unavailable")
                tokens = tuple(int(item) for item in (canonical_token_ids or ()))
                if not tokens or tuple(tokens[:record.prefix_token_count]) != record.canonical_token_ids:
                    raise ConversationCheckpointInvalid("new transcript is not an exact parent prefix")
                appended = tokens[record.prefix_token_count:]
                if not appended:
                    raise ConversationCheckpointInvalid("APPEND_DELTA has no canonical suffix")
            else:
                parent = None
                tokens = tuple(int(item) for item in (canonical_token_ids or ()))
                appended = tokens
            request_id = str(request_id or ("/request/" + uuid.uuid4().hex))
            generation_id = str(generation_id or uuid.uuid4().hex)
            if not request_id or not generation_id:
                raise ValueError("conversation turn identities are required")
            if generation_id != generation_id.lower() or any(
                    char not in "0123456789abcdef" for char in generation_id
            ) or len(generation_id) != 32:
                raise ValueError("conversation generation_id must be 16-byte lowercase hex")
            if request_id in self._turns:
                raise ConversationStateConflict("conversation request ID is already pending")
            turn = {
                "conversationId": continuation.conversation_id,
                "mode": continuation.mode.value,
                "requestId": request_id,
                "generationId": generation_id,
                "attemptEpoch": 1,
                "parentCheckpoint": parent,
                "expectedParentContextEpoch": continuation.expected_parent_context_epoch,
                "input": bytes(input_payload),
                "canonicalTokenIds": tokens,
                "appendedTokenIds": tuple(appended),
                "requestContractDigest": continuation.request_contract_digest,
                "allowFullPrefillFallback": continuation.allow_full_prefill_fallback,
            }
            self._turns[request_id] = turn
            return dict(turn)

    def abort_turn(self, request_id: str) -> bool:
        """Discard a pre-publication turn after planner setup fails.

        This only removes the user-side pending transaction.  It cannot touch a
        committed checkpoint or any Provider-owned state, which keeps a failed
        planner attempt from creating a partially visible conversation turn.
        """
        with self._lock:
            return self._turns.pop(str(request_id), None) is not None

    def finalize_token_suffix(self, turn: Mapping[str, Any], suffix_token_count: int,
                              *, max_tokens: int = MAX_CHECKPOINT_FINALIZE_TOKENS) -> tuple[str, int]:
        if suffix_token_count < 0 or suffix_token_count > max_tokens:
            raise ConversationCheckpointInvalid("checkpoint finalization exceeds sealed V1 cap")
        # This is a state-only transition: callers use the returned epoch for
        # receipts, but no sampling/event/callback cursor is created.
        request_id = str(turn.get("requestId", ""))
        with self._lock:
            tracked = self._turns.get(request_id)
            if tracked is None:
                tracked = turn if isinstance(turn, dict) else None
            if tracked is None:
                raise ConversationStateUnavailable("unknown conversation turn")
            previous = int(tracked.get("finalizeTokenCount", 0))
            if previous + suffix_token_count > max_tokens:
                raise ConversationCheckpointInvalid(
                    "checkpoint finalization exceeds sealed V1 cap")
            tracked["finalizeTokenCount"] = previous + suffix_token_count
            next_epoch = previous + suffix_token_count
        return "CHECKPOINT_FINALIZE", next_epoch

    def prepare_checkpoint(
        self,
        request_id: str,
        *,
        result_payload: bytes,
        receipts: Iterable[ProviderConversationStateReceiptV1],
        model_contract_digest: str,
        plan_role_map_digest: str,
        tokenizer_digest: str,
        chat_template_digest: str,
        application_messages: bytes,
        canonical_token_ids: Iterable[int],
        now_ms: int | None = None,
        retention_ms: int = DEFAULT_RETENTION_MS,
    ) -> bytes:
        """Build the exact next checkpoint without making it visible.

        The returned bytes are only a transaction preview.  ``commit_turn``
        must be called with the same arguments (including ``now_ms``) to
        persist the checkpoint.  Keeping this deterministic lets a caller
        publish Provider COMMIT controls against the final checkpoint digest
        before changing the User-side durable parent.
        """
        if retention_ms <= 0 or retention_ms > DEFAULT_RETENTION_MS:
            raise ValueError("conversation retention exceeds V1 bound")
        with self._lock:
            turn = self._turns.get(request_id)
            if turn is None:
                raise ConversationStateUnavailable("unknown conversation turn")
            values = tuple(receipts)
            if not values:
                raise ConversationStateUnavailable("all-role receipts are required")
            tokens = tuple(int(item) for item in canonical_token_ids)
            now = int(now_ms if now_ms is not None else time.time() * 1000)
            parent = turn["parentCheckpoint"]
            parent_epoch = int(parent.context_epoch) if parent is not None else 0
            current = self._checkpoints.get(turn["conversationId"])
            if current is not None and current.context_epoch != parent_epoch:
                raise ConversationStateConflict("conversation parent context epoch changed")
            roles = {item.role_name for item in values}
            if len(roles) != len(values):
                raise ConversationCheckpointInvalid("duplicate role receipt")
            successor = parent_epoch + 1
            expected_service = self.service_name or values[0].service_name
            expected_requester = (self.requester_identity
                                  or values[0].requester_identity)
            expected_security = (self.security_domain_digest
                                 or values[0].security_domain_digest)
            expected_model = values[0].model_digest
            if not all((expected_service, expected_requester, expected_security,
                        expected_model)):
                raise ConversationCheckpointInvalid(
                    "role receipt authorization scope is incomplete")
            if any(item.conversation_id != turn["conversationId"]
                   or item.parent_context_epoch != parent_epoch
                   or item.successor_context_epoch != successor
                   or item.origin_request_id != request_id
                   or item.origin_generation_id != str(turn["generationId"])
                   or item.service_name != expected_service
                   or item.requester_identity != expected_requester
                   or item.security_domain_digest != expected_security
                   or item.model_digest != expected_model
                   or item.plan_role_map_digest != plan_role_map_digest
                   or item.prefix_token_count != len(tokens)
                   or item.prefix_digest != _prefix_digest(tokens)
                   for item in values):
                raise ConversationCheckpointInvalid("role receipt set is not common")
            earliest = min(item.expires_at_ms for item in values)
            checkpoint = ConversationCheckpointV1(
                conversation_id=turn["conversationId"],
                parent_context_epoch=parent_epoch,
                context_epoch=successor,
                service_name=expected_service,
                requester_identity=expected_requester,
                security_domain_digest=expected_security,
                model_contract_digest=model_contract_digest,
                plan_role_map_digest=plan_role_map_digest,
                logical_prefix_digest=_prefix_digest(tokens),
                prefix_token_count=len(tokens),
                role_receipt_digests={item.role_name: item.receipt_digest for item in values},
                issued_at_ms=now,
                expires_at_ms=min(earliest, now + retention_ms),
            ).sign(self.signer_key)
            # Touch the payload/digests here to preserve the same bytes-like
            # and scalar boundaries as commit_turn; they are not persisted by
            # this preview method.  The final commit revalidates them while
            # constructing the encrypted transcript.
            bytes(result_payload)
            bytes(application_messages)
            str(tokenizer_digest)
            str(chat_template_digest)
            return checkpoint.to_bytes()

    def commit_turn(
        self,
        request_id: str,
        *,
        result_payload: bytes,
        receipts: Iterable[ProviderConversationStateReceiptV1],
        model_contract_digest: str,
        plan_role_map_digest: str,
        tokenizer_digest: str,
        chat_template_digest: str,
        application_messages: bytes,
        canonical_token_ids: Iterable[int],
        now_ms: int | None = None,
        retention_ms: int = DEFAULT_RETENTION_MS,
        promotion_transaction: ConversationStatePromotionTransaction | None = None,
    ) -> tuple[bytes, bytes]:
        if retention_ms <= 0 or retention_ms > DEFAULT_RETENTION_MS:
            raise ValueError("conversation retention exceeds V1 bound")
        with self._lock:
            turn = self._turns.get(request_id)
            if turn is None:
                raise ConversationStateUnavailable("unknown conversation turn")
            values = tuple(receipts)
            if not values:
                raise ConversationStateUnavailable("all-role receipts are required")
            if promotion_transaction is not None:
                if promotion_transaction.request_id != str(request_id):
                    raise ConversationCheckpointInvalid(
                        "promotion transaction origin request mismatch")
                expected_receipts = tuple(sorted(
                    item.receipt_digest for item in promotion_transaction.receipts))
                actual_receipts = tuple(sorted(item.receipt_digest for item in values))
                if expected_receipts != actual_receipts:
                    raise ConversationCheckpointInvalid(
                        "promotion transaction receipt set mismatch")
            tokens = tuple(int(item) for item in canonical_token_ids)
            now = int(now_ms if now_ms is not None else time.time() * 1000)
            parent = turn["parentCheckpoint"]
            parent_epoch = int(parent.context_epoch) if parent is not None else 0
            current = self._checkpoints.get(turn["conversationId"])
            if current is not None and current.context_epoch != parent_epoch:
                raise ConversationStateConflict("conversation parent context epoch changed")
            roles = {item.role_name for item in values}
            if len(roles) != len(values):
                raise ConversationCheckpointInvalid("duplicate role receipt")
            successor = parent_epoch + 1
            expected_service = self.service_name or values[0].service_name
            expected_requester = (self.requester_identity
                                  or values[0].requester_identity)
            expected_security = (self.security_domain_digest
                                 or values[0].security_domain_digest)
            expected_model = values[0].model_digest
            if not all((expected_service, expected_requester, expected_security,
                        expected_model)):
                raise ConversationCheckpointInvalid(
                    "role receipt authorization scope is incomplete")
            if any(item.conversation_id != turn["conversationId"]
                   or item.parent_context_epoch != parent_epoch
                   or item.successor_context_epoch != successor
                   or item.origin_request_id != request_id
                   or item.origin_generation_id != str(turn["generationId"])
                   or item.service_name != expected_service
                   or item.requester_identity != expected_requester
                   or item.security_domain_digest != expected_security
                   or item.model_digest != expected_model
                   or item.plan_role_map_digest != plan_role_map_digest
                   or item.prefix_token_count != len(tokens)
                   or item.prefix_digest != _prefix_digest(tokens)
                   for item in values):
                raise ConversationCheckpointInvalid("role receipt set is not common")
            earliest = min(item.expires_at_ms for item in values)
            checkpoint = ConversationCheckpointV1(
                conversation_id=turn["conversationId"],
                parent_context_epoch=parent_epoch,
                context_epoch=successor,
                service_name=expected_service,
                requester_identity=expected_requester,
                security_domain_digest=expected_security,
                model_contract_digest=model_contract_digest,
                plan_role_map_digest=plan_role_map_digest,
                logical_prefix_digest=_prefix_digest(tokens),
                prefix_token_count=len(tokens),
                role_receipt_digests={item.role_name: item.receipt_digest for item in values},
                issued_at_ms=now,
                expires_at_ms=min(earliest, now + retention_ms),
            ).sign(self.signer_key)
            transcript = ConversationTranscriptRecordV1(
                conversation_id=turn["conversationId"],
                context_epoch=successor,
                requester_identity=checkpoint.requester_identity,
                service_name=checkpoint.service_name,
                security_domain_digest=checkpoint.security_domain_digest,
                application_messages=bytes(application_messages),
                tokenizer_digest=tokenizer_digest,
                chat_template_digest=chat_template_digest,
                canonical_token_ids=tokens,
                prefix_digest=checkpoint.logical_prefix_digest,
                prefix_token_count=len(tokens),
                provider_role_receipts=tuple(
                    json.dumps(item.to_dict(), sort_keys=True,
                               separators=(",", ":")).encode()
                    for item in values),
                checkpoint_digest=checkpoint.checkpoint_digest,
                plan_role_map_digest=plan_role_map_digest,
                created_at_ms=now,
                expires_at_ms=checkpoint.expires_at_ms,
            )
            # The Provider ownership move and protected journal append are one
            # transaction boundary.  Staging kept every request-local owner
            # intact; if either step fails, rollback leaves the old parent and
            # all request-local candidates usable.
            try:
                if promotion_transaction is not None:
                    promotion_transaction.commit(
                        checkpoint_digest=checkpoint.checkpoint_digest)
                # The in-memory swap is performed only after every object
                # validates. Journal append is the durable owner; an append
                # failure leaves the previous checkpoint untouched.
                if self.journal is not None:
                    if not getattr(self.journal, "has_envelope_key", False):
                        raise ConversationStateUnavailable(
                            "conversation persistence requires an envelope key")
                    envelope_id = self._journal_envelope_id(
                        checkpoint.conversation_id, checkpoint.context_epoch)
                    payload = _canonical_payload({
                        "checkpoint": base64.b64encode(checkpoint.to_bytes()).decode(),
                        "transcript": transcript.to_dict(),
                    })
                    prepared = self.journal.prepare_envelope(
                        envelope_id, payload, expires_at_ms=checkpoint.expires_at_ms)
                    self.journal.commit_prepared_envelope(prepared, (
                        ("conversation-checkpoint", {
                            "conversationId": checkpoint.conversation_id,
                            "contextEpoch": checkpoint.context_epoch,
                            "checkpointDigest": checkpoint.checkpoint_digest,
                            "envelopeId": envelope_id,
                            "wireDigest": prepared.wire_digest,
                            "payloadDigest": "sha256:" + hashlib.sha256(payload).hexdigest(),
                            "expiresAtMs": checkpoint.expires_at_ms,
                        }),
                    ))
            except BaseException:
                if promotion_transaction is not None:
                    promotion_transaction.rollback()
                raise
            self._checkpoints[turn["conversationId"]] = checkpoint
            self._transcripts[turn["conversationId"]] = transcript
            self._turns.pop(request_id, None)
            return bytes(result_payload), checkpoint.to_bytes()

    def checkpoint(self, conversation_id: str) -> bytes | None:
        with self._lock:
            checkpoint = self._checkpoints.get(conversation_id)
            return checkpoint.to_bytes() if checkpoint is not None else None

    def transcript(self, conversation_id: str) -> ConversationTranscriptRecordV1 | None:
        with self._lock:
            return self._transcripts.get(conversation_id)

    def _validate_parent(self, parent: ConversationCheckpointV1,
                         continuation: ConversationContinuation) -> None:
        if (parent.conversation_id != continuation.conversation_id
                or parent.context_epoch != continuation.expected_parent_context_epoch
                or not any(parent.verify(key)
                           for key in self._verification_keys)
                or parent.expires_at_ms <= int(time.time() * 1000)):
            raise ConversationCheckpointInvalid("parent checkpoint authentication/epoch failed")


__all__ = [
    "ConversationError", "ConversationCheckpointInvalid",
    "ConversationStateUnavailable", "ConversationStateConflict",
    "ConversationInputMode", "ResidencyTier", "StateLifecycle",
    "ConversationContinuation", "ProviderConversationStateReceiptV1",
    "ConversationStateReferenceV1", "ConversationTurnBindingV1",
    "ConversationCheckpointV1", "ConversationTranscriptRecordV1",
    "ConversationStateEntryV1", "ConversationStateMetrics",
    "ProviderConversationStateManager", "ConversationStatePromotionTransaction",
    "ConversationCoordinator",
    "MAX_CHECKPOINT_FINALIZE_TOKENS", "DEFAULT_RETENTION_MS",
]
