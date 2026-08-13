"""Reusable NDNSF streaming substrate.

This module keeps app-neutral stream/session/chunk behavior in the NDNSF Python
core layer. Applications still own codecs, camera capture, tensor formats,
decoder queues, and any application-specific FEC repair algorithm.
"""

from __future__ import annotations

import json
import struct
import time
from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Any, Callable, Iterable, Optional

from ._ndnsf import (
    NativeStreamAdaptiveFetcherState,
    NativeStreamChunk,
    NativeStreamConsumerReorderBuffer,
    NativeStreamFecInfo,
    NativeStreamCursorFrontiers,
    NativeStreamMetrics,
    NativeStreamNameMapAdmissionResult,
    NativeStreamNameMapBlock,
    NativeStreamNameMapCheckpoint,
    NativeStreamNameMapEntry,
    NativeStreamNameMapResolution,
    NativeStreamNameMapResolverConfig,
    NativeStreamNameResolverState,
    NativeStreamProducerBuffer,
    NativeVerifiedStreamNameMapData,
    NativeLiveStreamDefinition,
    NativeLiveStreamDescriptor,
    NativeLiveStreamFecOptions,
    NativeLiveStreamItemAdmission,
    NativeLiveStreamReadiness,
    NativeLiveStreamSampleObservation,
    NativeLiveStreamSamplePredictor,
    NativePredictiveStreamCheckpoint,
    NativePredictiveStreamDescriptor,
    NativeSampleClassProfile,
    NativeStreamAdvancedOptions,
    NativeStreamConfig,
    NativePublishedPacketFeedOptions,
    make_stream_name_map_block_name as _native_make_stream_name_map_block_name,
    make_stream_name_map_root as _native_make_stream_name_map_root,
)


STREAM_CHUNK_MAGIC = b"NDS1"
STREAM_HEADER_STRUCT = struct.Struct("!4sI")
STREAM_NAME_MAP_CONTRACT_VERSION_V1 = 1
STREAM_NAME_MAP_CONTRACT_VERSION = STREAM_NAME_MAP_CONTRACT_VERSION_V1
STREAM_NAME_MAP_CONTRACT_VERSION_V2 = 2
STREAM_NAME_MAP_CONTENT_TYPE_MANIFEST = 4


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
    overflows: int = 0
    max_pending: int = 0
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
            "overflows": self.overflows,
            "maxPending": self.max_pending,
            "bytesProduced": self.bytes_produced,
            "bytesReceived": self.bytes_received,
        }


def _metrics_from_native(value: NativeStreamMetrics) -> StreamMetrics:
    return StreamMetrics(
        produced=value.produced,
        evicted=value.evicted,
        received=value.received,
        emitted=value.emitted,
        duplicates=value.duplicates,
        stale=value.stale,
        gaps=value.gaps,
        timeouts=value.timeouts,
        nacks=value.nacks,
        overflows=value.overflows,
        max_pending=value.max_pending,
        bytes_produced=value.bytes_produced,
        bytes_received=value.bytes_received,
    )


def _fec_to_native(value: Optional[StreamFecInfo]) -> Optional[NativeStreamFecInfo]:
    if value is None:
        return None
    native = NativeStreamFecInfo()
    native.scheme = value.scheme
    native.data_shards = value.data_shards
    native.parity_shards = value.parity_shards
    native.symbol_index = value.symbol_index
    native.symbol_count = value.symbol_count
    native.data_lengths = list(value.data_lengths)
    native.source_block_id = value.source_block_id
    native.repair_symbol = value.repair_symbol
    native.metadata = {str(key): str(item) for key, item in value.metadata.items()}
    return native


def _fec_from_native(value: Optional[NativeStreamFecInfo]) -> Optional[StreamFecInfo]:
    if value is None:
        return None
    return StreamFecInfo(
        scheme=value.scheme,
        data_shards=value.data_shards,
        parity_shards=value.parity_shards,
        symbol_index=value.symbol_index,
        symbol_count=value.symbol_count,
        data_lengths=tuple(value.data_lengths),
        source_block_id=value.source_block_id,
        repair_symbol=value.repair_symbol,
        metadata=dict(value.metadata),
    )


def _chunk_to_native(value: StreamChunk) -> NativeStreamChunk:
    native = NativeStreamChunk()
    native.stream_id = value.stream_id
    native.session_epoch = value.session_epoch
    native.seq = value.seq
    native.payload = value.payload
    native.content_type = value.content_type
    native.capture_ms = value.capture_ms
    native.arrival_ms = value.arrival_ms
    native.deadline_ms = value.deadline_ms
    native.key_chunk = value.key_chunk
    native.frame_id = value.frame_id
    native.frame_first_seq = value.frame_first_seq
    native.frame_last_seq = value.frame_last_seq
    native.segment_index = value.segment_index
    native.segment_count = value.segment_count
    native.fec = _fec_to_native(value.fec)
    native.metadata = {str(key): str(item) for key, item in value.metadata.items()}
    return native


def _chunk_from_native(value: NativeStreamChunk) -> StreamChunk:
    return StreamChunk(
        stream_id=value.stream_id,
        session_epoch=value.session_epoch,
        seq=value.seq,
        payload=bytes(value.payload),
        content_type=value.content_type,
        capture_ms=value.capture_ms,
        arrival_ms=value.arrival_ms,
        deadline_ms=value.deadline_ms,
        key_chunk=value.key_chunk,
        frame_id=value.frame_id,
        frame_first_seq=value.frame_first_seq,
        frame_last_seq=value.frame_last_seq,
        segment_index=value.segment_index,
        segment_count=value.segment_count,
        fec=_fec_from_native(value.fec),
        metadata=dict(value.metadata),
    )


class StreamProducerBuffer:
    """Bounded sequence-indexed buffer for recently produced stream chunks."""

    def __init__(self, max_chunks: int = 600) -> None:
        self._native = NativeStreamProducerBuffer(max(1, int(max_chunks)))

    @property
    def metrics(self) -> StreamMetrics:
        return _metrics_from_native(self._native.metrics)

    def put(self, chunk: StreamChunk) -> None:
        self._native.put(_chunk_to_native(chunk))

    def get(self, seq: int) -> Optional[StreamChunk]:
        value = self._native.get(int(seq))
        return None if value is None else _chunk_from_native(value)

    def encoded(self, seq: int) -> Optional[bytes]:
        chunk = self.get(seq)
        return None if chunk is None else encode_stream_chunk(chunk)

    def seqs(self) -> list[int]:
        return list(self._native.sequences())

    def __len__(self) -> int:
        return self._native.size()


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
        self.max_pending = max(1, int(max_pending))
        self._native = NativeStreamConsumerReorderBuffer(
            stream_id,
            int(session_epoch),
            int(next_seq),
            self.max_pending,
            max(1, int(history)),
        )

    @property
    def next_seq(self) -> int:
        return self._native.next_seq

    @property
    def pending_count(self) -> int:
        return self._native.pending_count

    @property
    def pending_bytes(self) -> int:
        return self._native.pending_bytes

    @property
    def metrics(self) -> StreamMetrics:
        return _metrics_from_native(self._native.metrics)

    def reset(self, stream_id: str, session_epoch: int, *, next_seq: int = 0) -> None:
        self.stream_id = stream_id
        self.session_epoch = int(session_epoch)
        self._native.reset(stream_id, int(session_epoch), int(next_seq))

    def push(self, chunk: StreamChunk) -> list[StreamChunk]:
        return [_chunk_from_native(value) for value in self._native.push(_chunk_to_native(chunk))]

    def missing_sequences(self, *, limit: int = 32) -> list[int]:
        return list(self._native.missing_sequences(max(0, int(limit))))

    def pending_sequences(self, *, limit: int = 0) -> list[int]:
        return list(self._native.pending_sequences(max(0, int(limit))))

    def drain_ready(self) -> list[StreamChunk]:
        return [_chunk_from_native(value) for value in self._native.drain_ready()]

    def skip_to(self, seq: int) -> None:
        self._native.skip_to(int(seq))


@dataclass(frozen=True)
class StreamFetchDecision:
    window: int
    lookahead: int
    interest_lifetime_ms: int
    missing_timeout_ms: int
    sample_demand: int
    packet_demand: int
    hold_ms: int
    recovery_checkpoint_ms: int
    pressure: float
    live_edge_confidence: float
    phase: str
    policy_mode: str
    reason: str
    remaining_recovery_budget_ms: int = 0
    mapping_begin_block: int = 0
    mapping_end_block: int = 0
    payload_begin_cursor: int = 0
    payload_end_cursor: int = 0
    aggregate_in_flight_limit: int = 0
    mapping_budget: int = 0
    payload_budget: int = 0
    retransmission_budget: int = 0
    future_wait_count: int = 0
    terminal_unproduced_advice: int = 0
    later_cursor_advice: int = 0
    atomic_expansions: int = 0
    atomic_deferrals: int = 0
    mapping_ready: bool = False
    future_wait: bool = False
    congestion_hold: bool = False
    retransmission_eligible: bool = False
    detector_profile: str = "none"
    mapping_wait_reason: str = "inactive"
    capacity_reason: str = ""


