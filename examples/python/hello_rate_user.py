#!/usr/bin/env python3
"""Open-loop Python HELLO user for MiniNDN performance smoke tests."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import os
import statistics
import sys
import threading
import time

from common import add_process_arguments, optional_local_nfd, print_commands, session_kwargs  # noqa: E402

from ndnsf import ServiceResponse, ServiceUser  # noqa: E402


@dataclass
class RateResult:
    offered: float
    sent: int
    success: int
    timeout: int
    failed: int
    avg_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    duration_s: float

    @property
    def actual_rps(self) -> float:
        return self.sent / self.duration_s if self.duration_s > 0 else 0.0

    @property
    def success_rate(self) -> float:
        return (self.success / self.sent * 100.0) if self.sent else 0.0


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int((len(ordered) - 1) * pct)
    return ordered[index]


def run_rate(
    user: ServiceUser,
    *,
    rate: float,
    duration_s: float,
    ack_timeout_ms: int,
    timeout_ms: int,
    strategy: str,
    drain_s: float,
) -> RateResult:
    lock = threading.Lock()
    all_done = threading.Event()
    sent = 0
    success = 0
    timeout = 0
    failed = 0
    latencies: list[float] = []
    pending = 0

    def mark_done() -> None:
        nonlocal pending
        pending -= 1
        if pending <= 0:
            all_done.set()

    interval = 1.0 / rate
    start = time.monotonic()
    end = start + duration_s
    next_send = start
    while time.monotonic() < end:
        now = time.monotonic()
        if now < next_send:
            time.sleep(min(0.001, next_send - now))
            continue

        send_time = time.monotonic()
        request_no = sent
        payload = f"HELLO:{request_no}".encode()
        with lock:
            sent += 1
            pending += 1
            all_done.clear()

        def on_response(response: ServiceResponse, issued_at=send_time) -> None:
            nonlocal success, failed
            elapsed_ms = (time.monotonic() - issued_at) * 1000.0
            with lock:
                if response.status:
                    success += 1
                    latencies.append(elapsed_ms)
                else:
                    failed += 1
                mark_done()

        def on_timeout(_request_id: str) -> None:
            nonlocal timeout
            with lock:
                timeout += 1
                mark_done()

        user.request_service_async(
            "/HELLO",
            payload,
            on_response=on_response,
            on_timeout=on_timeout,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            strategy=strategy,
        )
        next_send += interval

    actual_duration = time.monotonic() - start
    all_done.wait(timeout=drain_s)
    with lock:
        avg = statistics.mean(latencies) if latencies else 0.0
        return RateResult(
            offered=rate,
            sent=sent,
            success=success,
            timeout=timeout,
            failed=failed,
            avg_ms=avg,
            p50_ms=percentile(latencies, 0.50),
            p95_ms=percentile(latencies, 0.95),
            p99_ms=percentile(latencies, 0.99),
            duration_s=actual_duration,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run open-loop Python HELLO user")
    parser.add_argument("--rate-series", default="10,30,50,70,100,150")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--drain-seconds", type=float, default=10.0)
    parser.add_argument("--ack-timeout-ms", type=int, default=300)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--strategy", default="first-responding")
    parser.add_argument("--csv", default="")
    add_process_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run:
        print_commands([[
            "python",
            "examples/python/hello_rate_user.py",
            "--rate-series", args.rate_series,
            "--duration", str(args.duration),
            "--strategy", args.strategy,
        ]])
        return 0

    rates = [float(item.strip()) for item in args.rate_series.split(",") if item.strip()]
    rows = []
    with optional_local_nfd(args.start_local_nfd):
        user = ServiceUser(**session_kwargs(args))
        user.start()
        try:
            for rate in rates:
                result = run_rate(
                    user,
                    rate=rate,
                    duration_s=args.duration,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    strategy=args.strategy,
                    drain_s=args.drain_seconds,
                )
                rows.append(result)
                print(
                    "PY_RATE_SUMMARY "
                    f"offered={result.offered:g} actual={result.actual_rps:.2f} "
                    f"success={result.success_rate:.2f} avg={result.avg_ms:.2f} "
                    f"p50={result.p50_ms:.2f} p95={result.p95_ms:.2f} "
                    f"p99={result.p99_ms:.2f} timeout={result.timeout} "
                    f"sent={result.sent}",
                    flush=True,
                )
        finally:
            user.stop()

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as output:
            writer = csv.writer(output)
            writer.writerow([
                "offered_rps", "actual_rps", "success_rate", "avg_ms",
                "p50_ms", "p95_ms", "p99_ms", "timeout", "sent", "success",
                "failed",
            ])
            for result in rows:
                writer.writerow([
                    f"{result.offered:g}", f"{result.actual_rps:.4f}",
                    f"{result.success_rate:.4f}", f"{result.avg_ms:.4f}",
                    f"{result.p50_ms:.4f}", f"{result.p95_ms:.4f}",
                    f"{result.p99_ms:.4f}", result.timeout, result.sent,
                    result.success, result.failed,
                ])
    return 0


if __name__ == "__main__":
    exit_code = main()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(exit_code)
