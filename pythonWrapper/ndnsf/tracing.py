"""Distributed tracing for NDNSF using requestId as natural trace ID.

The NDN request name is already globally unique and flows through the
entire pipeline (REQUEST → ACK → SELECTION → EXECUTION → RESPONSE).
This module collects provider lifecycle events and exports Jaeger-compatible
JSON traces.
"""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any


def _now_us() -> int:
    return int(time.time() * 1_000_000)


class Span:
    def __init__(self, operation: str, trace_id: str, span_id: str,
                 parent_span_id: str = "", start_us: int = 0):
        self.operation = operation
        self.trace_id = trace_id
        self.span_id = span_id
        self.parent_span_id = parent_span_id
        self.start_us = start_us or _now_us()
        self.finish_us: int = 0
        self.tags: dict[str, str] = {}
        self.logs: list[dict[str, Any]] = []

    def set_tag(self, key: str, value: str) -> None:
        self.tags[key] = value

    def log(self, message: str) -> None:
        self.logs.append({"timestamp": _now_us(), "message": message})

    def finish(self, finish_us: int = 0) -> None:
        self.finish_us = finish_us or _now_us()

    def to_jaeger(self, service_name: str) -> dict[str, Any]:
        duration_us = max(0, self.finish_us - self.start_us)
        return {
            "traceID": self.trace_id,
            "spanID": self.span_id,
            "parentSpanID": self.parent_span_id or "",
            "operationName": self.operation,
            "startTime": self.start_us,
            "duration": duration_us,
            "tags": [{"key": k, "type": "string", "value": v} for k, v in self.tags.items()],
            "logs": [{"timestamp": l["timestamp"],
                      "fields": [{"key": "message", "type": "string", "value": l["message"]}]}
                     for l in self.logs],
            "process": {"serviceName": service_name},
        }


class TraceCollector:
    """Collect spans across providers and export as Jaeger JSON."""

    def __init__(self, service_name: str = "ndnsf"):
        self.service_name = service_name
        self._spans: dict[str, Span] = {}

    def start_span(self, operation: str, trace_id: str, *,
                   span_id: str = "", parent_span_id: str = "",
                   provider: str = "", role: str = "") -> Span:
        sid = span_id or f"{trace_id}-{len(self._spans)}"
        span = Span(operation, trace_id, sid, parent_span_id)
        if provider:
            span.set_tag("provider", provider)
        if role:
            span.set_tag("role", role)
        self._spans[sid] = span
        return span

    def finish_span(self, span_id: str, *, finish_us: int = 0) -> None:
        span = self._spans.get(span_id)
        if span:
            span.finish(finish_us)

    def from_provider_lifecycle(self,
                                 lifecycle: dict[str, Any],
                                 trace_id: str,
                                 provider: str,
                                 service: str) -> None:
        """Create spans from a ProviderRequestLifecycleStatus dict."""
        # REQUEST → ACK span
        ack_id = f"{trace_id}-ack"
        ack = self.start_span("ACK", trace_id, span_id=ack_id, provider=provider, role="ack")
        ack.set_tag("service", service)
        ack.start_us = int(lifecycle.get("requestObservedTimestampUs", 0))
        ack.finish(int(lifecycle.get("ackPublishedOrSuppressedTimestampUs", 0)))

        # SELECTION → EXECUTION span
        exec_id = f"{trace_id}-exec"
        exec_span = self.start_span("EXECUTION", trace_id, span_id=exec_id,
                                     parent_span_id=ack_id, provider=provider, role="execute")
        exec_span.start_us = int(lifecycle.get("selectionReceivedTimestampUs", 0))
        exec_span.finish(int(lifecycle.get("responsePublishedTimestampUs", 0)))
        exec_span.set_tag("finalStatus", str(lifecycle.get("finalStatus", "")))

    def to_jaeger_traces(self) -> list[dict[str, Any]]:
        return [s.to_jaeger(self.service_name) for s in self._spans.values() if s.finish_us > 0]

    def write_jaeger(self, path: str | Path) -> None:
        payload = {
            "data": self.to_jaeger_traces(),
        }
        Path(path).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