class StreamHealthState(str, Enum):
    ACTIVE = "ACTIVE"
    DEGRADED = "DEGRADED"
    CONGESTED = "CONGESTED"
    STALE = "STALE"
    STOPPED = "STOPPED"


@dataclass(frozen=True)
class StreamHealth:
    """App-neutral stream health snapshot.

    Applications still own codecs and domain policy. This helper only reports
    whether the generic stream session is fresh, congested, or producing gaps.
    """

    stream_id: str
    session_epoch: int
    state: StreamHealthState = StreamHealthState.ACTIVE
    next_seq: int = 0
    last_chunk_ms: int = 0
    updated_ms: int = field(default_factory=stream_now_ms)
    metrics: StreamMetrics = field(default_factory=StreamMetrics)
    fetch_decision: StreamFetchDecision | None = None
    reason: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_stream(cls,
                    info: StreamInfo,
                    metrics: StreamMetrics,
                    *,
                    next_seq: int | None = None,
                    last_chunk_ms: int = 0,
                    fetch_decision: StreamFetchDecision | None = None,
                    stopped: bool = False,
                    stale_after_ms: int = 3000,
                    now_ms_value: int | None = None,
                    reason: str = "",
                    metadata: dict[str, Any] | None = None) -> "StreamHealth":
        current = stream_now_ms() if now_ms_value is None else int(now_ms_value)
        state = StreamHealthState.ACTIVE
        computed_reason = reason
        if stopped:
            state = StreamHealthState.STOPPED
            computed_reason = computed_reason or "stopped"
        elif last_chunk_ms and stale_after_ms > 0 and current - int(last_chunk_ms) > stale_after_ms:
            state = StreamHealthState.STALE
            computed_reason = computed_reason or "stale"
        elif fetch_decision is not None and fetch_decision.reason == "congested":
            state = StreamHealthState.CONGESTED
            computed_reason = computed_reason or "congested"
        elif metrics.gaps or metrics.timeouts or metrics.nacks:
            state = StreamHealthState.DEGRADED
            computed_reason = computed_reason or "loss-or-gap"
        return cls(
            stream_id=info.stream_id,
            session_epoch=info.session_epoch,
            state=state,
            next_seq=info.next_seq if next_seq is None else int(next_seq),
            last_chunk_ms=int(last_chunk_ms),
            updated_ms=current,
            metrics=metrics,
            fetch_decision=fetch_decision,
            reason=computed_reason,
            metadata=_clean_metadata(metadata),
        )

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "streamId": self.stream_id,
            "sessionEpoch": int(self.session_epoch),
            "state": self.state.value,
            "nextSeq": int(self.next_seq),
            "lastChunkMs": int(self.last_chunk_ms),
            "updatedMs": int(self.updated_ms),
            "metrics": self.metrics.to_dict(),
            "reason": self.reason,
            "metadata": _clean_metadata(self.metadata),
        }
        if self.fetch_decision is not None:
            result["fetchDecision"] = {
                "window": int(self.fetch_decision.window),
                "lookahead": int(self.fetch_decision.lookahead),
                "interestLifetimeMs": int(self.fetch_decision.interest_lifetime_ms),
                "missingTimeoutMs": int(self.fetch_decision.missing_timeout_ms),
                "sampleDemand": int(self.fetch_decision.sample_demand),
                "packetDemand": int(self.fetch_decision.packet_demand),
                "holdMs": int(self.fetch_decision.hold_ms),
                "recoveryCheckpointMs": int(
                    self.fetch_decision.recovery_checkpoint_ms),
                "remainingRecoveryBudgetMs": int(
                    self.fetch_decision.remaining_recovery_budget_ms),
                "mappingBeginBlock": int(self.fetch_decision.mapping_begin_block),
                "mappingEndBlock": int(self.fetch_decision.mapping_end_block),
                "payloadBeginCursor": int(self.fetch_decision.payload_begin_cursor),
                "payloadEndCursor": int(self.fetch_decision.payload_end_cursor),
                "aggregateInFlightLimit": int(
                    self.fetch_decision.aggregate_in_flight_limit),
                "mappingBudget": int(self.fetch_decision.mapping_budget),
                "payloadBudget": int(self.fetch_decision.payload_budget),
                "retransmissionBudget": int(
                    self.fetch_decision.retransmission_budget),
                "futureWaitCount": int(self.fetch_decision.future_wait_count),
                "mappingReady": bool(self.fetch_decision.mapping_ready),
                "futureWait": bool(self.fetch_decision.future_wait),
                "congestionHold": bool(self.fetch_decision.congestion_hold),
                "detectorProfile": self.fetch_decision.detector_profile,
                "mappingWaitReason": self.fetch_decision.mapping_wait_reason,
                "pressure": float(self.fetch_decision.pressure),
                "liveEdgeConfidence": float(
                    self.fetch_decision.live_edge_confidence),
                "phase": self.fetch_decision.phase,
                "policyMode": self.fetch_decision.policy_mode,
                "reason": self.fetch_decision.reason,
            }
        return result


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
    live_edge_change_threshold: float = 0.10
    live_edge_period_similarity: float = 0.95
    live_edge_window: int = 30
    live_edge_stable_required: int = 4
    detection_period_ms: int = 1000
    recovery_reserve_packets: int = 1
    aggregate_in_flight_limit: int = 64
    mapping_reserve: int = 4
    retransmission_reserve: int = 1
    mapping_block_capacity: int = 16
    chase_multiplier: float = 2.0
    adjust_multiplier: float = 0.75
    congestion_decrease_multiplier: float = 0.5
    detector_profile: str = "ndnsf-conservative-seed"
    _native: NativeStreamAdaptiveFetcherState = field(init=False, repr=False)

    def __post_init__(self) -> None:
        self._native = NativeStreamAdaptiveFetcherState()
        self._sync_config()

    def _sync_config(self) -> None:
        for python_name in (
            "rtt_ms", "timeout_pressure", "nack_pressure",
            "duplicate_pressure", "backlog_pressure", "min_window",
            "base_window", "max_window", "min_lookahead", "base_lookahead",
            "max_lookahead", "min_interest_lifetime_ms",
            "max_interest_lifetime_ms", "min_missing_timeout_ms",
            "max_missing_timeout_ms", "live_edge_change_threshold",
            "live_edge_period_similarity", "live_edge_window",
            "live_edge_stable_required", "detection_period_ms",
            "recovery_reserve_packets", "aggregate_in_flight_limit",
            "mapping_reserve", "retransmission_reserve",
            "mapping_block_capacity", "chase_multiplier",
            "adjust_multiplier", "congestion_decrease_multiplier",
            "detector_profile",
        ):
            setattr(self._native, python_name, getattr(self, python_name))

    def _native_state(self) -> NativeStreamAdaptiveFetcherState:
        self._sync_config()
        return self._native

    def _sync_native(self, native: NativeStreamAdaptiveFetcherState) -> None:
        self.rtt_ms = native.rtt_ms
        self.timeout_pressure = native.timeout_pressure
        self.nack_pressure = native.nack_pressure
        self.duplicate_pressure = native.duplicate_pressure
        self.backlog_pressure = native.backlog_pressure

    def observe_rtt(self, sample_ms: float, *, alpha: float = 0.25) -> None:
        native = self._native_state()
        native.observe_rtt(float(sample_ms), float(alpha))
        self._sync_native(native)

    def record_timeout(self) -> None:
        native = self._native_state()
        native.record_timeout()
        self._sync_native(native)

    def record_timeout_evidence(self, cursor: int, *, known_produced: bool,
                                was_future: bool) -> None:
        native = self._native_state()
        native.record_timeout_evidence(
            int(cursor), bool(known_produced), bool(was_future))
        self._sync_native(native)

    def record_nack(self) -> None:
        native = self._native_state()
        native.record_nack()
        self._sync_native(native)

    def record_nack_reason(self, cursor: int, reason: str) -> None:
        native = self._native_state()
        native.record_nack_reason(int(cursor), str(reason))
        self._sync_native(native)

    def record_congestion_mark(self, cursor: int, mark: int) -> None:
        native = self._native_state()
        native.record_congestion_mark(int(cursor), int(mark))
        self._sync_native(native)

    def record_duplicate(self) -> None:
        native = self._native_state()
        native.record_duplicate()
        self._sync_native(native)

    def set_backlog_pressure(self, pressure: float) -> None:
        native = self._native_state()
        native.set_backlog_pressure(float(pressure))
        self._sync_native(native)

    def decay(self, factor: float = 0.85) -> None:
        native = self._native_state()
        native.decay(float(factor))
        self._sync_native(native)

    def reset_live(self, session_epoch: int, next_seq: int,
                   sample_period_ms: float, *, now_ms: int = 0) -> None:
        native = self._native_state()
        native.reset_live(int(session_epoch), int(next_seq),
                          float(sample_period_ms), int(now_ms))

    def configure_mapped_live(self, *, aggregate_limit: int | None = None,
                              mapping_reserve: int | None = None,
                              retransmission_reserve: int | None = None,
                              block_capacity: int | None = None,
                              detector_profile: str | None = None) -> None:
        if aggregate_limit is not None:
            self.aggregate_in_flight_limit = int(aggregate_limit)
        if mapping_reserve is not None:
            self.mapping_reserve = int(mapping_reserve)
        if retransmission_reserve is not None:
            self.retransmission_reserve = int(retransmission_reserve)
        if block_capacity is not None:
            self.mapping_block_capacity = int(block_capacity)
        if detector_profile is not None:
            self.detector_profile = str(detector_profile)
        native = self._native_state()
        native.configure_mapped_live(
            int(self.aggregate_in_flight_limit), int(self.mapping_reserve),
            int(self.retransmission_reserve), int(self.mapping_block_capacity),
            str(self.detector_profile))
        self.live_edge_change_threshold = native.live_edge_change_threshold
        self.live_edge_period_similarity = native.live_edge_period_similarity
        self.live_edge_window = native.live_edge_window
        self.live_edge_stable_required = native.live_edge_stable_required

    def reset_mapped_live(self, session_epoch: int, next_cursor: int,
                          sample_period_ms: float, latest_produced_cursor: int,
                          mapping_committed_through_cursor: int,
                          next_reserved_cursor: int, *, now_ms: int = 0) -> None:
        self.configure_mapped_live()
        self._native_state().reset_mapped_live(
            int(session_epoch), int(next_cursor), float(sample_period_ms),
            int(latest_produced_cursor),
            int(mapping_committed_through_cursor), int(next_reserved_cursor),
            int(now_ms))

    def update_mapping_frontier(self, mapping_committed_through_cursor: int,
                                next_reserved_cursor: int) -> None:
        self._native_state().update_mapping_frontier(
            int(mapping_committed_through_cursor), int(next_reserved_cursor))

    def advance_next_cursor(self, next_cursor: int) -> None:
        self._native_state().advance_next_cursor(int(next_cursor))

    def set_mapped_live_policy_enabled(self, enabled: bool) -> None:
        """Select live-v1 or its mapped-pressure rollback on the same wire."""
        self._native_state().set_mapped_live_policy_enabled(bool(enabled))

    def set_in_flight(self, *, mapping: int, payload: int,
                      retransmission: int) -> None:
        self._native_state().set_in_flight(
            int(mapping), int(payload), int(retransmission))

    def observe_accepted_sample(self, session_epoch: int, sample_id: int,
                                arrival_ms: int, retrieval_delay_ms: float,
                                *, segment_count: int = 1,
                                known_produced: bool = True) -> bool:
        native = self._native_state()
        accepted = native.observe_accepted_sample(
            int(session_epoch), int(sample_id), int(arrival_ms),
            float(retrieval_delay_ms), int(segment_count), bool(known_produced))
        self._sync_native(native)
        return bool(accepted)

    def observe_sample_extent(self, predicted_count: int,
                              actual_count: int) -> None:
        self._native_state().observe_sample_extent(
            int(predicted_count), int(actual_count))

    def begin_recovery(self, now_ms: int, playout_deadline_ms: int) -> None:
        self._native_state().begin_recovery(
            int(now_ms), int(playout_deadline_ms))

    def record_recovery(self, completed: bool) -> None:
        self._native_state().record_recovery(bool(completed))

    def record_invalid_observation(self) -> None:
        self._native_state().record_invalid_observation()

    def stop_live(self) -> None:
        self._native_state().stop_live()

    @property
    def phase(self) -> str:
        return str(self._native_state().phase_name)

    @property
    def invalid_observations(self) -> int:
        return int(self._native_state().invalid_observations)

    def decide(self, *, now_ms: int = 0,
               playout_deadline_ms: int = 0) -> StreamFetchDecision:
        decision = self._native_state().decide(
            int(now_ms), int(playout_deadline_ms))
        return StreamFetchDecision(
            window=decision.window,
            lookahead=decision.lookahead,
            interest_lifetime_ms=decision.interest_lifetime_ms,
            missing_timeout_ms=decision.missing_timeout_ms,
            sample_demand=decision.sample_demand,
            packet_demand=decision.packet_demand,
            hold_ms=decision.hold_ms,
            recovery_checkpoint_ms=decision.recovery_checkpoint_ms,
            remaining_recovery_budget_ms=decision.remaining_recovery_budget_ms,
            mapping_begin_block=decision.mapping_begin_block,
            mapping_end_block=decision.mapping_end_block,
            payload_begin_cursor=decision.payload_begin_cursor,
            payload_end_cursor=decision.payload_end_cursor,
            aggregate_in_flight_limit=decision.aggregate_in_flight_limit,
            mapping_budget=decision.mapping_budget,
            payload_budget=decision.payload_budget,
            retransmission_budget=decision.retransmission_budget,
            future_wait_count=decision.future_wait_count,
            terminal_unproduced_advice=decision.terminal_unproduced_advice,
            later_cursor_advice=decision.later_cursor_advice,
            atomic_expansions=decision.atomic_expansions,
            atomic_deferrals=decision.atomic_deferrals,
            pressure=decision.pressure,
            live_edge_confidence=decision.live_edge_confidence,
            mapping_ready=decision.mapping_ready,
            future_wait=decision.future_wait,
            congestion_hold=decision.congestion_hold,
            retransmission_eligible=decision.retransmission_eligible,
            phase=decision.phase_name,
            policy_mode=decision.policy_mode,
            detector_profile=decision.detector_profile,
            mapping_wait_reason=decision.mapping_wait_reason,
            capacity_reason=decision.capacity_reason,
            reason=decision.reason,
        )


