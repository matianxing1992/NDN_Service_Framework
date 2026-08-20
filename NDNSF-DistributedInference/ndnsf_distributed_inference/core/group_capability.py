"""Requester-sealed cross-Provider ``NDNSF_DATA_V1`` capability wire.

The byte encoding is intentionally identical to the C++
``ndnsf::di::GroupCapabilityV1`` contract.  Model-specific planning remains
outside this module; callers provide the already sealed member and operation
projection plus a Provider-specific epoch-key wrapping function.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import hmac
import secrets
from types import MappingProxyType
from typing import Callable, Mapping, Sequence


_KEY_BYTES = 32
_MAX_STRING_BYTES = 1 << 20
_MAX_WIRE_BYTES = 16 << 20
_MAX_ITEMS = 1 << 20


def _u64(value: int) -> bytes:
    value = int(value)
    if value < 0 or value >= 1 << 64:
        raise ValueError("GroupCapabilityV1 integer is outside uint64")
    return value.to_bytes(8, "big")


def _text(value: str, field: str) -> bytes:
    encoded = str(value).encode("utf-8")
    if not encoded or len(encoded) > _MAX_STRING_BYTES:
        raise ValueError(f"missing or oversized {field}")
    return _u64(len(encoded)) + encoded


def _bytes(value: bytes) -> bytes:
    encoded = bytes(value)
    return _u64(len(encoded)) + encoded


def _texts(values: Sequence[str], field: str) -> bytes:
    if len(values) > _MAX_ITEMS:
        raise ValueError(f"too many {field}")
    return _u64(len(values)) + b"".join(
        _text(value, field) for value in values)


@dataclass(frozen=True)
class GroupMemberV1:
    provider: str
    rank: int
    offer_digest: str
    endpoint_prefix: str

    def __post_init__(self) -> None:
        _text(self.provider, "member.provider")
        _text(self.offer_digest, "member.offerDigest")
        _text(self.endpoint_prefix, "member.endpointPrefix")
        _u64(self.rank)


@dataclass(frozen=True)
class GroupOperationV1:
    operation_index: int
    kind: str
    producer_ranks: tuple[str, ...]
    consumer_ranks: tuple[str, ...]
    tensor_layout_digest: str
    max_bytes: int
    max_segments: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "producer_ranks", tuple(self.producer_ranks))
        object.__setattr__(self, "consumer_ranks", tuple(self.consumer_ranks))
        _u64(self.operation_index)
        _text(self.kind, "operation.kind")
        _text(self.tensor_layout_digest, "operation.tensorLayoutDigest")
        _texts(self.producer_ranks, "operation.producerRank")
        _texts(self.consumer_ranks, "operation.consumerRank")
        if (self.max_bytes <= 0 or self.max_segments <= 0
                or self.max_segments > _MAX_ITEMS):
            raise ValueError("invalid GroupCapabilityV1 operation bounds")


@dataclass(frozen=True)
class GroupCapabilityV1:
    request_id: str
    attempt_id: str
    plan_digest: str
    group_id: str
    epoch: int
    ordered_members: tuple[GroupMemberV1, ...]
    permitted_operations: tuple[GroupOperationV1, ...]
    max_inflight_bytes: int
    no_progress_ms: int
    hard_deadline_ms: int
    epoch_key_id: str
    wrapped_epoch_key_by_provider: Mapping[str, bytes]
    capability_digest: str
    sealer_signature: bytes
    wrapped_epoch_key_digest_by_provider: Mapping[str, str] | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "ordered_members", tuple(self.ordered_members))
        object.__setattr__(
            self, "permitted_operations", tuple(self.permitted_operations))
        wrapped = {
            str(provider): bytes(value)
            for provider, value in self.wrapped_epoch_key_by_provider.items()
        }
        wrapped_digests = (
            {
                str(provider): str(value)
                for provider, value
                in self.wrapped_epoch_key_digest_by_provider.items()
            }
            if self.wrapped_epoch_key_digest_by_provider is not None
            else {
                provider: hashlib.sha256(value).hexdigest()
                for provider, value in wrapped.items()
            }
        )
        object.__setattr__(
            self, "wrapped_epoch_key_by_provider", MappingProxyType(wrapped))
        object.__setattr__(
            self, "wrapped_epoch_key_digest_by_provider",
            MappingProxyType(wrapped_digests))
        object.__setattr__(self, "sealer_signature", bytes(self.sealer_signature))
        self.validate()

    def validate(self) -> None:
        for field, value in (
            ("requestId", self.request_id),
            ("attemptId", self.attempt_id),
            ("planDigest", self.plan_digest),
            ("groupId", self.group_id),
            ("epochKeyId", self.epoch_key_id),
            ("capabilityDigest", self.capability_digest),
        ):
            _text(value, field)
        if (self.epoch <= 0 or not self.ordered_members
                or not self.permitted_operations
                or self.max_inflight_bytes <= 0 or self.no_progress_ms <= 0
                or self.hard_deadline_ms < self.no_progress_ms
                or not self.sealer_signature):
            raise ValueError("invalid GroupCapabilityV1 bounds or signature")
        if (len(self.ordered_members) > _MAX_ITEMS
                or len(self.permitted_operations) > _MAX_ITEMS):
            raise ValueError("too many GroupCapabilityV1 entries")
        providers = [member.provider for member in self.ordered_members]
        ranks = [member.rank for member in self.ordered_members]
        if len(set(providers)) != len(providers) or len(set(ranks)) != len(ranks):
            raise ValueError("duplicate GroupCapabilityV1 member")
        indexes = [item.operation_index for item in self.permitted_operations]
        if len(set(indexes)) != len(indexes):
            raise ValueError("duplicate GroupCapabilityV1 operation")
        member_ranks = {str(rank) for rank in ranks}
        for operation in self.permitted_operations:
            producer_ranks = set(operation.producer_ranks)
            consumer_ranks = set(operation.consumer_ranks)
            if (not producer_ranks or not consumer_ranks
                    or len(producer_ranks) != len(operation.producer_ranks)
                    or len(consumer_ranks) != len(operation.consumer_ranks)
                    or not producer_ranks.issubset(member_ranks)
                    or not consumer_ranks.issubset(member_ranks)):
                raise ValueError(
                    "GroupCapabilityV1 operation rank is not a unique member")
        if set(self.wrapped_epoch_key_digest_by_provider) != set(providers):
            raise ValueError("incomplete GroupCapabilityV1 key commitments")
        if (not self.wrapped_epoch_key_by_provider
                or not set(self.wrapped_epoch_key_by_provider).issubset(providers)):
            raise ValueError("invalid GroupCapabilityV1 key projection")
        if any(not value for value in self.wrapped_epoch_key_by_provider.values()):
            raise ValueError("missing GroupCapabilityV1 provider key")
        for provider, wrapped_key in self.wrapped_epoch_key_by_provider.items():
            expected = hashlib.sha256(wrapped_key).hexdigest()
            if self.wrapped_epoch_key_digest_by_provider[provider] != expected:
                raise ValueError("GroupCapabilityV1 key commitment mismatch")

    def canonical_bytes(self, include_digest: bool = False) -> bytes:
        output = bytearray()
        output.extend(_text("GroupCapabilityV1", "marker"))
        output.extend(_text(self.request_id, "requestId"))
        output.extend(_text(self.attempt_id, "attemptId"))
        output.extend(_text(self.plan_digest, "planDigest"))
        output.extend(_text(self.group_id, "groupId"))
        output.extend(_u64(self.epoch))
        output.extend(_u64(len(self.ordered_members)))
        for member in self.ordered_members:
            output.extend(_text(member.provider, "member.provider"))
            output.extend(_u64(member.rank))
            output.extend(_text(member.offer_digest, "member.offerDigest"))
            output.extend(_text(member.endpoint_prefix, "member.endpointPrefix"))
        output.extend(_u64(len(self.permitted_operations)))
        for operation in self.permitted_operations:
            output.extend(_u64(operation.operation_index))
            output.extend(_text(operation.kind, "operation.kind"))
            output.extend(_texts(
                operation.producer_ranks, "operation.producerRank"))
            output.extend(_texts(
                operation.consumer_ranks, "operation.consumerRank"))
            output.extend(_text(
                operation.tensor_layout_digest, "operation.tensorLayoutDigest"))
            output.extend(_u64(operation.max_bytes))
            output.extend(_u64(operation.max_segments))
        output.extend(_u64(self.max_inflight_bytes))
        output.extend(_u64(self.no_progress_ms))
        output.extend(_u64(self.hard_deadline_ms))
        output.extend(_text(self.epoch_key_id, "epochKeyId"))
        output.extend(_u64(len(self.wrapped_epoch_key_digest_by_provider)))
        for provider in sorted(self.wrapped_epoch_key_digest_by_provider):
            output.extend(_text(provider, "wrapped.provider"))
            output.extend(_text(
                self.wrapped_epoch_key_digest_by_provider[provider],
                "wrapped.digest"))
        if include_digest:
            output.extend(_text(self.capability_digest, "capabilityDigest"))
        return bytes(output)

    def to_bytes(self) -> bytes:
        projection = bytearray(self.canonical_bytes(True))
        projection.extend(_u64(len(self.wrapped_epoch_key_by_provider)))
        for provider in sorted(self.wrapped_epoch_key_by_provider):
            projection.extend(_text(provider, "wrapped.provider"))
            projection.extend(_bytes(self.wrapped_epoch_key_by_provider[provider]))
        wire = bytes(projection) + _bytes(self.sealer_signature)
        if len(wire) > _MAX_WIRE_BYTES:
            raise ValueError("GroupCapabilityV1 wire exceeds bound")
        return wire

    def project_for_provider(self, provider: str) -> "GroupCapabilityV1":
        """Return the signed capability with only ``provider``'s envelope."""

        provider = str(provider)
        if provider not in self.wrapped_epoch_key_digest_by_provider:
            raise ValueError("Provider is not a GroupCapabilityV1 member")
        wrapped = self.wrapped_epoch_key_by_provider.get(provider)
        if wrapped is None:
            raise ValueError("Provider envelope is unavailable for projection")
        return GroupCapabilityV1(
            **{
                **self.__dict__,
                "wrapped_epoch_key_by_provider": {provider: wrapped},
                "wrapped_epoch_key_digest_by_provider": dict(
                    self.wrapped_epoch_key_digest_by_provider),
            }
        )


