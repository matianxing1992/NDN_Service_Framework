"""Core service-discovery view over NDNSD health and capability hints.

Applications can keep their own discovery policy, but the common classification
of ready, draining, unavailable, and stale providers belongs in NDNSF core.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable

from .ndnsd_health import NdnsdProviderState
from .runtime_telemetry import GenericProviderRuntimeHint, ProviderCapabilityHint, now_ms


DRAIN_ACTIVE = "ACTIVE"
DRAIN_READY = "READY"
DRAIN_DRAINING = "DRAINING"
DRAIN_PROVISIONING = "PROVISIONING"
DRAIN_MAINTENANCE = "MAINTENANCE"
DRAIN_UNAVAILABLE = "UNAVAILABLE"
DRAIN_OFFLINE = "OFFLINE"

READY_DRAIN_STATES = frozenset({DRAIN_ACTIVE, DRAIN_READY, ""})
NON_READY_DRAIN_STATES = frozenset({
    DRAIN_DRAINING,
    DRAIN_PROVISIONING,
    DRAIN_MAINTENANCE,
    DRAIN_UNAVAILABLE,
    DRAIN_OFFLINE,
})


def normalize_drain_state(value: Any) -> str:
    text = str(value or DRAIN_ACTIVE).strip().upper()
    if text == "READY":
        return DRAIN_READY
    if text in {"DRAIN", "DRAINED"}:
        return DRAIN_DRAINING
    if text in {"DOWN", "STALE", "OPEN"}:
        return DRAIN_OFFLINE
    return text or DRAIN_ACTIVE


def provider_ready_for_new_request(record: Any, *, now_ms_value: int | None = None) -> bool:
    """Return whether a provider should receive a new request now."""

    if isinstance(record, ServiceDiscoveryRecord):
        return record.ready_for_new_request(now_ms_value=now_ms_value)
    if isinstance(record, ProviderCapabilityHint):
        return record.ready_for_new_request and record.is_fresh(now_ms_value=now_ms_value)
    if isinstance(record, NdnsdProviderState):
        return ServiceDiscoveryRecord.from_ndnsd_provider_state(record).ready_for_new_request(
            now_ms_value=now_ms_value)
    if isinstance(record, dict):
        return ServiceDiscoveryRecord.from_dict(record).ready_for_new_request(
            now_ms_value=now_ms_value)
    return False


@dataclass(frozen=True)
class ServiceDiscoveryRecord:
    provider_name: str
    service_name: str = ""
    ready: bool = True
    drain_state: str = DRAIN_ACTIVE
    reason_code: str = ""
    message: str = ""
    last_seen_ms: int = field(default_factory=now_ms)
    freshness_ms: int = 0
    runtime_hint: GenericProviderRuntimeHint | None = None
    capability_hint: ProviderCapabilityHint | None = None
    metadata: dict[str, Any] = field(default_factory=dict)
    source: str = "manual"

    def __post_init__(self) -> None:
        if not self.provider_name:
            raise ValueError("provider_name is required")
        object.__setattr__(self, "drain_state", normalize_drain_state(self.drain_state))

    def is_fresh(self, *, now_ms_value: int | None = None) -> bool:
        if self.freshness_ms <= 0:
            return True
        current = now_ms() if now_ms_value is None else int(now_ms_value)
        return current <= self.last_seen_ms + self.freshness_ms

    def ready_for_new_request(self, *, now_ms_value: int | None = None) -> bool:
        return (
            self.ready and
            self.is_fresh(now_ms_value=now_ms_value) and
            self.drain_state in READY_DRAIN_STATES
        )

    @classmethod
    def from_provider_capability_hint(
        cls,
        hint: ProviderCapabilityHint,
        *,
        source: str = "providerCapabilityHint",
    ) -> "ServiceDiscoveryRecord":
        expires_at_ms = int(hint.expires_at_ms or 0)
        freshness_ms = max(0, expires_at_ms - int(hint.timestamp_ms)) if expires_at_ms else 0
        return cls(
            provider_name=hint.provider_name,
            service_name=hint.service_name,
            ready=hint.ready,
            drain_state=hint.drain_state,
            reason_code=hint.reason_code,
            message=hint.message,
            last_seen_ms=int(hint.timestamp_ms),
            freshness_ms=freshness_ms,
            runtime_hint=hint.runtime_hint,
            capability_hint=hint,
            metadata=dict(hint.service_payload or {}),
            source=source,
        )

    @classmethod
    def from_ndnsd_provider_state(
        cls,
        state: NdnsdProviderState,
        *,
        source: str = "ndnsd",
    ) -> "ServiceDiscoveryRecord":
        metadata = dict(state.meta or {})
        runtime_status = str(metadata.get("runtimeStatus", metadata.get("runtime_status", "ready")))
        drain_state = normalize_drain_state(metadata.get("drainState", runtime_status))
        if runtime_status.lower() in {"overloaded", "degraded"}:
            ready = True
            reason_code = runtime_status.upper()
        elif drain_state in NON_READY_DRAIN_STATES:
            ready = False
            reason_code = drain_state
        else:
            ready = bool(state.is_fresh)
            reason_code = "" if ready else DRAIN_OFFLINE
        return cls(
            provider_name=state.provider,
            service_name=state.service_name,
            ready=ready,
            drain_state=drain_state,
            reason_code=reason_code,
            message=runtime_status,
            last_seen_ms=int(state.publish_timestamp_s * 1000),
            freshness_ms=int(state.service_lifetime_s * 1000),
            metadata=metadata,
            source=source,
        )

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ServiceDiscoveryRecord":
        if "providerCapabilityHint" in payload:
            return cls.from_provider_capability_hint(
                ProviderCapabilityHint.from_dict(dict(payload["providerCapabilityHint"])))
        hint_payload = payload.get("capabilityHint", payload.get("capability_hint"))
        capability_hint = None
        if isinstance(hint_payload, ProviderCapabilityHint):
            capability_hint = hint_payload
        elif isinstance(hint_payload, dict):
            capability_hint = ProviderCapabilityHint.from_dict(hint_payload)
        if capability_hint is not None:
            return cls.from_provider_capability_hint(capability_hint)
        runtime_payload = payload.get("runtimeHint", payload.get("runtime_hint"))
        runtime_hint = None
        if isinstance(runtime_payload, GenericProviderRuntimeHint):
            runtime_hint = runtime_payload
        elif isinstance(runtime_payload, dict) and runtime_payload:
            runtime_hint = GenericProviderRuntimeHint.from_dict(runtime_payload)
        return cls(
            provider_name=str(payload.get("providerName", payload.get(
                "provider_name", payload.get("provider", "")))),
            service_name=str(payload.get("serviceName", payload.get("service_name", ""))),
            ready=bool(payload.get("ready", True)),
            drain_state=payload.get("drainState", payload.get("drain_state", DRAIN_ACTIVE)),
            reason_code=str(payload.get("reasonCode", payload.get("reason_code", ""))),
            message=str(payload.get("message", "")),
            last_seen_ms=int(payload.get("lastSeenMs", payload.get("last_seen_ms", now_ms()))),
            freshness_ms=int(payload.get("freshnessMs", payload.get("freshness_ms", 0)) or 0),
            runtime_hint=runtime_hint,
            metadata=dict(payload.get("metadata", payload.get("meta", {})) or {}),
            source=str(payload.get("source", "dict")),
        )


@dataclass(frozen=True)
class ServiceDiscoverySnapshot:
    service_name: str
    records: tuple[ServiceDiscoveryRecord, ...] = ()
    observed_at_ms: int = field(default_factory=now_ms)

    @classmethod
    def from_records(
        cls,
        service_name: str,
        records: Iterable[ServiceDiscoveryRecord | ProviderCapabilityHint | NdnsdProviderState | dict[str, Any]],
    ) -> "ServiceDiscoverySnapshot":
        normalized = tuple(
            item if isinstance(item, ServiceDiscoveryRecord)
            else ServiceDiscoveryRecord.from_provider_capability_hint(item)
            if isinstance(item, ProviderCapabilityHint)
            else ServiceDiscoveryRecord.from_ndnsd_provider_state(item)
            if isinstance(item, NdnsdProviderState)
            else ServiceDiscoveryRecord.from_dict(dict(item))
            for item in records
        )
        return cls(service_name=service_name, records=normalized)

    @property
    def provider_names(self) -> tuple[str, ...]:
        return tuple(record.provider_name for record in self.records)

    def ready_records(self, *, now_ms_value: int | None = None) -> tuple[ServiceDiscoveryRecord, ...]:
        return tuple(
            record for record in self.records
            if record.ready_for_new_request(now_ms_value=now_ms_value)
        )

    def draining_records(self, *, now_ms_value: int | None = None) -> tuple[ServiceDiscoveryRecord, ...]:
        return tuple(
            record for record in self.records
            if record.is_fresh(now_ms_value=now_ms_value) and
            record.drain_state in NON_READY_DRAIN_STATES
        )

    def stale_records(self, *, now_ms_value: int | None = None) -> tuple[ServiceDiscoveryRecord, ...]:
        return tuple(
            record for record in self.records
            if not record.is_fresh(now_ms_value=now_ms_value)
        )

    def unavailable_records(self, *, now_ms_value: int | None = None) -> tuple[ServiceDiscoveryRecord, ...]:
        return tuple(
            record for record in self.records
            if not record.ready_for_new_request(now_ms_value=now_ms_value)
        )