@dataclass(frozen=True)
class StreamNameMapEntry:
    """One immutable cursor slot in a :class:`StreamNameMapBlock`.

    ``original_name`` is the URI spelling of a real nested NDN Name TLV.  A
    tombstone is declared before publication and deliberately has no name.
    """

    original_name: str = ""
    tombstone: bool = False
    group_id: str = ""
    sample_class: str = ""
    group_item_index: int = 0
    predicted_source_items: int = 0
    predicted_repair_items: int = 0

    def __post_init__(self) -> None:
        if self.tombstone and self.original_name:
            raise ValueError("a StreamNameMap tombstone cannot carry a name")
        if not self.tombstone and not self.original_name:
            raise ValueError("a named StreamNameMap entry requires original_name")

    @classmethod
    def from_name(cls, original_name: str) -> "StreamNameMapEntry":
        return cls(original_name=str(original_name), tombstone=False)

    @classmethod
    def from_grouped_name(
            cls, original_name: str, group_id: str, sample_class: str,
            group_item_index: int, predicted_source_items: int,
            predicted_repair_items: int) -> "StreamNameMapEntry":
        return cls(str(original_name), False, str(group_id), str(sample_class),
                   int(group_item_index), int(predicted_source_items),
                   int(predicted_repair_items))

    @classmethod
    def make_tombstone(cls) -> "StreamNameMapEntry":
        return cls(tombstone=True)

    def _to_native(self) -> NativeStreamNameMapEntry:
        if self.tombstone:
            return NativeStreamNameMapEntry.make_tombstone()
        if self.group_id or self.sample_class or self.predicted_source_items:
            return NativeStreamNameMapEntry.from_grouped_name(
                self.original_name, self.group_id, self.sample_class,
                int(self.group_item_index), int(self.predicted_source_items),
                int(self.predicted_repair_items))
        return NativeStreamNameMapEntry.from_name(self.original_name)

    @classmethod
    def _from_native(cls, value: NativeStreamNameMapEntry) -> "StreamNameMapEntry":
        if bool(value.is_tombstone()):
            return cls.make_tombstone()
        if bool(value.has_group_binding):
            return cls.from_grouped_name(
                value.original_name, value.group_id, value.sample_class,
                value.group_item_index, value.predicted_source_items,
                value.predicted_repair_items)
        return cls.from_name(value.original_name)


