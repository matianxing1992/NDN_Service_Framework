"""Reusable NDNSF streaming substrate.

This module keeps app-neutral stream/session/chunk behavior in the NDNSF Python
core layer. Applications still own codecs, camera capture, tensor formats,
decoder queues, and any application-specific FEC repair algorithm.
"""

from __future__ import annotations

import json
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass, field, replace
from typing import Any, Deque, Iterable, Optional


STREAM_CHUNK_MAGIC = b"NDS1"
STREAM_HEADER_STRUCT = struct.Struct("!4sI")


def stream_now_ms() -> int:
    return int(time.time() * 1000)


def _clean_metadata(value: Optional[dict[str, Any]]) -> dict[str, Any]:
    return dict(value or {})


def _to_int_tuple(value: Iterable[int] | None) -> tuple[int, ...]:
    if value is None:
        return ()
    return tuple(int(item) for item in value)


@dataclass(frozen=True)
class StreamFecInfo:
    """Codec-neutral FEC metadata attached to a stream chunk.

    The core records the symbol layout, but it does not implement the repair
    codec. A video application may use XOR parity; another application may use a
    stronger code while keeping the same generic metadata shape.
    """

    scheme: str = ""
    data_shards: int = 0
    parity_shards: int = 0
    symbol_index: int = 0
    symbol_count: int = 0
    data_lengths: tuple[int, ...] = ()
    source_block_id: str = ""
    repair_symbol: bool = False
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "scheme": self.scheme,
            "dataShards": int(self.data_shards),
            "parityShards": int(self.parity_shards),
            "symbolIndex": int(self.symbol_index),
            "symbolCount": int(self.symbol_count),
            "dataLengths": [int(item) for item in self.data_lengths],
            "sourceBlockId": self.source_block_id,
            "repairSymbol": bool(self.repair_symbol),
            "metadata": _clean_metadata(self.metadata),
        }

    @classmethod
    def from_dict(cls, value: Optional[dict[str, Any]]) -> Optional["StreamFecInfo"]:
        if not value:
            return None
        return cls(
            scheme=str(value.get("scheme", "")),
            data_shards=int(value.get("dataShards", value.get("data_shards", 0)) or 0),
            parity_shards=int(value.get("parityShards", value.get("parity_shards", 0)) or 0),
            symbol_index=int(value.get("symbolIndex", value.get("symbol_index", 0)) or 0),
            symbol_count=int(value.get("symbolCount", value.get("symbol_count", 0)) or 0),
            data_lengths=_to_int_tuple(value.get("dataLengths", value.get("data_lengths", ()))),
            source_block_id=str(value.get("sourceBlockId", value.get("source_block_id", ""))),
            repair_symbol=bool(value.get("repairSymbol", value.get("repair_symbol", False))),
            metadata=_clean_metadata(value.get("metadata")),
        )

    @property
    def enabled(self) -> bool:
        return self.data_shards > 0 or self.parity_shards > 0 or self.symbol_count > 0


