"""Prometheus-compatible metrics for NDNSF.

Usage::

    from ndnsf.metrics import NdnMetrics
    metrics = NdnMetrics()
    metrics.request_total.labels(service="/Inference/NativeTracer", status="success").inc()
    metrics.lease_granted.labels(provider="/P/backbone").inc()
    ...
    print(metrics.dumps())  # Prometheus text format
"""

from __future__ import annotations

import threading
from typing import Any


class _Metric:
    def __init__(self, name: str, help_text: str, metric_type: str,
                 label_names: tuple[str, ...] = ()):
        self.name = name
        self.help = help_text
        self.metric_type = metric_type
        self.label_names = label_names
        self._values: dict[tuple[str, ...], float] = {}
        self._lock = threading.Lock()

    def labels(self, **kwargs: str) -> "_BoundMetric":
        return _BoundMetric(self, tuple(kwargs.get(k, "") for k in self.label_names))

    def _get(self, label_values: tuple[str, ...]) -> float:
        with self._lock:
            return self._values.get(label_values, 0.0)

    def _set(self, label_values: tuple[str, ...], value: float) -> None:
        with self._lock:
            self._values[label_values] = value

    def _inc(self, label_values: tuple[str, ...], delta: float = 1.0) -> None:
        with self._lock:
            self._values[label_values] = self._values.get(label_values, 0.0) + delta


class _BoundMetric:
    def __init__(self, metric: _Metric, label_values: tuple[str, ...]):
        self._metric = metric
        self._label_values = label_values

    def inc(self, delta: float = 1.0) -> None:
        self._metric._inc(self._label_values, delta)

    def set(self, value: float) -> None:
        self._metric._set(self._label_values, value)


class _Counter(_Metric):
    def __init__(self, name: str, help_text: str, label_names: tuple[str, ...] = ()):
        super().__init__(name, help_text, "counter", label_names)


class _Gauge(_Metric):
    def __init__(self, name: str, help_text: str, label_names: tuple[str, ...] = ()):
        super().__init__(name, help_text, "gauge", label_names)


class _Histogram(_Metric):
    def __init__(self, name: str, help_text: str, label_names: tuple[str, ...] = (),
                 buckets: tuple[float, ...] = (10, 50, 100, 250, 500, 1000, 5000)):
        super().__init__(name, help_text, "histogram", label_names)
        self.buckets = buckets
        self._sums: dict[tuple[str, ...], float] = {}
        self._counts: dict[tuple[str, ...], int] = {}
        self._bucket_counts: dict[tuple[str, ...], dict[float, int]] = {}

    def observe(self, value: float, **labels: str) -> None:
        label_values = tuple(labels.get(k, "") for k in self.label_names)
        with self._lock:
            self._sums[label_values] = self._sums.get(label_values, 0.0) + value
            self._counts[label_values] = self._counts.get(label_values, 0) + 1
            bc = self._bucket_counts.setdefault(label_values, {})
            for b in self.buckets:
                if value <= b:
                    bc[b] = bc.get(b, 0) + 1


class NdnMetrics:
    def __init__(self):
        self.request_total = _Counter(
            "ndnsf_requests_total", "Total service requests.",
            ("service", "status"))
        self.request_duration_ms = _Histogram(
            "ndnsf_request_duration_ms", "Request duration in milliseconds.",
            ("service",))
        self.lease_granted = _Counter(
            "ndnsf_lease_granted_total", "Total granted admission leases.",
            ("provider",))
        self.lease_consumed = _Counter(
            "ndnsf_lease_consumed_total", "Total consumed admission leases.",
            ("provider",))
        self.lease_rejected = _Counter(
            "ndnsf_lease_rejected_total", "Total rejected admission leases.",
            ("provider", "reason"))
        self.ack_queue_depth = _Gauge(
            "ndnsf_ack_queue_depth", "Current ACK queue depth.",
            ("provider",))
        self.ack_idle_workers = _Gauge(
            "ndnsf_ack_idle_workers", "Current idle workers.",
            ("provider",))
        self.retry_total = _Counter(
            "ndnsf_retry_total", "Total retry attempts.",
            ("service",))
        self.circuit_breaker_state = _Gauge(
            "ndnsf_circuit_breaker_state", "Circuit breaker state (0=CLOSED, 1=OPEN, 2=HALF_OPEN).",
            ("provider",))
        self.rate_limited_total = _Counter(
            "ndnsf_rate_limited_total", "Total rate-limited requests.",
            ("service",))
        self.health_score = _Gauge(
            "ndnsf_health_score", "Provider health score 0.0-1.0.",
            ("provider",))

    def dumps(self) -> str:
        lines: list[str] = []
        for attr in dir(self):
            m = getattr(self, attr)
            if not isinstance(m, _Metric):
                continue
            lines.append(f"# HELP {m.name} {m.help}")
            lines.append(f"# TYPE {m.name} {m.metric_type}")
            if isinstance(m, _Histogram):
                for lv, count in m._counts.items():
                    label_str = _label_str(m.label_names, lv)
                    lines.append(f"{m.name}_count{label_str} {count}")
                    lines.append(f"{m.name}_sum{label_str} {m._sums.get(lv, 0.0):.3f}")
                    bc = m._bucket_counts.get(lv, {})
                    inf_count = sum(1 for b, c in bc.items() if c > 0)
                    cum = 0
                    for b in sorted(m.buckets):
                        cum += bc.get(b, 0)
                        lines.append(f"{m.name}_bucket{{le=\"{b}\"}} {cum}")
                    lines.append(f"{m.name}_bucket{{le=\"+Inf\"}} {cum}")
            else:
                for lv, value in sorted(m._values.items()):
                    label_str = _label_str(m.label_names, lv)
                    lines.append(f"{m.name}{label_str} {value:.6g}")
        lines.append("")
        return "\n".join(lines)


def _label_str(names: tuple[str, ...], values: tuple[str, ...]) -> str:
    if not names:
        return ""
    pairs = ",".join(f'{n}="{v}"' for n, v in zip(names, values))
    return "{" + pairs + "}"


# ---------------------------------------------------------------------------
# Lightweight HTTP metrics server
# ---------------------------------------------------------------------------

def start_metrics_server(metrics: NdnMetrics, port: int = 9090,
                         bind: str = "127.0.0.1") -> None:
    """Start a lightweight HTTP server serving /metrics in a daemon thread."""
    import threading
    import socketserver
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class _Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path == "/metrics":
                body = metrics.dumps().encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; version=0.0.4")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/health":
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"OK\n")
            else:
                self.send_response(404)
                self.end_headers()

        def log_message(self, format: str, *args: Any) -> None:
            pass  # suppress logs

    server = HTTPServer((bind, port), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