@dataclass(frozen=True)
class StreamNameMapBlock:
    """Canonical, fixed-capacity cursor-to-semantic-name Mapping content.

    Encoding and hashing always delegate to the C++ implementation.  Python
    therefore cannot drift into a second wire codec.
    """

    stream_id: str
    session_epoch: int
    mapping_version: int
    block_number: int
    block_capacity: int
    first_cursor: int
    entries: tuple[StreamNameMapEntry, ...]
    previous_content_digest: bytes | None = None
    contract_version: int = STREAM_NAME_MAP_CONTRACT_VERSION

    def __post_init__(self) -> None:
        object.__setattr__(self, "entries", tuple(self.entries))
        if self.previous_content_digest is not None:
            object.__setattr__(self, "previous_content_digest",
                               bytes(self.previous_content_digest))

    def _to_native(self) -> NativeStreamNameMapBlock:
        value = NativeStreamNameMapBlock()
        value.contract_version = int(self.contract_version)
        value.stream_id = self.stream_id
        value.session_epoch = int(self.session_epoch)
        value.mapping_version = int(self.mapping_version)
        value.block_number = int(self.block_number)
        value.block_capacity = int(self.block_capacity)
        value.first_cursor = int(self.first_cursor)
        value.previous_content_digest = self.previous_content_digest
        value.entries = [entry._to_native() for entry in self.entries]
        return value

    @classmethod
    def _from_native(cls, value: NativeStreamNameMapBlock) -> "StreamNameMapBlock":
        digest = value.previous_content_digest
        return cls(
            contract_version=int(value.contract_version),
            stream_id=value.stream_id,
            session_epoch=int(value.session_epoch),
            mapping_version=int(value.mapping_version),
            block_number=int(value.block_number),
            block_capacity=int(value.block_capacity),
            first_cursor=int(value.first_cursor),
            previous_content_digest=None if digest is None else bytes(digest),
            entries=tuple(StreamNameMapEntry._from_native(entry)
                          for entry in value.entries),
        )

    def wire_encode(self) -> bytes:
        return bytes(self._to_native().wire_encode())

    def validate(self) -> str | None:
        """Return the native contract error, or ``None`` when canonical."""

        result = self._to_native().validate()
        return None if result is None else str(result)

    @classmethod
    def decode(cls, wire: bytes) -> "StreamNameMapBlock":
        return cls._from_native(NativeStreamNameMapBlock.decode(bytes(wire)))

    def canonical_content(self) -> bytes:
        return bytes(self._to_native().canonical_content())

    def content_digest(self) -> bytes:
        return bytes(self._to_native().content_digest())

    def fits_signed_wire_budget(self, signed_envelope_overhead: int,
                                configured_wire_cap: int) -> bool:
        return bool(self._to_native().fits_signed_wire_budget(
            int(signed_envelope_overhead), int(configured_wire_cap)))

    @property
    def last_cursor(self) -> int:
        return int(self._to_native().last_cursor())


def make_stream_name_map_root(provider: str, stream_id: str) -> str:
    """Return the Mapping base name before typed Version/Sequence components."""

    return str(_native_make_stream_name_map_root(str(provider), str(stream_id)))


def make_stream_name_map_block_name(mapping_root: str, mapping_version: int,
                                    block_number: int) -> str:
    """Return one exact Mapping Data name with typed Version and SequenceNum."""

    return str(_native_make_stream_name_map_block_name(
        str(mapping_root), int(mapping_version), int(block_number)))


@dataclass(frozen=True)
class StreamCursorFrontiers:
    """The five ordered cursor frontiers advertised by a Provider."""

    oldest_retained: int
    latest_join: int
    latest_produced: int
    mapping_committed_through: int
    next_reserved: int

    def _to_native(self) -> NativeStreamCursorFrontiers:
        value = NativeStreamCursorFrontiers()
        value.oldest_retained = int(self.oldest_retained)
        value.latest_join = int(self.latest_join)
        value.latest_produced = int(self.latest_produced)
        value.mapping_committed_through = int(self.mapping_committed_through)
        value.next_reserved = int(self.next_reserved)
        return value

    @classmethod
    def _from_native(cls, value: NativeStreamCursorFrontiers) -> "StreamCursorFrontiers":
        return cls(
            oldest_retained=int(value.oldest_retained),
            latest_join=int(value.latest_join),
            latest_produced=int(value.latest_produced),
            mapping_committed_through=int(value.mapping_committed_through),
            next_reserved=int(value.next_reserved),
        )

    def validate(self, block_capacity: int,
                 checkpoint_block: int) -> str | None:
        result = self._to_native().validate(int(block_capacity),
                                            int(checkpoint_block))
        return None if result is None else str(result)


@dataclass(frozen=True)
class StreamNameMapCheckpoint:
    frontiers: StreamCursorFrontiers
    block_number: int
    content_digest: bytes

    def __post_init__(self) -> None:
        object.__setattr__(self, "content_digest", bytes(self.content_digest))

    def _to_native(self) -> NativeStreamNameMapCheckpoint:
        value = NativeStreamNameMapCheckpoint()
        value.frontiers = self.frontiers._to_native()
        value.block_number = int(self.block_number)
        value.content_digest = self.content_digest
        return value

    @classmethod
    def _from_native(cls, value: NativeStreamNameMapCheckpoint) -> "StreamNameMapCheckpoint":
        return cls(
            frontiers=StreamCursorFrontiers._from_native(value.frontiers),
            block_number=int(value.block_number),
            content_digest=bytes(value.content_digest),
        )


@dataclass(frozen=True)
class StreamNameMapResolverConfig:
    stream_id: str
    session_epoch: int
    mapping_version: int
    block_capacity: int
    expected_provider: str
    mapping_root: str
    payload_prefix: str
    signed_wire_cap: int = 8800
    max_verified_blocks: int = 32
    max_quarantine_blocks: int = 8
    max_reverse_entries: int = 65536
    max_original_name_wire_bytes: int = 4096
    contract_version: int = STREAM_NAME_MAP_CONTRACT_VERSION

    def _to_native(self) -> NativeStreamNameMapResolverConfig:
        value = NativeStreamNameMapResolverConfig()
        value.contract_version = int(self.contract_version)
        value.stream_id = self.stream_id
        value.session_epoch = int(self.session_epoch)
        value.mapping_version = int(self.mapping_version)
        value.block_capacity = int(self.block_capacity)
        value.expected_provider = self.expected_provider
        value.mapping_root = self.mapping_root
        value.payload_prefix = self.payload_prefix
        value.signed_wire_cap = int(self.signed_wire_cap)
        value.max_verified_blocks = int(self.max_verified_blocks)
        value.max_quarantine_blocks = int(self.max_quarantine_blocks)
        value.max_reverse_entries = int(self.max_reverse_entries)
        value.max_original_name_wire_bytes = int(self.max_original_name_wire_bytes)
        return value


@dataclass(frozen=True)
class StreamNameMapAdmissionResult:
    disposition: str
    timing: str
    reason: str
    state_changed: bool
    mapping_committed_through: int
    accepted: bool
    fatal: bool

    @classmethod
    def _from_native(cls, value: NativeStreamNameMapAdmissionResult) -> "StreamNameMapAdmissionResult":
        return cls(
            disposition=str(value.disposition_name),
            timing=str(value.timing_name),
            reason=value.reason,
            state_changed=bool(value.state_changed),
            mapping_committed_through=int(value.mapping_committed_through),
            accepted=bool(value.accepted),
            fatal=bool(value.fatal),
        )


@dataclass(frozen=True)
class StreamNameMapResolution:
    cursor: int
    original_name: str
    tombstone: bool
    terminal_unproduced: bool
    timing: str
    schedulable: bool

    @classmethod
    def _from_native(cls, value: NativeStreamNameMapResolution) -> "StreamNameMapResolution":
        return cls(
            cursor=int(value.cursor),
            original_name=value.original_name,
            tombstone=bool(value.tombstone),
            terminal_unproduced=bool(value.terminal_unproduced),
            timing=str(value.timing_name),
            schedulable=bool(value.schedulable),
        )


