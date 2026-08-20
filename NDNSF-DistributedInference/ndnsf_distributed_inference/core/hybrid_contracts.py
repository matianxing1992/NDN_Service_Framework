"""Model-neutral tensor-group and NDNSF_DATA_V1 contracts.

Adapters certify tensor layout; NDNSF-DI owns rank/epoch/replay and failure
semantics.  This module intentionally carries metadata and encrypted segment
envelopes only; it never chooses a model-specific split.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import hashlib
import hmac
import json
import os
from typing import Any, Iterable, Mapping


NDNSF_DATA_V1 = "NDNSF_DATA_V1"


class TensorDisposition(str, Enum):
    SHARDED = "SHARDED"
    REPLICATED = "REPLICATED"
    OWNER_ONLY = "OWNER_ONLY"
    LOCAL_DERIVED = "LOCAL_DERIVED"


@dataclass(frozen=True)
class TensorSlice:
    tensor: str
    rank: int
    disposition: TensorDisposition | str
    axis: int | None = None
    begin: int = 0
    end: int = 0
    layout: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "disposition", TensorDisposition(self.disposition))
        if (not self.tensor or self.rank < 0 or not self.layout
                or self.begin < 0 or self.end < self.begin):
            raise ValueError("invalid tensor slice")
        if self.disposition == TensorDisposition.SHARDED:
            if self.axis is None or self.end <= self.begin:
                raise ValueError("sharded tensor slice requires a non-empty axis range")
        elif self.axis is not None and self.axis < 0:
            raise ValueError("tensor axis must be non-negative")


@dataclass(frozen=True)
class LocalTensorGroup:
    role: str
    epoch: str
    participants: tuple[str, ...]
    tensors: tuple[TensorSlice, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "participants", tuple(self.participants))
        object.__setattr__(self, "tensors", tuple(self.tensors))
        if (not self.role or not self.epoch or not self.participants
                or len(set(self.participants)) != len(self.participants)
                or not self.tensors):
            raise ValueError("local tensor group is incomplete")
        ranks = {item.rank for item in self.tensors}
        if ranks != set(range(len(self.participants))):
            raise ValueError("tensor group rank cover is incomplete")
        for tensor in {item.tensor for item in self.tensors}:
            slices = [item for item in self.tensors if item.tensor == tensor]
            dispositions = {item.disposition for item in slices}
            if len(dispositions) != 1:
                raise ValueError("one tensor cannot mix disposition contracts")
            disposition = next(iter(dispositions))
            slice_ranks = tuple(item.rank for item in slices)
            if len(set(slice_ranks)) != len(slice_ranks):
                raise ValueError("tensor contains a duplicate rank slice")
            if disposition == TensorDisposition.SHARDED:
                if set(slice_ranks) != set(range(len(self.participants))):
                    raise ValueError("sharded tensor rank cover is incomplete")
                axes = {item.axis for item in slices}
                if len(axes) != 1:
                    raise ValueError("sharded tensor axes are inconsistent")
                ranges = sorted((item.begin, item.end) for item in slices)
                if ranges[0][0] != 0 or any(
                        end != next_begin
                        for (_begin, end), (next_begin, _next_end)
                        in zip(ranges, ranges[1:])):
                    raise ValueError("sharded tensor ranges must be contiguous")
            elif disposition in {
                    TensorDisposition.REPLICATED,
                    TensorDisposition.LOCAL_DERIVED,
            } and set(slice_ranks) != set(range(len(self.participants))):
                raise ValueError("replicated/local tensor rank cover is incomplete")
            elif disposition == TensorDisposition.OWNER_ONLY and len(slices) != 1:
                raise ValueError("owner-only tensor must have exactly one owner")
            if disposition == TensorDisposition.SHARDED:
                if any(end > next_begin
                       for (_begin, end), (next_begin, _next_end)
                       in zip(ranges, ranges[1:])):
                    raise ValueError("overlapping sharded tensor ranges")


@dataclass(frozen=True)
class RedistributionEdge:
    producer_ranks: tuple[int, ...]
    consumer_ranks: tuple[int, ...]
    tensor: str
    operation: str
    epoch: str
    integrity_digest: str
    source_layout_digest: str
    target_layout_digest: str
    temporary_memory_bytes: int
    complete_output: bool = True
    axis: int = -1

    def __post_init__(self) -> None:
        object.__setattr__(self, "producer_ranks", tuple(self.producer_ranks))
        object.__setattr__(self, "consumer_ranks", tuple(self.consumer_ranks))
        valid_operation_for_rank_shape = (
            (len(self.producer_ranks) == 1 and len(self.consumer_ranks) > 1
             and self.operation == "SCATTER")
            or (len(self.producer_ranks) > 1 and len(self.consumer_ranks) == 1
                and self.operation == "GATHER")
            or (len(self.producer_ranks) > 1 and len(self.consumer_ranks) > 1
                and self.operation == "RESHARD")
            or (len(self.producer_ranks) == 1 and len(self.consumer_ranks) == 1
                and self.source_layout_digest != self.target_layout_digest
                and self.operation == "RESHARD")
        )
        if not valid_operation_for_rank_shape:
            raise ValueError("redistribution operation does not match rank shape")
        if (not self.producer_ranks or not self.consumer_ranks or not self.tensor
                or self.operation not in {"GATHER", "SCATTER", "RESHARD"}
                or not self.epoch or len(self.integrity_digest) != 71
                or not self.integrity_digest.startswith("sha256:")
                or len(self.source_layout_digest) != 71
                or not self.source_layout_digest.startswith("sha256:")
                or len(self.target_layout_digest) != 71
                or not self.target_layout_digest.startswith("sha256:")
                or self.temporary_memory_bytes < 0
                or self.complete_output is not True
                or not isinstance(self.axis, int)
                or self.axis < -16 or self.axis >= 16):
            raise ValueError("invalid redistribution edge")
        try:
            int(self.integrity_digest[7:], 16)
            int(self.source_layout_digest[7:], 16)
            int(self.target_layout_digest[7:], 16)
        except ValueError as exc:
            raise ValueError("invalid redistribution edge digest") from exc
        if (len(set(self.producer_ranks)) != len(self.producer_ranks)
                or len(set(self.consumer_ranks)) != len(self.consumer_ranks)
                or set(self.producer_ranks) & set(self.consumer_ranks)
                or any(rank < 0 for rank in (
                    *self.producer_ranks, *self.consumer_ranks))):
            raise ValueError("invalid redistribution rank set")


@dataclass(frozen=True)
class HybridPlan:
    stages: int
    tensor_degrees: tuple[int, ...]
    rank_labels: tuple[str, ...]
    redistributions: tuple[RedistributionEdge, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "tensor_degrees", tuple(self.tensor_degrees))
        object.__setattr__(self, "rank_labels", tuple(self.rank_labels))
        if (self.stages <= 0 or len(self.tensor_degrees) != self.stages
                or any(int(value) < 1 for value in self.tensor_degrees)
                or len(self.rank_labels) != sum(self.tensor_degrees)
                or len(set(self.rank_labels)) != len(self.rank_labels)):
            raise ValueError("hybrid plan rank/stage cover is incomplete")
        object.__setattr__(self, "redistributions", tuple(self.redistributions))
        expected_labels = tuple(
            f"S{stage}R{rank}"
            for stage, degree in enumerate(self.tensor_degrees)
            for rank in range(degree)
        )
        if self.rank_labels != expected_labels:
            raise ValueError("hybrid plan rank labels are not canonical")

        offsets = []
        rank_stage = {}
        cursor = 0
        for stage, degree in enumerate(self.tensor_degrees):
            offsets.append(cursor)
            for rank in range(cursor, cursor + degree):
                rank_stage[rank] = stage
            cursor += degree

        seen_edges = set()
        boundary_edges: dict[int, list[RedistributionEdge]] = {}
        for edge in self.redistributions:
            if any(rank not in rank_stage for rank in (
                    *edge.producer_ranks, *edge.consumer_ranks)):
                raise ValueError("redistribution references an unknown rank")
            producer_stages = {rank_stage[rank] for rank in edge.producer_ranks}
            consumer_stages = {rank_stage[rank] for rank in edge.consumer_ranks}
            if len(producer_stages) != 1 or len(consumer_stages) != 1:
                raise ValueError("redistribution mixes stage rank sets")
            producer_stage = next(iter(producer_stages))
            consumer_stage = next(iter(consumer_stages))
            if consumer_stage != producer_stage + 1:
                raise ValueError("redistribution must follow one adjacent stage boundary")
            expected_producers = set(range(
                offsets[producer_stage],
                offsets[producer_stage] + self.tensor_degrees[producer_stage]))
            expected_consumers = set(range(
                offsets[consumer_stage],
                offsets[consumer_stage] + self.tensor_degrees[consumer_stage]))
            if (set(edge.producer_ranks) != expected_producers
                    or set(edge.consumer_ranks) != expected_consumers):
                raise ValueError("redistribution rank cover is incomplete")
            identity = (
                edge.producer_ranks, edge.consumer_ranks, edge.tensor,
                edge.operation, edge.epoch, edge.integrity_digest)
            if identity in seen_edges:
                raise ValueError("duplicate redistribution edge")
            seen_edges.add(identity)
            boundary_edges.setdefault(producer_stage, []).append(edge)

        for stage, (producer_degree, consumer_degree) in enumerate(zip(
                self.tensor_degrees, self.tensor_degrees[1:])):
            if producer_degree != consumer_degree and not boundary_edges.get(stage):
                raise ValueError("degree-changing boundary omits redistribution")

    @property
    def rank_count(self) -> int:
        return sum(self.tensor_degrees)

    def canonical_dict(self) -> dict[str, object]:
        return {
            "stages": self.stages,
            "tensor_degrees": list(self.tensor_degrees),
            "rank_labels": list(self.rank_labels),
            "redistributions": [
                {
                    "producer_ranks": list(edge.producer_ranks),
                    "consumer_ranks": list(edge.consumer_ranks),
                    "tensor": edge.tensor,
                    "operation": edge.operation,
                    "epoch": edge.epoch,
                    "integrity_digest": edge.integrity_digest,
                    "source_layout_digest": edge.source_layout_digest,
                    "target_layout_digest": edge.target_layout_digest,
                    "temporary_memory_bytes": edge.temporary_memory_bytes,
                    "complete_output": edge.complete_output,
                    "axis": edge.axis,
                }
                for edge in self.redistributions
            ],
        }

    def digest(self) -> str:
        encoded = json.dumps(
            self.canonical_dict(), sort_keys=True, separators=(",", ":"),
        ).encode("utf-8")
        return "sha256:" + hashlib.sha256(encoded).hexdigest()


def validate_role_dataflow_contracts(
    roles: Iterable[Any],
    contracts: Iterable[Any],
) -> None:
    """Validate the sealed, model-neutral role/dataflow graph.

    The immutable SDK values live above the core layer, so this validator uses
    their public data attributes instead of importing SDK classes.  Keeping the
    graph-wide invariant here gives every Python entry point one authoritative
    implementation without creating a core-to-SDK dependency cycle.
    """

    roles = tuple(roles)
    contracts = tuple(contracts)
    role_ids = tuple(str(role.role_id) for role in roles)
    contract_roles = tuple(str(contract.role) for contract in contracts)
    if (not roles or len(set(role_ids)) != len(role_ids)
            or set(contract_roles) != set(role_ids)
            or len(set(contract_roles)) != len(contract_roles)):
        raise ValueError("role/dataflow cover is incomplete or duplicated")
    if sum(bool(item.terminal_response_owner) for item in contracts) != 1:
        raise ValueError("exactly one role must own the terminal Response")

    publishers: dict[str, list[str]] = {}
    edges: dict[str, set[str]] = {role: set() for role in role_ids}
    incoming: dict[str, int] = {role: 0 for role in role_ids}
    for contract in contracts:
        for endpoint in tuple(contract.may_publish):
            publishers.setdefault(
                str(endpoint.endpoint_digest), []).append(str(contract.role))
    for contract in contracts:
        for endpoint in tuple(contract.must_fetch):
            source_kind = getattr(endpoint.source_kind, "value", endpoint.source_kind)
            if str(source_kind) == "APPLICATION_INPUT":
                continue
            owners = publishers.get(str(endpoint.endpoint_digest), ())
            producer_role = str(endpoint.producer_role)
            if owners != [producer_role]:
                raise ValueError(
                    "mustFetch endpoint requires exactly one matching mayPublish")
            if producer_role not in edges:
                raise ValueError(
                    "tensor endpoint references an unknown producer role")
            consumer_role = str(contract.role)
            if consumer_role not in edges[producer_role]:
                edges[producer_role].add(consumer_role)
                incoming[consumer_role] += 1

    ready = sorted(role for role, degree in incoming.items() if degree == 0)
    visited = 0
    while ready:
        role = ready.pop(0)
        visited += 1
        for consumer in sorted(edges[role]):
            incoming[consumer] -= 1
            if incoming[consumer] == 0:
                ready.append(consumer)
                ready.sort()
    if visited != len(role_ids):
        raise ValueError("role dataflow contains a cycle")


@dataclass(frozen=True)
class DataSegmentV1:
    operation_id: str
    epoch: str
    producer: str
    consumer: str
    segment_no: int
    payload: bytes
    key: bytes
    nonce: bytes
    aad: bytes
    mac: str = ""

    def __post_init__(self) -> None:
        if (not self.operation_id or not self.epoch or not self.producer
                or not self.consumer or self.segment_no < 0 or not self.payload
                or len(self.key) < 16 or len(self.nonce) < 12 or len(self.nonce) > 32):
            raise ValueError("invalid NDNSF_DATA_V1 segment")
        expected = hmac.new(
            self.key, self.operation_id.encode() + b"|" + self.epoch.encode()
            + b"|" + self.segment_no.to_bytes(8, "big") + self.nonce
            + self.aad + self.payload,
            hashlib.sha256).hexdigest()
        if self.mac and not hmac.compare_digest(self.mac, expected):
            raise ValueError("NDNSF_DATA_V1 segment MAC mismatch")
        object.__setattr__(self, "mac", expected)

    @classmethod
    def create(cls, *, operation_id: str, epoch: str, producer: str,
               consumer: str, segment_no: int, payload: bytes, key: bytes,
               aad: bytes = b"") -> "DataSegmentV1":
        return cls(operation_id, epoch, producer, consumer, segment_no,
                   bytes(payload), bytes(key), os.urandom(16), bytes(aad))

    def verify(self, key: bytes) -> None:
        if not hmac.compare_digest(self.mac, DataSegmentV1(
                self.operation_id, self.epoch, self.producer, self.consumer,
                self.segment_no, self.payload, bytes(key), self.nonce,
                self.aad).mac):
            raise ValueError("NDNSF_DATA_V1 replay/integrity verification failed")


class DataSegmentReplayWindow:
    """Bounded duplicate/replay guard for one operation epoch."""

    def __init__(self, *, operation_id: str, epoch: str, max_segments: int = 4096):
        if not operation_id or not epoch or max_segments <= 0:
            raise ValueError("invalid DATA replay window")
        self.operation_id = operation_id
        self.epoch = epoch
        self.max_segments = int(max_segments)
        self._seen: set[tuple[int, bytes]] = set()

    def accept(self, segment: DataSegmentV1, *, key: bytes) -> None:
        if segment.operation_id != self.operation_id or segment.epoch != self.epoch:
            raise ValueError("DATA segment epoch binding mismatch")
        segment.verify(key)
        identity = (segment.segment_no, bytes(segment.nonce))
        if identity in self._seen:
            raise ValueError("duplicate or replayed DATA segment")
        if len(self._seen) >= self.max_segments:
            raise RuntimeError("DATA replay window is full")
        self._seen.add(identity)


__all__ = [
    "NDNSF_DATA_V1", "TensorDisposition", "TensorSlice", "LocalTensorGroup",
    "RedistributionEdge", "HybridPlan", "validate_role_dataflow_contracts",
    "DataSegmentV1", "DataSegmentReplayWindow",
]
