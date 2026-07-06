"""NDNSD-based health scoring and circuit breaker for NDNSF core.

Leverages the NDNSD service-discovery bus (SVS-synchronized) to maintain
provider liveness, capacity, and health state without polling or extra
wire protocols.

Usage::

    health = NdnsdHealthTracker()
    health.update_from_ndnsd(user.get_ndnsd_services())
    score = health.health_score("/Provider/backbone")
    if health.circuit_breaker_state("/Provider/backbone") == "OPEN":
        ...  # skip this provider
"""

from __future__ import annotations

from dataclasses import dataclass, field
import time
from typing import Any


def _now_ms() -> int:
    return int(time.time() * 1000)


@dataclass
class NdnsdProviderState:
    provider: str = ""
    service_name: str = ""
    service_lifetime_s: int = 30
    publish_timestamp_s: int = 0
    meta: dict[str, str] = field(default_factory=dict)

    @property
    def age_s(self) -> float:
        return time.time() - float(self.publish_timestamp_s)

    @property
    def is_fresh(self) -> bool:
        return self.service_lifetime_s > 0 and self.age_s < self.service_lifetime_s

    @property
    def freshness_ratio(self) -> float:
        """1.0 = just published; 0.0 = at or past TTL."""
        if self.service_lifetime_s <= 0:
            return 0.0
        return max(0.0, 1.0 - self.age_s / self.service_lifetime_s)

    def meta_int(self, key: str, default: int = 0) -> int:
        try:
            return int(self.meta.get(key, str(default)))
        except (ValueError, TypeError):
            return default

    def meta_str(self, key: str, default: str = "") -> str:
        return str(self.meta.get(key, default))