class StreamNameResolver:
    """Bounded Python facade over the single native StreamNameMap resolver.

    Callers must validate the Mapping Data signature and Provider chain before
    calling :meth:`admit_verified_block`.  This object performs deterministic
    envelope, continuity, frontier, immutability, and cache-bound checks only.
    """

    def __init__(self, config: StreamNameMapResolverConfig,
                 checkpoint: StreamNameMapCheckpoint) -> None:
        self._native = NativeStreamNameResolverState()
        self.reset(config, checkpoint)

    def reset(self, config: StreamNameMapResolverConfig,
              checkpoint: StreamNameMapCheckpoint) -> None:
        self._native.reset(config._to_native(), checkpoint._to_native())

    def admit_verified_block(
        self,
        *,
        data_name: str,
        verified_provider: str,
        content: bytes,
        signed_wire_size: int,
        content_type: int = STREAM_NAME_MAP_CONTENT_TYPE_MANIFEST,
        has_final_block: bool = False,
        received_monotonic_ms: int = 0,
        required_before_monotonic_ms: int = 0,
    ) -> StreamNameMapAdmissionResult:
        value = NativeVerifiedStreamNameMapData()
        value.data_name = str(data_name)
        value.verified_provider = str(verified_provider)
        value.content_type = int(content_type)
        value.has_final_block = bool(has_final_block)
        value.signed_wire_size = int(signed_wire_size)
        value.received_monotonic_ms = int(received_monotonic_ms)
        value.required_before_monotonic_ms = int(required_before_monotonic_ms)
        return StreamNameMapAdmissionResult._from_native(
            self._native.admit_verified_wire(value, bytes(content)))

    def refresh_checkpoint(self, checkpoint: StreamNameMapCheckpoint) -> StreamNameMapAdmissionResult:
        return StreamNameMapAdmissionResult._from_native(
            self._native.refresh_checkpoint(checkpoint._to_native()))

    def lookup(self, cursor: int) -> StreamNameMapResolution | None:
        value = self._native.lookup(int(cursor))
        return None if value is None else StreamNameMapResolution._from_native(value)

    def resolve(self, cursor: int) -> str | None:
        value = self._native.resolve(int(cursor))
        return None if value is None else str(value)

    def reverse_resolve(self, original_name: str) -> int | None:
        value = self._native.reverse_resolve(str(original_name))
        return None if value is None else int(value)

    def mark_terminal_unproduced(self, cursor: int) -> bool:
        return bool(self._native.mark_terminal_unproduced(int(cursor)))

    def evict_local_block(self, block_number: int) -> bool:
        return bool(self._native.evict_local_block(int(block_number)))

    @property
    def frontiers(self) -> StreamCursorFrontiers:
        return StreamCursorFrontiers._from_native(self._native.frontiers())

    @property
    def checkpoint(self) -> StreamNameMapCheckpoint:
        return StreamNameMapCheckpoint._from_native(self._native.checkpoint())

    @property
    def faulted(self) -> bool:
        return bool(self._native.faulted())

    @property
    def verified_block_count(self) -> int:
        return int(self._native.verified_block_count())

    @property
    def quarantined_block_count(self) -> int:
        return int(self._native.quarantined_block_count())

    @property
    def binding_count(self) -> int:
        return int(self._native.binding_count())

    @property
    def diagnostics(self) -> dict[str, int]:
        return {str(key): int(value)
                for key, value in self._native.diagnostics().items()}


@dataclass(frozen=True)
class SampleClassProfile:
    """Opaque bounded sample class; Core does not interpret ``class_id``."""

    class_id: str
    seed_source_items: int
    hard_max_source_items: int
    history_capacity: int = 32
    safety_margin_items: int = 1

    def _to_native(self) -> NativeSampleClassProfile:
        return NativeSampleClassProfile.bounded(
            str(self.class_id), int(self.seed_source_items),
            int(self.hard_max_source_items), int(self.history_capacity),
            int(self.safety_margin_items))

    @classmethod
    def _from_native(cls, value) -> "SampleClassProfile":
        return cls(
            class_id=str(value.class_id),
            seed_source_items=int(value.seed_source_items),
            hard_max_source_items=int(value.hard_max_source_items),
            history_capacity=int(value.history_capacity),
            safety_margin_items=int(value.safety_margin_items),
        )


@dataclass(frozen=True)
class SampleClassPredictionStatus:
    class_id: str
    prediction: int
    observations: int
    underpredictions: int
    underpredicted_items: int
    overpredictions: int
    overpredicted_items: int

    @classmethod
    def _from_native(cls, value) -> "SampleClassPredictionStatus":
        return cls(
            class_id=str(value.class_id), prediction=int(value.prediction),
            observations=int(value.observations),
            underpredictions=int(value.underpredictions),
            underpredicted_items=int(value.underpredicted_items),
            overpredictions=int(value.overpredictions),
            overpredicted_items=int(value.overpredicted_items))


class LiveStreamSamplePredictor:
    """Bounded per-class predictor shared by C++ and Python applications."""

    def __init__(self, profiles: tuple[SampleClassProfile, ...] | list[SampleClassProfile]):
        self._native = NativeLiveStreamSamplePredictor(
            [profile._to_native() for profile in profiles])

    def predict(self, class_id: str) -> int:
        return int(self._native.predict(str(class_id)))

    def observe(self, class_id: str, actual_source_items: int) -> bool:
        return bool(self._native.observe(str(class_id), int(actual_source_items)))

    def status(self, class_id: str) -> SampleClassPredictionStatus | None:
        value = self._native.status(str(class_id))
        return None if value is None else SampleClassPredictionStatus._from_native(value)

    @property
    def statuses(self) -> dict[str, SampleClassPredictionStatus]:
        return {str(key): SampleClassPredictionStatus._from_native(value)
                for key, value in self._native.statuses().items()}


@dataclass(frozen=True)
class LiveStreamFecOptions:
    """Optional Core recovery over opaque application bytes; disabled by default."""

    scheme: str = "none"
    max_source_items: int = 0
    max_source_bytes: int = 0
    recovery_budget_ms: int = 0
    repair_symbols: int = 0

    @classmethod
    def none(cls) -> "LiveStreamFecOptions":
        return cls()

    @classmethod
    def xor_one_repair(
        cls, source_items: int, max_source_bytes: int,
        recovery_budget_ms: int = 500,
    ) -> "LiveStreamFecOptions":
        native = NativeLiveStreamFecOptions.xor_one_repair(
            int(source_items), int(max_source_bytes), int(recovery_budget_ms))
        return cls("xor-one-repair", native.max_source_items,
                   native.max_source_bytes, native.recovery_budget_ms,
                   native.repair_symbols)

    @classmethod
    def gf256_two_repair(
        cls, source_items: int, max_source_bytes: int,
        recovery_budget_ms: int = 500,
    ) -> "LiveStreamFecOptions":
        native = NativeLiveStreamFecOptions.gf256_two_repair(
            int(source_items), int(max_source_bytes), int(recovery_budget_ms))
        return cls("gf256-two-repair", native.max_source_items,
                   native.max_source_bytes, native.recovery_budget_ms,
                   native.repair_symbols)

    def _to_native(self) -> NativeLiveStreamFecOptions:
        if self.scheme == "none":
            return NativeLiveStreamFecOptions.none()
        if self.scheme == "gf256-two-repair":
            return NativeLiveStreamFecOptions.gf256_two_repair(
                int(self.max_source_items), int(self.max_source_bytes),
                int(self.recovery_budget_ms))
        if self.scheme != "xor-one-repair":
            raise ValueError(f"unsupported LiveStream FEC scheme: {self.scheme}")
        return NativeLiveStreamFecOptions.xor_one_repair(
            int(self.max_source_items), int(self.max_source_bytes),
            int(self.recovery_budget_ms))

    @classmethod
    def _from_native(cls, value) -> "LiveStreamFecOptions":
        spelling = str(value.scheme).rsplit(".", 1)[-1].lower()
        scheme = {
            "none": "none",
            "xor_one_repair": "xor-one-repair",
            "gf256_two_repair": "gf256-two-repair",
        }.get(spelling)
        if scheme is None:
            raise ValueError(f"unknown native LiveStream FEC scheme: {value.scheme}")
        return cls(
            scheme=scheme,
            max_source_items=int(value.max_source_items),
            max_source_bytes=int(value.max_source_bytes),
            recovery_budget_ms=int(value.recovery_budget_ms),
            repair_symbols=int(value.repair_symbols),
        )

    @property
    def source_items(self) -> int:
        """Compatibility spelling; new code should use ``max_source_items``."""
        return self.max_source_items


@dataclass(frozen=True)
class StreamAdvancedOptions:
    mapping_block_capacity: int = 16
    mapping_ahead_blocks: int = 4
    retained_items: int = 600
    max_name_reservations: int = 65536
    max_pending_interests: int = 256
    signed_wire_cap: int = 8800
    startup_timeout_ms: int = 1000

    def _to_native(self) -> NativeStreamAdvancedOptions:
        value = NativeStreamAdvancedOptions()
        value.mapping_block_capacity = int(self.mapping_block_capacity)
        value.mapping_ahead_blocks = int(self.mapping_ahead_blocks)
        value.retained_items = int(self.retained_items)
        value.max_name_reservations = int(self.max_name_reservations)
        value.max_pending_interests = int(self.max_pending_interests)
        value.signed_wire_cap = int(self.signed_wire_cap)
        value.startup_timeout_ms = int(self.startup_timeout_ms)
        return value


@dataclass(frozen=True)
class StreamConfig:
    stream_id: str
    data_prefix: str
    sample_period_ms: float
    sample_classes: tuple[SampleClassProfile, ...]
    fec: LiveStreamFecOptions = field(default_factory=LiveStreamFecOptions.none)
    session_epoch: int | None = None
    advanced: StreamAdvancedOptions = field(default_factory=StreamAdvancedOptions)

    def _to_native(self) -> NativeStreamConfig:
        value = NativeStreamConfig()
        value.stream_id = str(self.stream_id)
        value.data_prefix = str(self.data_prefix)
        value.sample_period_ms = float(self.sample_period_ms)
        value.sample_classes = [
            profile._to_native() for profile in self.sample_classes
        ]
        value.fec = self.fec._to_native()
        value.session_epoch = (
            None if self.session_epoch is None else int(self.session_epoch)
        )
        value.advanced = self.advanced._to_native()
        return value


