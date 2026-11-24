"""Stateless conversation wire codecs for reference and historical journal reads.

Production request, checkpoint and residency lifecycles belong to the native
Runtime. These value codecs retain exact signed wire representations without
creating or advancing a conversation owner.
"""

from __future__ import annotations

import base64
from dataclasses import dataclass, replace
from enum import Enum
import hashlib
import hmac
import json
from typing import Any, Iterable, Mapping


SCHEMA_CHECKPOINT = "ndnsf-di-conversation-checkpoint-v1"
SCHEMA_TRANSCRIPT = "ndnsf-di-conversation-transcript-v1"
SCHEMA_RECEIPT = "ndnsf-di-provider-conversation-receipt-v1"
SCHEMA_STATE_REFERENCE = "ndnsf-di-conversation-state-reference-v1"


class ConversationError(RuntimeError):
    """Base class for fail-closed continuation errors."""


class ConversationCheckpointInvalid(ConversationError):
    pass


class ConversationInputMode(str, Enum):
    FULL_CONTEXT = "full-context"
    APPEND_DELTA = "append-delta"


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
    def from_transport_dict(cls, value: Mapping[str, Any]) -> "ProviderConversationStateReceiptV1":
        """Expand the compact collaboration representation before canonical decoding."""
        packed = value.get("stateComponentDigests") if isinstance(value, Mapping) else None
        if isinstance(packed, str):
            try:
                raw = base64.b64decode(packed, validate=True)
            except ValueError as exc:
                raise ConversationCheckpointInvalid("Provider receipt digests are malformed") from exc
            digest_size = hashlib.sha256().digest_size
            if not raw or len(raw) % digest_size or base64.b64encode(raw).decode("ascii") != packed:
                raise ConversationCheckpointInvalid("Provider receipt digests are malformed")
            value = {**value, "stateComponentDigests": [
                "sha256:" + raw[offset:offset + digest_size].hex()
                for offset in range(0, len(raw), digest_size)
            ]}
        return cls.from_dict(value)

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


__all__ = [
    "ConversationError",
    "ConversationCheckpointInvalid",
    "ConversationInputMode",
    "ConversationContinuation",
    "ProviderConversationStateReceiptV1",
    "ConversationStateReferenceV1",
    "ConversationTurnBindingV1",
    "ConversationCheckpointV1",
    "ConversationTranscriptRecordV1",
]