class NdnsdHealthTracker:
    """Tracks provider health from NDNSD service discovery updates.

    Providers are scored on freshness, capacity, and stability.
    A circuit breaker opens when a provider misses its TTL.
    """

    def __init__(self,
                 *,
                 stale_threshold_ratio: float = 0.8,
                 circuit_open_after_missed: int = 2,
                 half_open_probe_after_s: float = 5.0):
        self._providers: dict[str, NdnsdProviderState] = {}
        self._missed_heartbeats: dict[str, int] = {}
        self._circuit_state: dict[str, str] = {}
        self._circuit_changed_at_ms: dict[str, int] = {}
        self.stale_threshold_ratio = float(stale_threshold_ratio)
        self.circuit_open_after_missed = int(circuit_open_after_missed)
        self.half_open_probe_after_s = float(half_open_probe_after_s)

    # ------------------------------------------------------------------
    # Update
    # ------------------------------------------------------------------

    def update_from_ndnsd(self, services: list[dict[str, Any]]) -> None:
        """Ingest NDNSD service details from ``ServiceUser.get_ndnsd_services()``."""
        current_ms = _now_ms()
        seen: set[str] = set()
        for entry in services:
            provider = str(entry.get("provider", ""))
            if not provider:
                continue
            seen.add(provider)
            state = NdnsdProviderState(
                provider=provider,
                service_name=str(entry.get("serviceName", "")),
                service_lifetime_s=int(entry.get("serviceLifetime", 30)),
                publish_timestamp_s=int(entry.get("publishTimestamp", 0)),
                meta={str(k): str(v) for k, v in entry.get("serviceMetaInfo", {}).items()},
            )
            previous = self._providers.get(provider)
            self._providers[provider] = state
            if previous is None:
                self._missed_heartbeats[provider] = 0
                self._circuit_state[provider] = "CLOSED"
                self._circuit_changed_at_ms[provider] = current_ms
            if state.is_fresh:
                self._missed_heartbeats[provider] = 0
                if self._circuit_state.get(provider) == "OPEN":
                    if self._can_half_open(provider, current_ms):
                        self._circuit_state[provider] = "HALF_OPEN"
                        self._circuit_changed_at_ms[provider] = current_ms
            else:
                self._missed_heartbeats[provider] = self._missed_heartbeats.get(provider, 0) + 1
                missed = self._missed_heartbeats[provider]
                if missed >= self.circuit_open_after_missed:
                    self._circuit_state[provider] = "OPEN"
                    self._circuit_changed_at_ms[provider] = current_ms
        for provider in list(self._providers):
            if provider not in seen:
                self._missed_heartbeats[provider] = self._missed_heartbeats.get(provider, 0) + 1
                if self._missed_heartbeats[provider] >= self.circuit_open_after_missed:
                    self._circuit_state[provider] = "OPEN"
                    self._circuit_changed_at_ms[provider] = current_ms

    # ------------------------------------------------------------------
    # Health scoring
    # ------------------------------------------------------------------

    def health_score(self, provider: str) -> float:
        """Return 0.0 (worst) to 1.0 (best) health score for a provider.

        Unknown providers (never seen on NDNSD) default to 1.0 — they may
        simply not have NDNSD enabled. Only providers that have published
        and then went stale get degraded scores.
        """
        state = self._providers.get(provider)
        if state is None:
            return 1.0  # never seen → assume healthy
        freshness = state.freshness_ratio
        idle_workers = state.meta_int("idleWorkers", -1)
        capacity_score = 0.5
        if idle_workers >= 0:
            workers = state.meta_int("workers", 1)
            capacity_score = min(1.0, idle_workers / max(1, workers))
        runtime_status = state.meta_str("runtimeStatus", "ready")
        status_score = 1.0
        if runtime_status in {"overloaded", "degraded"}:
            status_score = 0.3
        elif runtime_status in {"draining", "provisioning"}:
            status_score = 0.5
        return freshness * 0.4 + capacity_score * 0.35 + status_score * 0.25

    def circuit_breaker_state(self, provider: str) -> str:
        """Return CLOSED, OPEN, or HALF_OPEN for a provider.

        Unknown providers default to CLOSED (they may not have NDNSD enabled).
        """
        state = self._circuit_state.get(provider, "CLOSED")
        if state == "OPEN":
            if self._can_half_open(provider, _now_ms()):
                return "HALF_OPEN"
        return state

    def is_available(self, provider: str) -> bool:
        """Return True if the provider should receive requests."""
        cb = self.circuit_breaker_state(provider)
        return cb != "OPEN"

    # ------------------------------------------------------------------
    # Capacity helpers
    # ------------------------------------------------------------------

    def provider_capacity(self, provider: str) -> dict[str, Any]:
        """Return observed capacity from NDNSD meta info."""
        state = self._providers.get(provider)
        if state is None:
            return {}
        return {
            "provider": provider,
            "freshnessRatio": state.freshness_ratio,
            "queue": state.meta_int("queue"),
            "activeWorkers": state.meta_int("activeWorkers"),
            "idleWorkers": state.meta_int("idleWorkers"),
            "workers": state.meta_int("workers"),
            "runtimeStatus": state.meta_str("runtimeStatus", "ready"),
            "fragmentDigest": state.meta_str("fragmentDigest"),
            "readyCostMs": state.meta_int("readyCostMs"),
        }

    def all_providers(self) -> list[str]:
        return sorted(self._providers)

    def snapshot(self) -> dict[str, Any]:
        return {
            "providerCount": len(self._providers),
            "providers": {
                provider: {
                    "state": self.circuit_breaker_state(provider),
                    "healthScore": round(self.health_score(provider), 4),
                    "missedHeartbeats": self._missed_heartbeats.get(provider, 0),
                    "capacity": self.provider_capacity(provider),
                }
                for provider in sorted(self._providers)
            },
        }

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _can_half_open(self, provider: str, current_ms: int) -> bool:
        changed_at = self._circuit_changed_at_ms.get(provider, 0)
        return current_ms - changed_at > int(self.half_open_probe_after_s * 1000)