@dataclass(frozen=True)
class LiveStreamDefinition:
    stream_id: str
    provider: str
    semantic_data_prefix: str
    session_epoch: int
    mapping_version: int
    contract_version: int = STREAM_NAME_MAP_CONTRACT_VERSION
    mapping_block_capacity: int = 16
    mapping_ahead_blocks: int = 4
    retained_items: int = 600
    max_name_reservations: int = 65536
    max_pending_interests: int = 256
    signed_wire_cap: int = 8800
    sample_period_ms: float = 0.0
    sample_classes: tuple[SampleClassProfile, ...] = ()
    fec: LiveStreamFecOptions = field(default_factory=LiveStreamFecOptions.none)
    _mapping_root: str = field(default="", repr=False, compare=False)

    @classmethod
    def _from_native(cls, value) -> "LiveStreamDefinition":
        return cls(
            stream_id=str(value.stream_id),
            provider=str(value.provider),
            semantic_data_prefix=str(value.semantic_data_prefix),
            session_epoch=int(value.session_epoch),
            mapping_version=int(value.mapping_version),
            contract_version=int(value.contract_version),
            mapping_block_capacity=int(value.mapping_block_capacity),
            mapping_ahead_blocks=int(value.mapping_ahead_blocks),
            retained_items=int(value.retained_items),
            max_name_reservations=int(value.max_name_reservations),
            max_pending_interests=int(value.max_pending_interests),
            signed_wire_cap=int(value.signed_wire_cap),
            sample_period_ms=float(value.sample_period_ms),
            sample_classes=tuple(
                SampleClassProfile._from_native(profile)
                for profile in value.sample_classes
            ),
            fec=LiveStreamFecOptions._from_native(value.fec),
            _mapping_root=str(value.mapping_root),
        )

    @property
    def mapping_root(self) -> str:
        if not self._mapping_root:
            raise ValueError("mapping_root is available on received stream definitions")
        return self._mapping_root

    def _to_native(self) -> NativeLiveStreamDefinition:
        value = NativeLiveStreamDefinition()
        value.contract_version = int(self.contract_version)
        value.stream_id = self.stream_id
        value.provider = self.provider
        value.semantic_data_prefix = self.semantic_data_prefix
        value.session_epoch = int(self.session_epoch)
        value.mapping_version = int(self.mapping_version)
        value.mapping_block_capacity = int(self.mapping_block_capacity)
        value.mapping_ahead_blocks = int(self.mapping_ahead_blocks)
        value.retained_items = int(self.retained_items)
        value.max_name_reservations = int(self.max_name_reservations)
        value.max_pending_interests = int(self.max_pending_interests)
        value.signed_wire_cap = int(self.signed_wire_cap)
        value.sample_period_ms = float(self.sample_period_ms)
        value.sample_classes = [profile._to_native() for profile in self.sample_classes]
        value.fec = self.fec._to_native()
        error = value.validate()
        if error is not None:
            raise ValueError(f"invalid LiveStream definition: {error}")
        return value


class LiveStreamItemReservation:
    def __init__(self, native) -> None:
        self._native = native

    @property
    def cursor(self) -> int:
        return int(self._native.cursor)

    @property
    def original_name(self) -> str:
        return str(self._native.original_name)


class LiveStreamGroupReservation:
    def __init__(self, native) -> None:
        self._native = native

    @property
    def group_id(self) -> str:
        return str(self._native.group_id)

    @property
    def sources(self) -> tuple[LiveStreamItemReservation, ...]:
        return tuple(LiveStreamItemReservation(value) for value in self._native.sources)

    @property
    def repairs(self) -> tuple[LiveStreamItemReservation, ...]:
        return tuple(LiveStreamItemReservation(value) for value in self._native.repairs)


class LiveStreamSampleReservation:
    def __init__(self, native) -> None:
        self._native = native

    @property
    def sample_id(self) -> int:
        return int(self._native.sample_id)

    @property
    def sample_class(self) -> str:
        return str(self._native.sample_class)

    @property
    def predicted_source_items(self) -> int:
        return int(self._native.predicted_source_items)

    @property
    def group(self) -> LiveStreamGroupReservation:
        return LiveStreamGroupReservation(self._native.group)


class LiveStreamDescriptor:
    def __init__(self, native: NativeLiveStreamDescriptor) -> None:
        self._native = native

    @property
    def stream_id(self) -> str:
        return str(self._native.definition.stream_id)

    @property
    def provider(self) -> str:
        return str(self._native.definition.provider)

    @property
    def contract_version(self) -> int:
        return int(self._native.definition.contract_version)

    @property
    def semantic_data_prefix(self) -> str:
        return str(self._native.definition.semantic_data_prefix)

    @property
    def safe_join_cursor(self) -> int:
        return int(self._native.safe_join_cursor)

    @property
    def measured_sample_period_ms(self) -> float:
        return float(self._native.measured_sample_period_ms)

    def to_dict(self) -> dict[str, Any]:
        definition = self._native.definition
        fec = definition.fec
        checkpoint = StreamNameMapCheckpoint._from_native(self._native.checkpoint)
        return {
            "definition": {
                "streamId": str(definition.stream_id),
                "contractVersion": int(definition.contract_version),
                "provider": str(definition.provider),
                "semanticDataPrefix": str(definition.semantic_data_prefix),
                "sessionEpoch": int(definition.session_epoch),
                "mappingVersion": int(definition.mapping_version),
                "mappingBlockCapacity": int(definition.mapping_block_capacity),
                "mappingAheadBlocks": int(definition.mapping_ahead_blocks),
                "retainedItems": int(definition.retained_items),
                "maxNameReservations": int(definition.max_name_reservations),
                "maxPendingInterests": int(definition.max_pending_interests),
                "signedWireCap": int(definition.signed_wire_cap),
                "samplePeriodMs": float(definition.sample_period_ms),
                "sampleClasses": [
                    {
                        "classId": str(profile.class_id),
                        "seedSourceItems": int(profile.seed_source_items),
                        "hardMaxSourceItems": int(profile.hard_max_source_items),
                        "historyCapacity": int(profile.history_capacity),
                        "safetyMarginItems": int(profile.safety_margin_items),
                    }
                    for profile in definition.sample_classes
                ],
                "fec": {
                    "scheme": ("none" if not fec.enabled else
                               "gf256-two-repair" if int(fec.recovery_capacity) == 2
                               else "xor-one-repair"),
                    "maxSourceItems": int(fec.max_source_items),
                    "maxSourceBytes": int(fec.max_source_bytes),
                    "recoveryBudgetMs": int(fec.recovery_budget_ms),
                    "repairSymbols": int(fec.repair_symbols),
                },
            },
            "checkpoint": {
                "frontiers": {
                    "oldestRetained": checkpoint.frontiers.oldest_retained,
                    "latestJoin": checkpoint.frontiers.latest_join,
                    "latestProduced": checkpoint.frontiers.latest_produced,
                    "mappingCommittedThrough": checkpoint.frontiers.mapping_committed_through,
                    "nextReserved": checkpoint.frontiers.next_reserved,
                },
                "blockNumber": checkpoint.block_number,
                "contentDigestHex": checkpoint.content_digest.hex(),
            },
            "measuredSamplePeriodMs": self.measured_sample_period_ms,
            "safeJoinCursor": self.safe_join_cursor,
        }

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "LiveStreamDescriptor":
        definition_value = dict(payload["definition"])
        fec_value = dict(definition_value.get("fec") or {})
        fec_scheme = fec_value.get("scheme", "none")
        if fec_scheme == "xor-one-repair":
            fec = LiveStreamFecOptions.xor_one_repair(
                int(fec_value.get("maxSourceItems", fec_value.get("sourceItems", 0))),
                int(fec_value["maxSourceBytes"]),
                int(fec_value.get("recoveryBudgetMs", 500)),
            )
        elif fec_scheme == "gf256-two-repair":
            fec = LiveStreamFecOptions.gf256_two_repair(
                int(fec_value.get("maxSourceItems", fec_value.get("sourceItems", 0))),
                int(fec_value["maxSourceBytes"]),
                int(fec_value.get("recoveryBudgetMs", 500)),
            )
        elif fec_scheme == "none":
            fec = LiveStreamFecOptions.none()
        else:
            raise ValueError(f"unsupported descriptor FEC scheme: {fec_scheme}")
        definition = LiveStreamDefinition(
            stream_id=str(definition_value["streamId"]),
            provider=str(definition_value["provider"]),
            semantic_data_prefix=str(definition_value["semanticDataPrefix"]),
            session_epoch=int(definition_value["sessionEpoch"]),
            mapping_version=int(definition_value["mappingVersion"]),
            contract_version=int(definition_value.get("contractVersion", 1)),
            mapping_block_capacity=int(definition_value.get("mappingBlockCapacity", 16)),
            mapping_ahead_blocks=int(definition_value.get("mappingAheadBlocks", 4)),
            retained_items=int(definition_value.get("retainedItems", 600)),
            max_name_reservations=int(definition_value.get("maxNameReservations", 65536)),
            max_pending_interests=int(definition_value.get("maxPendingInterests", 256)),
            signed_wire_cap=int(definition_value.get("signedWireCap", 8800)),
            sample_period_ms=float(definition_value.get("samplePeriodMs", 0.0)),
            sample_classes=tuple(
                SampleClassProfile(
                    class_id=str(item["classId"]),
                    seed_source_items=int(item["seedSourceItems"]),
                    hard_max_source_items=int(item["hardMaxSourceItems"]),
                    history_capacity=int(item.get("historyCapacity", 32)),
                    safety_margin_items=int(item.get("safetyMarginItems", 1)),
                )
                for item in definition_value.get("sampleClasses", ())
            ),
            fec=fec,
        )
        checkpoint_value = dict(payload["checkpoint"])
        frontier_value = dict(checkpoint_value["frontiers"])
        checkpoint = StreamNameMapCheckpoint(
            frontiers=StreamCursorFrontiers(
                oldest_retained=int(frontier_value["oldestRetained"]),
                latest_join=int(frontier_value["latestJoin"]),
                latest_produced=int(frontier_value["latestProduced"]),
                mapping_committed_through=int(frontier_value["mappingCommittedThrough"]),
                next_reserved=int(frontier_value["nextReserved"]),
            ),
            block_number=int(checkpoint_value["blockNumber"]),
            content_digest=bytes.fromhex(str(checkpoint_value["contentDigestHex"])),
        )
        native = NativeLiveStreamDescriptor()
        native.definition = definition._to_native()
        native.checkpoint = checkpoint._to_native()
        native.measured_sample_period_ms = float(payload["measuredSamplePeriodMs"])
        native.safe_join_cursor = int(payload["safeJoinCursor"])
        error = native.validate()
        if error is not None:
            raise ValueError(f"invalid LiveStream descriptor: {error}")
        return cls(native)


