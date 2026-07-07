"""Merge Provider — per-deployment lifecycle authority.

The Merge role (terminal pipeline stage) is the natural authority for a
deployment's state — it sees every request, knows when ref_count hits
zero, and has NDNSD write access (as a ServiceProvider).

No central coordinator is needed.  Each deployment's Merge role manages
its own lifecycle.  Multiple deployments = multiple Merge Providers (one
per deployment), naturally distributed.
"""

from __future__ import annotations

import json
import threading
import time
from typing import Any

from ndnsf.runtime_telemetry import (
    DeploymentStatus,
    now_ms,
)


class DeploymentManager:
    """Manages deployment lifecycle for the local Merge role.

    One instance per deployment.  Handles state transitions, ref_count
    tracking, and NDNSD publishing.
    """

    def __init__(self,
                 provider: Any,          # ServiceProvider (has NDNSD write)
                 deployment_id: str,
                 service_name: str = "",
                 plan_id: str = "",
                 idle_timeout_s: int = 300):
        self.provider = provider
        self.deployment_id = deployment_id
        self.service_name = service_name
        self.plan_id = plan_id
        self.idle_timeout_s = idle_timeout_s

        self._ref_count = 0
        self._status = "PROVISIONING"
        self._fragment_map: dict[str, list[dict[str, Any]]] = {}
        self._created_at_ms = now_ms()
        self._updated_at_ms = self._created_at_ms
        self._lock = threading.Lock()

    # ── Request tracking (called by pipeline handler) ──

    def request_start(self) -> None:
        with self._lock:
            self._ref_count += 1
            self._touch()

    def request_end(self) -> None:
        with self._lock:
            self._ref_count = max(0, self._ref_count - 1)
            self._touch()

    # ── State management ──

    def set_ready(self, fragment_map: dict[str, list[dict[str, Any]]]) -> None:
        with self._lock:
            self._fragment_map = fragment_map
            self._status = "ACTIVE"
            self._touch()

    def set_degraded(self, reason: str = "") -> None:
        with self._lock:
            self._status = "DEGRADED"
            self._touch()

    def mark_provisioning(self) -> None:
        with self._lock:
            self._status = "PROVISIONING"
            self._touch()

    # ── Auto-transition ──

    def auto_transition(self) -> str | None:
        """Check and apply auto state transitions.  Returns new status or None."""
        with self._lock:
            now = now_ms()
            age_ms = now - self._updated_at_ms
            changed = None

            if self._status == "PROVISIONING":
                if self._fragment_map and age_ms > 15000:
                    self._status = "ACTIVE"
                    changed = "ACTIVE"
            elif self._status == "ACTIVE" and self._ref_count == 0:
                if age_ms > self.idle_timeout_s * 1000:
                    self._status = "IDLE"
                    changed = "IDLE"

            if changed:
                self._touch(now)
            return changed

    # ── Snapshot ──

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "deploymentId": self.deployment_id,
                "planId": self.plan_id,
                "serviceName": self.service_name,
                "status": self._status,
                "fragmentMap": self._fragment_map,
                "refCount": self._ref_count,
                "idleTimeoutS": self.idle_timeout_s,
                "createdAtMs": self._created_at_ms,
                "updatedAtMs": self._updated_at_ms,
            }

    # ── NDNSD publish ──

    def publish(self, all_deployments: list[dict[str, Any]]) -> None:
        """Publish this deployment's state into the NDNSD deployments list."""
        try:
            self.provider.update_ndnsd_meta(
                "deployments",
                json.dumps(all_deployments, sort_keys=True))
        except Exception:
            pass

    # ── Internal ──

    def _touch(self, when_ms: int = 0) -> None:
        self._updated_at_ms = when_ms or now_ms()
