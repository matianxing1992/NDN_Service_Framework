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
import os
from typing import Mapping


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
            if any(item.disposition == TensorDisposition.SHARDED for item in slices):
                ranges = sorted((item.begin, item.end) for item in slices
                                if item.disposition == TensorDisposition.SHARDED)
                if any(end > begin for (begin, end), (next_begin, _)
                       in zip(ranges, ranges[1:]) if end > next_begin):
                    raise ValueError("overlapping sharded tensor ranges")


@dataclass(frozen=True)
class RedistributionEdge:
    producer_ranks: tuple[int, ...]
    consumer_ranks: tuple[int, ...]
    tensor: str
    operation: str
    epoch: str
    integrity_digest: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "producer_ranks", tuple(self.producer_ranks))
        object.__setattr__(self, "consumer_ranks", tuple(self.consumer_ranks))
        if (not self.producer_ranks or not self.consumer_ranks or not self.tensor
                or self.operation not in {"GATHER", "SCATTER", "RESHARD"}
                or not self.epoch or not self.integrity_digest.startswith("sha256:")):
            raise ValueError("invalid redistribution edge")


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

    @property
    def rank_count(self) -> int:
        return sum(self.tensor_degrees)


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
    "RedistributionEdge", "HybridPlan", "DataSegmentV1", "DataSegmentReplayWindow",
]