class LiveStreamItemAdmission:
    def __init__(self, native) -> None:
        self._native = native

    @classmethod
    def accept_item(cls) -> "LiveStreamItemAdmission":
        return cls(NativeLiveStreamItemAdmission.accept_item())

    @classmethod
    def reject_item(cls, reason: str) -> "LiveStreamItemAdmission":
        return cls(NativeLiveStreamItemAdmission.reject_item(str(reason)))


@dataclass(frozen=True)
class VerifiedLiveStreamItem:
    cursor: int
    original_name: str
    verified_provider: str
    content: bytes
    provenance: str
    received_ms: int

    @classmethod
    def _from_native(cls, value) -> "VerifiedLiveStreamItem":
        provenance = str(value.provenance).rsplit(".", 1)[-1].lower().replace("_", "-")
        return cls(int(value.cursor), str(value.original_name),
                   str(value.verified_provider), bytes(value.content),
                   provenance, int(value.received_ms))


@dataclass(frozen=True)
class StreamSubscriptionOptions:
    on_item: Callable[
        ["VerifiedLiveStreamItem"], bool | "LiveStreamItemAdmission"
    ]
    start: str = "latest"
    prefetch_policy: str | None = None
    aggregate_interest_limit: int = 64
    enable_fec_recovery: bool = True
    require_full_delivery: bool = False
    interest_lifetime_ms: int = 500
    on_status: Callable[[Any], None] | None = None


class StreamPublisher:
    """Predictive high-level facade over the Core-owned stream publisher."""

    def __init__(self, native) -> None:
        self._native = native

    def start(self) -> "PredictiveStreamDescriptor":
        """Start the predictive stream. No bootstrap data is required."""
        return PredictiveStreamDescriptor(self._native.start())

    def push(self, signed_data: bytes) -> None:
        """Push one exact-name, application-signed NDN Data packet.

        The application owns the keychain and signature. ``ndnsf`` validates
        and retains the supplied wire; it does not sign or wrap it again.
        """
        self._native.push(bytes(signed_data))

    def flush(self) -> None:
        """Generate and publish FEC parity for pending source segments."""
        self._native.flush()

    def status(self):
        return self._native.status()

    def stop(self) -> None:
        self._native.stop()


@dataclass(frozen=True)
class PredictiveStreamCheckpoint:
    initial_sample_id: int = 0
    oldest_retained_sample_id: int = 0
    latest_produced_sample_id: int = 0
    next_expected_sample_id: int = 0

    def to_dict(self) -> dict[str, int]:
        return {
            "initialSampleId": int(self.initial_sample_id),
            "oldestRetainedSampleId": int(self.oldest_retained_sample_id),
            "latestProducedSampleId": int(self.latest_produced_sample_id),
            "nextExpectedSampleId": int(self.next_expected_sample_id),
        }


class PredictiveStreamDescriptor:
    def __init__(self, native) -> None:
        self._native = native

    @property
    def definition(self) -> "LiveStreamDefinition":
        return LiveStreamDefinition._from_native(self._native.definition)

    @property
    def checkpoint(self) -> PredictiveStreamCheckpoint:
        c = self._native.checkpoint
        return PredictiveStreamCheckpoint(
            initial_sample_id=c.initial_sample_id,
            oldest_retained_sample_id=c.oldest_retained_sample_id,
            latest_produced_sample_id=c.latest_produced_sample_id,
            next_expected_sample_id=c.next_expected_sample_id,
        )

    @property
    def frontier_name(self) -> str:
        return str(self._native.frontier_name)

    def to_dict(self) -> dict[str, Any]:
        definition = self.definition
        fec = definition.fec
        return {
            "definition": {
                "streamId": definition.stream_id,
                "contractVersion": int(definition.contract_version),
                "provider": definition.provider,
                "semanticDataPrefix": definition.semantic_data_prefix,
                "sessionEpoch": int(definition.session_epoch),
                "mappingVersion": int(definition.mapping_version),
                "mappingBlockCapacity": int(definition.mapping_block_capacity),
                "mappingAheadBlocks": int(definition.mapping_ahead_blocks),
                "retainedItems": int(definition.retained_items),
                "maxNameReservations": int(definition.max_name_reservations),
                "maxPendingInterests": int(definition.max_pending_interests),
                "signedWireCap": int(definition.signed_wire_cap),
                "samplePeriodMs": float(definition.sample_period_ms),
                "sampleClasses": [
                    {
                        "classId": profile.class_id,
                        "seedSourceItems": int(profile.seed_source_items),
                        "hardMaxSourceItems": int(profile.hard_max_source_items),
                        "historyCapacity": int(profile.history_capacity),
                        "safetyMarginItems": int(profile.safety_margin_items),
                    }
                    for profile in definition.sample_classes
                ],
                "fec": {
                    "scheme": fec.scheme,
                    "maxSourceItems": int(fec.max_source_items),
                    "maxSourceBytes": int(fec.max_source_bytes),
                    "recoveryBudgetMs": int(fec.recovery_budget_ms),
                    "repairSymbols": int(fec.repair_symbols),
                },
            },
            "checkpoint": self.checkpoint.to_dict(),
            "frontierName": self.frontier_name,
            "measuredSamplePeriodMs": float(
                self._native.measured_sample_period_ms
            ),
        }

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PredictiveStreamDescriptor":
        definition_value = dict(payload["definition"])
        fec_value = dict(definition_value.get("fec") or {})
        fec_scheme = str(fec_value.get("scheme", "none"))
        if fec_scheme == "xor-one-repair":
            fec = LiveStreamFecOptions.xor_one_repair(
                int(fec_value["maxSourceItems"]),
                int(fec_value["maxSourceBytes"]),
                int(fec_value.get("recoveryBudgetMs", 500)),
            )
        elif fec_scheme == "gf256-two-repair":
            fec = LiveStreamFecOptions.gf256_two_repair(
                int(fec_value["maxSourceItems"]),
                int(fec_value["maxSourceBytes"]),
                int(fec_value.get("recoveryBudgetMs", 500)),
            )
        elif fec_scheme == "none":
            fec = LiveStreamFecOptions.none()
        else:
            raise ValueError(f"unsupported descriptor FEC scheme: {fec_scheme}")
        definition = LiveStreamDefinition(
            stream_id=str(definition_value["streamId"]),
            provider=str(definition_value["provider"]),
            semantic_data_prefix=str(definition_value["semanticDataPrefix"]),
            session_epoch=int(definition_value["sessionEpoch"]),
            mapping_version=int(definition_value["mappingVersion"]),
            contract_version=int(definition_value.get("contractVersion", 2)),
            mapping_block_capacity=int(definition_value.get("mappingBlockCapacity", 16)),
            mapping_ahead_blocks=int(definition_value.get("mappingAheadBlocks", 4)),
            retained_items=int(definition_value.get("retainedItems", 600)),
            max_name_reservations=int(definition_value.get("maxNameReservations", 65536)),
            max_pending_interests=int(definition_value.get("maxPendingInterests", 256)),
            signed_wire_cap=int(definition_value.get("signedWireCap", 8800)),
            sample_period_ms=float(definition_value.get("samplePeriodMs", 0.0)),
            sample_classes=tuple(
                SampleClassProfile(
                    class_id=str(item["classId"]),
                    seed_source_items=int(item["seedSourceItems"]),
                    hard_max_source_items=int(item["hardMaxSourceItems"]),
                    history_capacity=int(item.get("historyCapacity", 32)),
                    safety_margin_items=int(item.get("safetyMarginItems", 1)),
                )
                for item in definition_value.get("sampleClasses", ())
            ),
            fec=fec,
        )
        checkpoint_value = dict(payload["checkpoint"])
        checkpoint = PredictiveStreamCheckpoint(
            initial_sample_id=int(checkpoint_value["initialSampleId"]),
            oldest_retained_sample_id=int(
                checkpoint_value["oldestRetainedSampleId"]
            ),
            latest_produced_sample_id=int(
                checkpoint_value["latestProducedSampleId"]
            ),
            next_expected_sample_id=int(
                checkpoint_value["nextExpectedSampleId"]
            ),
        )
        native_checkpoint = NativePredictiveStreamCheckpoint()
        native_checkpoint.initial_sample_id = checkpoint.initial_sample_id
        native_checkpoint.oldest_retained_sample_id = (
            checkpoint.oldest_retained_sample_id
        )
        native_checkpoint.latest_produced_sample_id = (
            checkpoint.latest_produced_sample_id
        )
        native_checkpoint.next_expected_sample_id = (
            checkpoint.next_expected_sample_id
        )
        native = NativePredictiveStreamDescriptor(
            definition._to_native(),
            native_checkpoint,
            str(payload["frontierName"]),
            float(payload.get("measuredSamplePeriodMs", definition.sample_period_ms)),
        )
        return cls(native)