@dataclass(frozen=True)
class StreamInfo:
    """Description returned by a stream start/control service."""

    stream_id: str
    session_epoch: int
    stream_prefix: str
    next_seq: int = 0
    content_type: str = "application/octet-stream"
    freshness_ms: int = 80
    max_payload_bytes: int = 3600
    window: int = 32
    lookahead: int = 8
    interest_lifetime_ms: int = 500
    missing_timeout_ms: int = 300
    reliability: str = "best-effort"
    created_ms: int = field(default_factory=stream_now_ms)
    metadata: dict[str, Any] = field(default_factory=dict)

    def chunk_name(self, seq: int) -> str:
        return f"{self.stream_prefix.rstrip('/')}/{int(seq)}"

    def to_dict(self) -> dict[str, Any]:
        return {
            "streamId": self.stream_id,
            "sessionEpoch": int(self.session_epoch),
            "streamPrefix": self.stream_prefix,
            "nextSeq": int(self.next_seq),
            "contentType": self.content_type,
            "freshnessMs": int(self.freshness_ms),
            "maxPayloadBytes": int(self.max_payload_bytes),
            "window": int(self.window),
            "lookahead": int(self.lookahead),
            "interestLifetimeMs": int(self.interest_lifetime_ms),
            "missingTimeoutMs": int(self.missing_timeout_ms),
            "reliability": self.reliability,
            "createdMs": int(self.created_ms),
            "metadata": _clean_metadata(self.metadata),
        }

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "StreamInfo":
        return cls(
            stream_id=str(value.get("streamId", value.get("stream_id", ""))),
            session_epoch=int(value.get("sessionEpoch", value.get("session_epoch", 0)) or 0),
            stream_prefix=str(value.get("streamPrefix", value.get("stream_prefix", ""))),
            next_seq=int(value.get("nextSeq", value.get("next_seq", 0)) or 0),
            content_type=str(value.get("contentType", value.get("content_type", "application/octet-stream"))),
            freshness_ms=int(value.get("freshnessMs", value.get("freshness_ms", 80)) or 80),
            max_payload_bytes=int(value.get("maxPayloadBytes", value.get("max_payload_bytes", 3600)) or 3600),
            window=int(value.get("window", 32) or 32),
            lookahead=int(value.get("lookahead", 8) or 8),
            interest_lifetime_ms=int(value.get("interestLifetimeMs", value.get("interest_lifetime_ms", 500)) or 500),
            missing_timeout_ms=int(value.get("missingTimeoutMs", value.get("missing_timeout_ms", 300)) or 300),
            reliability=str(value.get("reliability", "best-effort")),
            created_ms=int(value.get("createdMs", value.get("created_ms", stream_now_ms())) or 0),
            metadata=_clean_metadata(value.get("metadata")),
        )


@dataclass(frozen=True)
class StreamChunk:
    """One app-neutral stream chunk plus opaque application payload bytes."""

    stream_id: str
    session_epoch: int
    seq: int
    payload: bytes
    content_type: str = "application/octet-stream"
    capture_ms: int = 0
    arrival_ms: int = 0
    deadline_ms: int = 0
    key_chunk: bool = False
    frame_id: int = 0
    frame_first_seq: int = 0
    frame_last_seq: int = 0
    segment_index: int = 0
    segment_count: int = 1
    fec: Optional[StreamFecInfo] = None
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_dict(self, *, include_payload: bool = False) -> dict[str, Any]:
        result: dict[str, Any] = {
            "streamId": self.stream_id,
            "sessionEpoch": int(self.session_epoch),
            "seq": int(self.seq),
            "contentType": self.content_type,
            "captureMs": int(self.capture_ms),
            "arrivalMs": int(self.arrival_ms),
            "deadlineMs": int(self.deadline_ms),
            "keyChunk": bool(self.key_chunk),
            "frameId": int(self.frame_id),
            "frameFirstSeq": int(self.frame_first_seq),
            "frameLastSeq": int(self.frame_last_seq),
            "segmentIndex": int(self.segment_index),
            "segmentCount": int(self.segment_count),
            "payloadSize": len(self.payload),
            "metadata": _clean_metadata(self.metadata),
        }
        if self.fec is not None:
            result["fec"] = self.fec.to_dict()
        if include_payload:
            result["payload"] = list(self.payload)
        return result

    @classmethod
    def from_dict(cls, value: dict[str, Any], payload: bytes = b"") -> "StreamChunk":
        if not payload and "payload" in value:
            payload = bytes(value.get("payload") or b"")
        return cls(
            stream_id=str(value.get("streamId", value.get("stream_id", ""))),
            session_epoch=int(value.get("sessionEpoch", value.get("session_epoch", 0)) or 0),
            seq=int(value.get("seq", value.get("packetSeq", 0)) or 0),
            payload=bytes(payload),
            content_type=str(value.get("contentType", value.get("content_type", "application/octet-stream"))),
            capture_ms=int(value.get("captureMs", value.get("capture_ms", 0)) or 0),
            arrival_ms=int(value.get("arrivalMs", value.get("arrival_ms", 0)) or 0),
            deadline_ms=int(value.get("deadlineMs", value.get("deadline_ms", 0)) or 0),
            key_chunk=bool(value.get("keyChunk", value.get("key_chunk", False))),
            frame_id=int(value.get("frameId", value.get("frame_id", value.get("frameSeq", 0))) or 0),
            frame_first_seq=int(value.get("frameFirstSeq", value.get("frame_first_seq", 0)) or 0),
            frame_last_seq=int(value.get("frameLastSeq", value.get("frame_last_seq", 0)) or 0),
            segment_index=int(value.get("segmentIndex", value.get("segment_index", 0)) or 0),
            segment_count=int(value.get("segmentCount", value.get("segment_count", 1)) or 1),
            fec=StreamFecInfo.from_dict(value.get("fec")),
            metadata=_clean_metadata(value.get("metadata")),
        )

    def with_arrival_ms(self, arrival_ms: Optional[int] = None) -> "StreamChunk":
        return replace(self, arrival_ms=stream_now_ms() if arrival_ms is None else int(arrival_ms))


