"""Service-neutral runtime telemetry types for NDNSF core.

These types mirror the C++ structs in ``ServiceProvider.hpp`` and are
reusable across NDNSF-DI, UAV, DistributedRepo, and future applications.

DI-specific types (fragment state, model keys, plan templates, advisory
coordinators) remain in ``ndnsf_distributed_inference.runtime_v1``.
"""

from __future__ import annotations

import base64
from dataclasses import dataclass, field, asdict, replace as dataclass_replace
from enum import Enum
import hashlib
import json
import time
from pathlib import Path
from typing import Any, Iterable


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------


def now_ms() -> int:
    return int(time.time() * 1000)


def to_plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        return {key: to_plain(item) for key, item in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): to_plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_plain(item) for item in value]
    return value


def stable_json(payload: Any) -> str:
    return json.dumps(to_plain(payload), sort_keys=True, separators=(",", ":"))


def stable_digest(payload: Any, *, length: int = 16) -> str:
    return hashlib.sha256(stable_json(payload).encode("utf-8")).hexdigest()[:length]


def write_json(path: str | Path, payload: Any) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(to_plain(payload), indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")


def read_json(path: str | Path) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _string_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [item.strip() for item in value.split(",") if item.strip()]
    return [str(item) for item in value]


def _safe_field(value: str, field: str) -> str:
    text = str(value)
    if not text:
        raise ValueError(f"{field} must not be empty")
    if any(ch in text for ch in ";\r\n"):
        raise ValueError(f"{field} must not contain ';' or newlines: {text!r}")
    return text


def encode_ack_metadata(fields: dict[str, Any]) -> bytes:
    """Encode typed metadata as legacy-compatible ``key=value;`` fields."""
    parts: list[str] = []
    for key in sorted(fields):
        value = fields[key]
        if value is None:
            continue
        safe_key = _safe_field(key, "ACK key")
        if isinstance(value, bool):
            encoded = "1" if value else "0"
        elif isinstance(value, (int, float, str)):
            encoded = str(value)
        elif isinstance(value, (list, tuple)) and all(
            isinstance(item, (str, int, float)) for item in value
        ):
            if not value:
                continue
            encoded = ",".join(str(item) for item in value)
        else:
            raw = stable_json(value).encode("utf-8")
            encoded = "json64:" + base64.urlsafe_b64encode(raw).decode("ascii")
        _safe_field(encoded, f"ACK field {safe_key}")
        parts.append(f"{safe_key}={encoded}")
    return (";".join(parts) + (";" if parts else "")).encode("utf-8")


def parse_ack_metadata(payload: bytes | str) -> dict[str, Any]:
    text = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else str(payload)
    fields: dict[str, Any] = {}
    for item in text.split(";"):
        if not item or "=" not in item:
            continue
        key, value = item.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            continue
        if value.startswith("json64:"):
            try:
                raw = base64.urlsafe_b64decode(value[len("json64:"):].encode("ascii"))
                fields[key] = json.loads(raw.decode("utf-8"))
            except Exception:
                fields[key] = value
        else:
            fields[key] = value
    return fields


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------


class AdmissionLeaseStatus(str, Enum):
    GRANTED = "GRANTED"
    REJECTED = "REJECTED"
    DELAYED = "DELAYED"
    CONSUMED = "CONSUMED"
    EXPIRED = "EXPIRED"
    RELEASED = "RELEASED"


# ---------------------------------------------------------------------------
# Fragment / residency ready-cost constants (service-neutral)
# ---------------------------------------------------------------------------

RESIDENCY_READY_COST_MS: dict[str, float] = {
    "GPU_LOADED": 0.0,
    "CPU_RESIDENT": 8.0,
    "DISK_RESIDENT": 35.0,
    "REPO_AVAILABLE": 120.0,
    "MISSING": 1_000_000.0,
}


# ---------------------------------------------------------------------------
# Dataclasses — network / runtime / lease types
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class PeerNetworkMetric:
    src_peer: str
    dst_peer: str
    rtt_ms: float = 0.0
    bandwidth_mbps: float = 0.0
    loss_rate: float = 0.0
    jitter_ms: float = 0.0
    bytes_sampled: int = 0
    updated_at_ms: int = field(default_factory=now_ms)
    confidence: float = 1.0

    def __post_init__(self) -> None:
        if not self.src_peer or not self.dst_peer:
            raise ValueError("peer metric requires src_peer and dst_peer")
        if not 0.0 <= float(self.loss_rate) <= 1.0:
            raise ValueError("loss_rate must be between 0 and 1")
        if self.bandwidth_mbps < 0:
            raise ValueError("bandwidth_mbps must be non-negative")
        if not 0.0 <= float(self.confidence) <= 1.0:
            raise ValueError("confidence must be between 0 and 1")

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "PeerNetworkMetric":
        return cls(
            src_peer=str(payload.get("srcPeer", payload.get("src_peer", ""))),
            dst_peer=str(payload.get("dstPeer", payload.get("dst_peer", ""))),
            rtt_ms=float(payload.get("rttMs", payload.get("rtt_ms", 0.0)) or 0.0),
            bandwidth_mbps=float(payload.get(
                "bandwidthMbps",
                payload.get("bandwidth_mbps", 0.0)) or 0.0),
            loss_rate=float(payload.get("lossRate", payload.get("loss_rate", 0.0)) or 0.0),
            jitter_ms=float(payload.get("jitterMs", payload.get("jitter_ms", 0.0)) or 0.0),
            bytes_sampled=int(payload.get("bytesSampled", payload.get("bytes_sampled", 0)) or 0),
            updated_at_ms=int(payload.get("updatedAtMs", payload.get("updated_at_ms", now_ms()))),
            confidence=float(payload.get("confidence", 1.0)),
        )


@dataclass(frozen=True)
class GenericProviderRuntimeHint:
    provider_name: str
    timestamp_ms: int = field(default_factory=now_ms)
    active_work_count: int = 0
    queue_length: int = 0
    estimated_queue_wait_ms: float = 0.0
    capacity_hints: dict[str, Any] = field(default_factory=dict)
    peer_metrics: tuple[PeerNetworkMetric, ...] = ()
    confidence: float = 1.0

    def __post_init__(self) -> None:
        if not self.provider_name:
            raise ValueError("provider_name is required")
        if self.active_work_count < 0 or self.queue_length < 0:
            raise ValueError("work and queue counts must be non-negative")
        if not 0.0 <= float(self.confidence) <= 1.0:
            raise ValueError("confidence must be between 0 and 1")

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "GenericProviderRuntimeHint":
        metrics = payload.get("peerMetrics", payload.get("peer_metrics", ())) or ()
        return cls(
            provider_name=str(payload.get("providerName", payload.get("provider_name", ""))),
            timestamp_ms=int(payload.get("timestampMs", payload.get("timestamp_ms", now_ms()))),
            active_work_count=int(payload.get(
                "activeWorkCount",
                payload.get("active_work_count", 0)) or 0),
            queue_length=int(payload.get("queueLength", payload.get("queue_length", 0)) or 0),
            estimated_queue_wait_ms=float(payload.get(
                "estimatedQueueWaitMs",
                payload.get("estimated_queue_wait_ms", 0.0)) or 0.0),
            capacity_hints=dict(payload.get("capacityHints", payload.get("capacity_hints", {})) or {}),
            peer_metrics=tuple(
                item if isinstance(item, PeerNetworkMetric) else PeerNetworkMetric.from_dict(dict(item))
                for item in metrics
            ),
            confidence=float(payload.get("confidence", 1.0)),
        )


@dataclass(frozen=True)
class GenericAdmissionLease:
    lease_id: str
    request_id: str
    service_name: str
    provider_name: str
    status: AdmissionLeaseStatus = AdmissionLeaseStatus.GRANTED
    reason_code: str = ""
    estimated_start_ms: int = 0
    estimated_finish_ms: int = 0
    expires_at_ms: int = 0
    resource_binding_schema: str = ""
    resource_binding: dict[str, Any] = field(default_factory=dict)
    consumed: bool = False

    def __post_init__(self) -> None:
        if not self.lease_id:
            raise ValueError("lease_id is required")
        if not self.request_id:
            raise ValueError("request_id is required")
        if not self.service_name:
            raise ValueError("service_name is required")
        if not self.provider_name:
            raise ValueError("provider_name is required")

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "GenericAdmissionLease":
        return cls(
            lease_id=str(payload.get("leaseId", payload.get("lease_id", ""))),
            request_id=str(payload.get("requestId", payload.get("request_id", ""))),
            service_name=str(payload.get("serviceName", payload.get("service_name", ""))),
            provider_name=str(payload.get("providerName", payload.get("provider_name", ""))),
            status=AdmissionLeaseStatus(payload.get("status", AdmissionLeaseStatus.GRANTED.value)),
            reason_code=str(payload.get("reasonCode", payload.get("reason_code", ""))),
            estimated_start_ms=int(payload.get(
                "estimatedStartMs",
                payload.get("estimated_start_ms", 0)) or 0),
            estimated_finish_ms=int(payload.get(
                "estimatedFinishMs",
                payload.get("estimated_finish_ms", 0)) or 0),
            expires_at_ms=int(payload.get("expiresAtMs", payload.get("expires_at_ms", 0)) or 0),
            resource_binding_schema=str(payload.get(
                "resourceBindingSchema",
                payload.get("resource_binding_schema", ""))),
            resource_binding=dict(payload.get(
                "resourceBinding",
                payload.get("resource_binding", {})) or {}),
            consumed=bool(payload.get("consumed", False)),
        )

    def is_valid(self, *, now_ms_value: int | None = None) -> bool:
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        return (
            self.status == AdmissionLeaseStatus.GRANTED and
            not self.consumed and
            (not self.expires_at_ms or current < self.expires_at_ms)
        )

    def binding_digest(self) -> str:
        return stable_digest({
            "schema": self.resource_binding_schema,
            "binding": self.resource_binding,
        }, length=24)


@dataclass(frozen=True)
class GenericLeaseValidationResult:
    status: bool
    reason_code: str = ""
    lease_id: str = ""
    request_id: str = ""
    service_name: str = ""
    provider_name: str = ""


@dataclass(frozen=True)
class GenericAckMetadata:
    provider_runtime_hint: GenericProviderRuntimeHint
    lease_offers: tuple[GenericAdmissionLease, ...] = ()
    service_payload_schema: str = ""
    service_payload: dict[str, Any] = field(default_factory=dict)
    metric_digest: str = ""
    notes: str = ""
    schema: str = "ndnsf-ack-metadata-v1"

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "GenericAckMetadata":
        hint_payload = payload.get("providerRuntimeHint", payload.get("provider_runtime_hint", {}))
        lease_payloads = payload.get("leaseOffers", payload.get("lease_offers", ())) or ()
        return cls(
            schema=str(payload.get("schema", "ndnsf-ack-metadata-v1")),
            provider_runtime_hint=(
                hint_payload if isinstance(hint_payload, GenericProviderRuntimeHint)
                else GenericProviderRuntimeHint.from_dict(dict(hint_payload))
            ),
            lease_offers=tuple(
                item if isinstance(item, GenericAdmissionLease) else GenericAdmissionLease.from_dict(dict(item))
                for item in lease_payloads
            ),
            service_payload_schema=str(payload.get(
                "servicePayloadSchema",
                payload.get("service_payload_schema", ""))),
            service_payload=dict(payload.get("servicePayload", payload.get("service_payload", {})) or {}),
            metric_digest=str(payload.get("metricDigest", payload.get("metric_digest", ""))),
            notes=str(payload.get("notes", "")),
        )

    def to_ack_fields(self) -> dict[str, Any]:
        return {"genericAckMetadata": to_plain(self)}

    @classmethod
    def from_ack_fields(cls, fields: dict[str, Any]) -> "GenericAckMetadata":
        payload = fields.get("genericAckMetadata", fields.get("generic_ack_metadata", {}))
        if not isinstance(payload, dict):
            raise ValueError("genericAckMetadata field must be a JSON object")
        return cls.from_dict(payload)


# ---------------------------------------------------------------------------
# Lease table
# ---------------------------------------------------------------------------


class ProviderAdmissionLeaseTable:
    def __init__(self) -> None:
        self._leases: dict[str, GenericAdmissionLease] = {}
        self._counters: dict[str, int] = {
            "granted": 0,
            "rejected": 0,
            "expired": 0,
            "consumed": 0,
            "released": 0,
        }

    def grant(self, lease: GenericAdmissionLease) -> GenericAdmissionLease:
        self._leases[lease.lease_id] = lease
        if lease.status == AdmissionLeaseStatus.GRANTED:
            self._counters["granted"] += 1
        else:
            self._counters["rejected"] += 1
        return lease

    def release(self, lease_id: str) -> GenericLeaseValidationResult:
        lease = self._leases.pop(lease_id, None)
        if lease is None:
            return GenericLeaseValidationResult(False, "LEASE_NOT_FOUND", lease_id=lease_id)
        self._counters["released"] += 1
        return GenericLeaseValidationResult(
            True,
            "LEASE_RELEASED",
            lease_id=lease.lease_id,
            request_id=lease.request_id,
            service_name=lease.service_name,
            provider_name=lease.provider_name,
        )

    def consume(self, *,
                lease_id: str,
                request_id: str,
                service_name: str,
                provider_name: str,
                resource_binding: dict[str, Any] | None = None,
                now_ms_value: int | None = None) -> GenericLeaseValidationResult:
        lease = self._leases.get(lease_id)
        if lease is None:
            return GenericLeaseValidationResult(False, "LEASE_NOT_FOUND", lease_id=lease_id)
        if lease.consumed:
            return self._result(False, "LEASE_ALREADY_CONSUMED", lease)
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        if lease.expires_at_ms and current >= lease.expires_at_ms:
            self._counters["expired"] += 1
            return self._result(False, "LEASE_EXPIRED", lease)
        if lease.request_id != request_id:
            return self._result(False, "LEASE_REQUEST_MISMATCH", lease)
        if lease.service_name != service_name:
            return self._result(False, "LEASE_SERVICE_MISMATCH", lease)
        if lease.provider_name != provider_name:
            return self._result(False, "LEASE_PROVIDER_MISMATCH", lease)
        if resource_binding is not None and stable_digest(resource_binding) != stable_digest(lease.resource_binding):
            return self._result(False, "LEASE_BINDING_MISMATCH", lease)
        self._leases[lease_id] = dataclass_replace(
            lease,
            status=AdmissionLeaseStatus.CONSUMED,
            consumed=True,
        )
        self._counters["consumed"] += 1
        return self._result(True, "LEASE_CONSUMED", lease)

    def counters(self) -> dict[str, int]:
        return dict(self._counters)

    @staticmethod
    def _result(status: bool, reason: str, lease: GenericAdmissionLease) -> GenericLeaseValidationResult:
        return GenericLeaseValidationResult(
            status=status,
            reason_code=reason,
            lease_id=lease.lease_id,
            request_id=lease.request_id,
            service_name=lease.service_name,
            provider_name=lease.provider_name,
        )


# ---------------------------------------------------------------------------
# Provider network matrix
# ---------------------------------------------------------------------------


class ProviderNetworkMatrix:
    def __init__(self,
                 metrics: Iterable[PeerNetworkMetric] = (),
                 *,
                 default_rtt_ms: float = 50.0,
                 default_bandwidth_mbps: float = 100.0,
                 stale_after_ms: int = 30000,
                 stale_penalty_ms: float = 25.0,
                 unknown_penalty_ms: float = 100.0):
        self.metrics = {(m.src_peer, m.dst_peer): m for m in metrics}
        self.default_rtt_ms = float(default_rtt_ms)
        self.default_bandwidth_mbps = float(default_bandwidth_mbps)
        self.stale_after_ms = int(stale_after_ms)
        self.stale_penalty_ms = float(stale_penalty_ms)
        self.unknown_penalty_ms = float(unknown_penalty_ms)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ProviderNetworkMatrix":
        metrics = [
            item if isinstance(item, PeerNetworkMetric) else PeerNetworkMetric.from_dict(dict(item))
            for item in payload.get("metrics", [])
        ]
        return cls(
            metrics,
            default_rtt_ms=float(payload.get("defaultRttMs", payload.get("default_rtt_ms", 50.0))),
            default_bandwidth_mbps=float(payload.get(
                "defaultBandwidthMbps",
                payload.get("default_bandwidth_mbps", 100.0))),
            stale_after_ms=int(payload.get("staleAfterMs", payload.get("stale_after_ms", 30000))),
            stale_penalty_ms=float(payload.get(
                "stalePenaltyMs",
                payload.get("stale_penalty_ms", 25.0))),
            unknown_penalty_ms=float(payload.get(
                "unknownPenaltyMs",
                payload.get("unknown_penalty_ms", 100.0))),
        )

    def metric(self, src_peer: str, dst_peer: str) -> PeerNetworkMetric | None:
        return self.metrics.get((src_peer, dst_peer))

    def transfer_cost_ms(self, src_peer: str, dst_peer: str, bytes_count: int,
                         *, now_ms_value: int | None = None) -> tuple[float, dict[str, Any]]:
        if src_peer == dst_peer:
            return 0.0, {"sameProvider": True}
        metric = self.metric(src_peer, dst_peer)
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        unknown = metric is None
        rtt = self.default_rtt_ms if metric is None else max(0.0, metric.rtt_ms)
        bandwidth = self.default_bandwidth_mbps if metric is None else max(0.001, metric.bandwidth_mbps)
        loss = 0.0 if metric is None else metric.loss_rate
        jitter = 0.0 if metric is None else max(0.0, metric.jitter_ms)
        confidence = 0.0 if metric is None else metric.confidence
        transfer_ms = max(0, int(bytes_count)) * 8.0 / (bandwidth * 1_000_000.0) * 1000.0
        stale = metric is not None and self.stale_after_ms > 0 and current - metric.updated_at_ms > self.stale_after_ms
        penalty = 0.0
        if unknown:
            penalty += self.unknown_penalty_ms
        if stale:
            penalty += self.stale_penalty_ms
        penalty += loss * 100.0 + jitter + (1.0 - confidence) * 25.0
        total = rtt + transfer_ms + penalty
        return total, {
            "srcPeer": src_peer,
            "dstPeer": dst_peer,
            "bytes": max(0, int(bytes_count)),
            "rttMs": rtt,
            "bandwidthMbps": bandwidth,
            "transferMs": transfer_ms,
            "lossRate": loss,
            "jitterMs": jitter,
            "confidence": confidence,
            "unknown": unknown,
            "stale": stale,
            "penaltyMs": penalty,
            "totalMs": total,
        }