class LiveStreamPublisher:
    def __init__(self, native) -> None:
        self._native = native

    def reserve_ahead(self, original_name: str) -> LiveStreamItemReservation:
        return LiveStreamItemReservation(self._native.reserve_ahead(str(original_name)))

    def reserve_many_ahead(self, original_names: Iterable[str]) -> tuple[LiveStreamItemReservation, ...]:
        return tuple(LiveStreamItemReservation(value) for value in
                     self._native.reserve_many_ahead([str(name) for name in original_names]))

    def reserve_group(self, group_id: str, source_names: Iterable[str],
                      repair_names: Iterable[str]) -> LiveStreamGroupReservation:
        return LiveStreamGroupReservation(self._native.reserve_group(
            str(group_id), [str(name) for name in source_names],
            [str(name) for name in repair_names]))

    def announce_sample(self, sample_id: int, sample_class: str,
                        name_factory) -> LiveStreamSampleReservation:
        return LiveStreamSampleReservation(self._native.announce_sample(
            int(sample_id), str(sample_class), name_factory))

    def prepare_sample_extent(
            self, reservation: LiveStreamSampleReservation,
            actual_source_items: int) -> tuple[LiveStreamItemReservation, ...]:
        return tuple(LiveStreamItemReservation(value) for value in
                     self._native.prepare_sample_extent(
                         reservation._native, int(actual_source_items)))

    def publish(self, reservation: LiveStreamItemReservation, opaque_content: bytes) -> None:
        self._native.publish(reservation._native, bytes(opaque_content))

    def publish_group(self, reservation: LiveStreamGroupReservation,
                      opaque_sources: Iterable[bytes]) -> None:
        self._native.publish_group(
            reservation._native, [bytes(value) for value in opaque_sources])

    def publish_sample(self, reservation: LiveStreamSampleReservation,
                       opaque_sources: Iterable[bytes]) -> None:
        self._native.publish_sample(
            reservation._native, [bytes(value) for value in opaque_sources])

    def activate(self, *, measured_sample_period_ms: float,
                 safe_join_cursor: int) -> LiveStreamDescriptor:
        readiness = NativeLiveStreamReadiness()
        readiness.measured_sample_period_ms = float(measured_sample_period_ms)
        readiness.safe_join_cursor = int(safe_join_cursor)
        return LiveStreamDescriptor(self._native.activate(readiness))

    def open_published_packet_feed(
            self, *, from_cursor: int = 0, max_queued_packets: int = 1024,
            max_queued_bytes: int = 8 * 1024 * 1024) -> "PublishedPacketFeed":
        options = NativePublishedPacketFeedOptions()
        options.from_cursor = int(from_cursor)
        options.max_queued_packets = int(max_queued_packets)
        options.max_queued_bytes = int(max_queued_bytes)
        return PublishedPacketFeed(self._native.open_published_packet_feed(options))

    def status(self):
        return self._native.status()

    def stop(self) -> None:
        self._native.stop()


@dataclass(frozen=True)
class PublishedLiveStreamPacket:
    kind: str
    stream_id: str
    session_epoch: int
    mapping_version: int
    cursor: Optional[int]
    data_name: str
    provider: str
    signed_data_wire: bytes
    wire_digest: bytes
    materialized_monotonic_us: int

    @classmethod
    def _from_native(cls, value) -> "PublishedLiveStreamPacket":
        kind = str(value.kind).rsplit(".", 1)[-1].lower().replace("_", "-")
        return cls(kind, str(value.stream_id), int(value.session_epoch),
                   int(value.mapping_version),
                   None if value.cursor is None else int(value.cursor),
                   str(value.data_name), str(value.provider),
                   bytes(value.signed_data_wire), bytes(value.wire_digest),
                   int(value.materialized_monotonic_us))


class PublishedPacketFeed:
    def __init__(self, native) -> None:
        self._native = native

    def take_available(self, max_items: int = 128) -> tuple[PublishedLiveStreamPacket, ...]:
        return tuple(PublishedLiveStreamPacket._from_native(value)
                     for value in self._native.take_available(int(max_items)))

    def status(self):
        return self._native.status()

    def close(self) -> None:
        self._native.close()


class LiveStreamConsumerHandle:
    def __init__(self, native) -> None:
        self._native = native

    def start(self) -> None:
        self._native.start()

    def observe_accepted_sample(self, sample_id: int, arrival_ms: int,
                                retrieval_delay_ms: float,
                                item_count: int = 1) -> bool:
        value = NativeLiveStreamSampleObservation()
        value.sample_id = int(sample_id)
        value.arrival_ms = int(arrival_ms)
        value.retrieval_delay_ms = float(retrieval_delay_ms)
        value.item_count = int(item_count)
        return bool(self._native.observe_accepted_sample(value))

    def status(self):
        return self._native.status()

    def stop(self) -> None:
        self._native.stop()


class PredictiveStreamSubscriber:
    def __init__(self, native) -> None:
        self._native = native

    def start(self) -> None:
        self._native.start()

    def status(self):
        return self._native.status()

    def stop(self) -> None:
        self._native.stop()


__all__ = [
    "STREAM_CHUNK_MAGIC",
    "STREAM_NAME_MAP_CONTENT_TYPE_MANIFEST",
    "STREAM_NAME_MAP_CONTRACT_VERSION",
    "STREAM_NAME_MAP_CONTRACT_VERSION_V1",
    "STREAM_NAME_MAP_CONTRACT_VERSION_V2",
    "SampleClassProfile",
    "SampleClassPredictionStatus",
    "LiveStreamSamplePredictor",
    "LiveStreamSampleReservation",
    "StreamAdaptiveFetcherState",
    "StreamAdvancedOptions",
    "StreamChunk",
    "StreamConfig",
    "StreamConsumerReorderBuffer",
    "StreamCursorFrontiers",
    "StreamFecInfo",
    "StreamFetchDecision",
    "StreamHealth",
    "StreamHealthState",
    "StreamInfo",
    "StreamMetrics",
    "StreamNameMapAdmissionResult",
    "StreamNameMapBlock",
    "StreamNameMapCheckpoint",
    "StreamNameMapEntry",
    "StreamNameMapResolution",
    "StreamNameMapResolverConfig",
    "StreamNameResolver",
    "StreamProducerBuffer",
    "StreamPublisher",
    "StreamSubscriptionOptions",
    "PredictiveStreamCheckpoint",
    "PredictiveStreamDescriptor",
    "LiveStreamConsumerHandle",
    "PredictiveStreamSubscriber",
    "LiveStreamDefinition",
    "LiveStreamDescriptor",
    "LiveStreamFecOptions",
    "LiveStreamGroupReservation",
    "LiveStreamItemAdmission",
    "LiveStreamItemReservation",
    "LiveStreamPublisher",
    "PublishedLiveStreamPacket",
    "PublishedPacketFeed",
    "VerifiedLiveStreamItem",
    "decode_stream_chunk",
    "encode_stream_chunk",
    "make_stream_name_map_block_name",
    "make_stream_name_map_root",
    "stream_now_ms",
]