def seal_group_capability_v1(
    *,
    request_id: str,
    attempt_id: str,
    plan_digest: str,
    group_id: str,
    epoch: int,
    ordered_members: Sequence[GroupMemberV1],
    permitted_operations: Sequence[GroupOperationV1],
    max_inflight_bytes: int,
    no_progress_ms: int,
    hard_deadline_ms: int,
    wrap_epoch_key: Callable[[str, bytes], bytes],
    random_bytes: Callable[[int], bytes] = secrets.token_bytes,
) -> GroupCapabilityV1:
    """Seal one request-scoped capability without exposing the epoch key."""

    if not callable(wrap_epoch_key) or not callable(random_bytes):
        raise TypeError("GroupCapabilityV1 sealer callbacks must be callable")
    members = tuple(ordered_members)
    operations = tuple(permitted_operations)
    providers = [member.provider for member in members]
    if len(set(providers)) != len(providers):
        raise ValueError("duplicate GroupCapabilityV1 member")
    epoch_key = bytearray(random_bytes(_KEY_BYTES))
    if len(epoch_key) != _KEY_BYTES:
        raise ValueError("epoch key generator returned the wrong size")
    try:
        wrapped = {
            provider: bytes(wrap_epoch_key(provider, bytes(epoch_key)))
            for provider in providers
        }
        wrapped_digests = {
            provider: hashlib.sha256(value).hexdigest()
            for provider, value in wrapped.items()
        }
        epoch_key_id = hashlib.sha256(epoch_key).hexdigest()
        unsigned = GroupCapabilityV1(
            request_id=request_id,
            attempt_id=attempt_id,
            plan_digest=plan_digest,
            group_id=group_id,
            epoch=epoch,
            ordered_members=members,
            permitted_operations=operations,
            max_inflight_bytes=max_inflight_bytes,
            no_progress_ms=no_progress_ms,
            hard_deadline_ms=hard_deadline_ms,
            epoch_key_id=epoch_key_id,
            wrapped_epoch_key_by_provider=wrapped,
            capability_digest="pending",
            sealer_signature=b"pending",
            wrapped_epoch_key_digest_by_provider=wrapped_digests,
        )
        capability_digest = hashlib.sha256(
            unsigned.canonical_bytes(False)).hexdigest()
        digest_bound = GroupCapabilityV1(
            **{
                **unsigned.__dict__,
                "capability_digest": capability_digest,
                "sealer_signature": b"pending",
            }
        )
        signature = hmac.new(
            epoch_key, digest_bound.canonical_bytes(True), hashlib.sha256
        ).digest()
        return GroupCapabilityV1(
            **{
                **digest_bound.__dict__,
                "sealer_signature": signature,
            }
        )
    finally:
        for index in range(len(epoch_key)):
            epoch_key[index] = 0


__all__ = [
    "GroupCapabilityV1",
    "GroupMemberV1",
    "GroupOperationV1",
    "seal_group_capability_v1",
]
