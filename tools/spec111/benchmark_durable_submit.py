#!/usr/bin/env python3
"""Bounded local benchmark for the crash-durable APP request path."""

from __future__ import annotations

import argparse
from concurrent.futures import Future
import json
from pathlib import Path
import statistics
import sys
import tempfile
import time
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [
    str(ROOT / "NDNSF-DistributedInference"),
    str(ROOT / "pythonWrapper"),
]

from ndnsf_distributed_inference.app_sdk import APPClient, RuntimeJournal


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    position = max(0, min(len(ordered) - 1, int(
        fraction * len(ordered) + 0.999999) - 1))
    return ordered[position]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--requests", type=int, default=300)
    parser.add_argument("--edge-window", type=int, default=50)
    parser.add_argument("--max-drift-percent", type=float, default=15.0)
    parser.add_argument("--max-p50-ms", type=float, default=10.0)
    parser.add_argument("--max-p95-ms", type=float, default=20.0)
    args = parser.parse_args()
    if (args.warmup < 0 or args.requests <= 0 or args.edge_window <= 0 or
            args.requests < 2 * args.edge_window):
        raise SystemExit("invalid benchmark sample/window configuration")

    network = SimpleNamespace(
        encode_input=lambda service, value: bytes(value),
    )

    def submit_network(service, payload, **options):
        result = SimpleNamespace(
            status=True,
            payload=payload,
            error="",
            request_id=options["request_id"],
            data_name=f"/provider/NDNSF/RESPONSE/{options['request_id']}",
            signer_certificate="/provider/KEY/cert",
            wire_digest="sha256:" + "a" * 64,
        )
        options["on_result"](result)
        future = Future()
        future.set_result(result)
        return future

    network.async_distributed_inference = submit_network
    samples: list[float] = []
    with tempfile.TemporaryDirectory(prefix="spec111-durable-benchmark-") as root:
        client = APPClient(
            RuntimeJournal.for_test(root, "benchmark-requester"),
            network_client=network,
            requester_identity="/benchmark/requester",
        )
        for index in range(args.warmup + args.requests):
            started = time.perf_counter_ns()
            result = client.distributed_inference(
                "/benchmark/service",
                b"benchmark-payload",
                deployment_revision="sha256:" + "b" * 64,
                timeout_ms=1_000,
            )
            elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
            if not result.status or result.payload != b"benchmark-payload":
                raise SystemExit("durable benchmark request failed")
            if index >= args.warmup:
                samples.append(elapsed_ms)

        first = statistics.median(samples[:args.edge_window])
        last = statistics.median(samples[-args.edge_window:])
        drift = abs(last - first) / min(first, last) * 100.0
        p50 = statistics.median(samples)
        p95 = percentile(samples, 0.95)
        status = (
            "PASS" if drift <= args.max_drift_percent and
            p50 <= args.max_p50_ms and p95 <= args.max_p95_ms else "FAIL"
        )
        report = {
            "schema": "ndnsf-di-spec111-durable-submit-microbenchmark-v1",
            "status": status,
            "warmupRequests": args.warmup,
            "measuredRequests": len(samples),
            "edgeWindow": args.edge_window,
            "p50Ms": round(p50, 6),
            "p95Ms": round(p95, 6),
            "firstWindowMedianMs": round(first, 6),
            "lastWindowMedianMs": round(last, 6),
            "edgeDriftPercent": round(drift, 6),
            "bounds": {
                "maxDriftPercent": args.max_drift_percent,
                "maxP50Ms": args.max_p50_ms,
                "maxP95Ms": args.max_p95_ms,
            },
            "journalUsageBytes": client.journal.usage_bytes(),
            "temporaryStateRemovedOnExit": True,
        }
        print(json.dumps(report, sort_keys=True))
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