def encode_stream_chunk(chunk: StreamChunk) -> bytes:
    """Encode a stream chunk as magic + header length + JSON header + payload."""

    header = json.dumps(
        chunk.to_dict(include_payload=False),
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return STREAM_HEADER_STRUCT.pack(STREAM_CHUNK_MAGIC, len(header)) + header + chunk.payload


def decode_stream_chunk(wire: bytes) -> StreamChunk:
    if len(wire) < STREAM_HEADER_STRUCT.size:
        raise ValueError("stream chunk wire is too short")
    magic, header_len = STREAM_HEADER_STRUCT.unpack(wire[:STREAM_HEADER_STRUCT.size])
    if magic != STREAM_CHUNK_MAGIC:
        raise ValueError("stream chunk magic mismatch")
    header_start = STREAM_HEADER_STRUCT.size
    header_end = header_start + int(header_len)
    if header_end > len(wire):
        raise ValueError("stream chunk header exceeds wire size")
    header = json.loads(wire[header_start:header_end].decode("utf-8"))
    payload = wire[header_end:]
    return StreamChunk.from_dict(header, payload)


@dataclass
class StreamMetrics:
    produced: int = 0
    evicted: int = 0
    received: int = 0
    emitted: int = 0
    duplicates: int = 0
    stale: int = 0
    gaps: int = 0
    timeouts: int = 0
    nacks: int = 0
    bytes_produced: int = 0
    bytes_received: int = 0

    def to_dict(self) -> dict[str, int]:
        return {
            "produced": self.produced,
            "evicted": self.evicted,
            "received": self.received,
            "emitted": self.emitted,
            "duplicates": self.duplicates,
            "stale": self.stale,
            "gaps": self.gaps,
            "timeouts": self.timeouts,
            "nacks": self.nacks,
            "bytesProduced": self.bytes_produced,
            "bytesReceived": self.bytes_received,
        }


class StreamProducerBuffer:
    """Bounded sequence-indexed buffer for recently produced stream chunks."""

    def __init__(self, max_chunks: int = 600) -> None:
        self._max_chunks = max(1, int(max_chunks))
        self._chunks: dict[int, StreamChunk] = {}
        self._order: Deque[int] = deque()
        self._metrics = StreamMetrics()
        self._lock = threading.Lock()

    @property
    def metrics(self) -> StreamMetrics:
        with self._lock:
            return replace(self._metrics)

    def put(self, chunk: StreamChunk) -> None:
        with self._lock:
            if chunk.seq not in self._chunks:
                self._order.append(chunk.seq)
            self._chunks[chunk.seq] = chunk
            self._metrics.produced += 1
            self._metrics.bytes_produced += len(chunk.payload)
            while len(self._order) > self._max_chunks:
                old_seq = self._order.popleft()
                if self._chunks.pop(old_seq, None) is not None:
                    self._metrics.evicted += 1

    def get(self, seq: int) -> Optional[StreamChunk]:
        with self._lock:
            return self._chunks.get(int(seq))

    def encoded(self, seq: int) -> Optional[bytes]:
        chunk = self.get(seq)
        return None if chunk is None else encode_stream_chunk(chunk)

    def seqs(self) -> list[int]:
        with self._lock:
            return list(self._order)

    def __len__(self) -> int:
        with self._lock:
            return len(self._chunks)


class StreamConsumerReorderBuffer:
    """Current-session reorder buffer with duplicate and stale-session guards."""

    def __init__(
        self,
        stream_id: str,
        session_epoch: int,
        *,
        next_seq: int = 0,
        max_pending: int = 512,
        history: int = 1024,
    ) -> None:
        self.stream_id = stream_id
        self.session_epoch = int(session_epoch)
        self.next_seq = int(next_seq)
        self.max_pending = max(1, int(max_pending))
        self._pending: dict[int, StreamChunk] = {}
        self._completed: set[int] = set()
        self._completed_order: Deque[int] = deque()
        self._history = max(1, int(history))
        self._metrics = StreamMetrics()
        self._lock = threading.Lock()

    @property
    def metrics(self) -> StreamMetrics:
        with self._lock:
            return replace(self._metrics)

    def reset(self, stream_id: str, session_epoch: int, *, next_seq: int = 0) -> None:
        with self._lock:
            self.stream_id = stream_id
            self.session_epoch = int(session_epoch)
            self.next_seq = int(next_seq)
            self._pending.clear()
            self._completed.clear()
            self._completed_order.clear()

    def push(self, chunk: StreamChunk) -> list[StreamChunk]:
        with self._lock:
            if chunk.stream_id != self.stream_id or chunk.session_epoch != self.session_epoch:
                self._metrics.stale += 1
                return []
            if chunk.seq < self.next_seq or chunk.seq in self._pending or chunk.seq in self._completed:
                self._metrics.duplicates += 1
                return []
            if len(self._pending) >= self.max_pending:
                self._drop_oldest_pending()
            self._pending[chunk.seq] = chunk.with_arrival_ms(chunk.arrival_ms or stream_now_ms())
            self._metrics.received += 1
            self._metrics.bytes_received += len(chunk.payload)
            emitted: list[StreamChunk] = []
            while self.next_seq in self._pending:
                ready = self._pending.pop(self.next_seq)
                emitted.append(ready)
                self._mark_completed(self.next_seq)
                self.next_seq += 1
            if not emitted and self._pending:
                self._metrics.gaps += 1
            self._metrics.emitted += len(emitted)
            return emitted

    def missing_sequences(self, *, limit: int = 32) -> list[int]:
        with self._lock:
            if not self._pending:
                return []
            highest = max(self._pending)
            return [
                seq for seq in range(self.next_seq, highest)
                if seq not in self._pending
            ][:max(0, int(limit))]

    def skip_to(self, seq: int) -> None:
        with self._lock:
            target = int(seq)
            for old in [item for item in self._pending if item < target]:
                self._pending.pop(old, None)
            self.next_seq = max(self.next_seq, target)

    def _mark_completed(self, seq: int) -> None:
        self._completed.add(seq)
        self._completed_order.append(seq)
        while len(self._completed_order) > self._history:
            self._completed.discard(self._completed_order.popleft())

    def _drop_oldest_pending(self) -> None:
        if not self._pending:
            return
        oldest = min(self._pending)
        self._pending.pop(oldest, None)
        self._metrics.stale += 1


@dataclass(frozen=True)
class StreamFetchDecision:
    window: int
    lookahead: int
    interest_lifetime_ms: int
    missing_timeout_ms: int
    pressure: float
    reason: str


@dataclass
class StreamAdaptiveFetcherState:
    """Generic adaptive fetch policy state.

    The helper is intentionally conservative. Applications may layer
    service-specific bitrate or codec decisions above this generic fetch policy.
    """

    rtt_ms: float = 100.0
    timeout_pressure: float = 0.0
    nack_pressure: float = 0.0
    duplicate_pressure: float = 0.0
    backlog_pressure: float = 0.0
    min_window: int = 4
    base_window: int = 32
    max_window: int = 256
    min_lookahead: int = 2
    base_lookahead: int = 8
    max_lookahead: int = 128
    min_interest_lifetime_ms: int = 100
    max_interest_lifetime_ms: int = 2000
    min_missing_timeout_ms: int = 80
    max_missing_timeout_ms: int = 1500

    def observe_rtt(self, sample_ms: float, *, alpha: float = 0.25) -> None:
        sample = max(1.0, float(sample_ms))
        alpha = min(1.0, max(0.0, float(alpha)))
        self.rtt_ms = self.rtt_ms * (1.0 - alpha) + sample * alpha

    def record_timeout(self) -> None:
        self.timeout_pressure = min(1.0, self.timeout_pressure + 0.25)

    def record_nack(self) -> None:
        self.nack_pressure = min(1.0, self.nack_pressure + 0.2)

    def record_duplicate(self) -> None:
        self.duplicate_pressure = min(1.0, self.duplicate_pressure + 0.1)

    def set_backlog_pressure(self, pressure: float) -> None:
        self.backlog_pressure = min(1.0, max(0.0, float(pressure)))

    def decay(self, factor: float = 0.85) -> None:
        factor = min(1.0, max(0.0, float(factor)))
        self.timeout_pressure *= factor
        self.nack_pressure *= factor
        self.duplicate_pressure *= factor
        self.backlog_pressure *= factor

    def decide(self) -> StreamFetchDecision:
        pressure = max(
            self.timeout_pressure,
            self.nack_pressure,
            self.duplicate_pressure * 0.5,
            self.backlog_pressure,
        )
        pressure = min(1.0, max(0.0, pressure))
        if pressure >= 0.65:
            reason = "congested"
        elif pressure >= 0.25:
            reason = "cautious"
        else:
            reason = "stable"

        window = int(round(self.base_window / (1.0 + pressure * 2.0)))
        lookahead = int(round(self.base_lookahead / (1.0 + pressure * 1.5)))
        lifetime = int(round(max(2.0 * self.rtt_ms, self.min_interest_lifetime_ms) * (1.0 + pressure)))
        missing = int(round(max(1.5 * self.rtt_ms, self.min_missing_timeout_ms) * (1.0 + pressure)))

        return StreamFetchDecision(
            window=min(self.max_window, max(self.min_window, window)),
            lookahead=min(self.max_lookahead, max(self.min_lookahead, lookahead)),
            interest_lifetime_ms=min(self.max_interest_lifetime_ms, max(self.min_interest_lifetime_ms, lifetime)),
            missing_timeout_ms=min(self.max_missing_timeout_ms, max(self.min_missing_timeout_ms, missing)),
            pressure=pressure,
            reason=reason,
        )


__all__ = [
    "STREAM_CHUNK_MAGIC",
    "StreamAdaptiveFetcherState",
    "StreamChunk",
    "StreamConsumerReorderBuffer",
    "StreamFecInfo",
    "StreamFetchDecision",
    "StreamInfo",
    "StreamMetrics",
    "StreamProducerBuffer",
    "decode_stream_chunk",
    "encode_stream_chunk",
    "stream_now_ms",
]
