#!/usr/bin/env python3
"""Export NDNSF network telemetry logs as a NativeTracer network profile."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any


TOKEN_RE = re.compile(r"(\w+)=([^ \n\r\t]+)")


def producer_from_data_name(data_name: str) -> str:
    marker = "/NDNSF/DI/ACTIVATION/"
    if marker in data_name:
        return data_name.split(marker, 1)[0]
    marker = "/NDNSF/"
    if marker in data_name:
        return data_name.split(marker, 1)[0]
    return ""


def parse_line(line: str, current_identity: str = "") -> dict[str, str] | None:
    if "NDNSF_NETWORK_TELEMETRY" not in line:
        if "NDNSF_DI_DEPENDENCY_INPUT_TIMING" not in line:
            return None
        fields = dict(TOKEN_RE.findall(line))
        data_name = fields.get("data_name", "")
        fetch_ms = fields.get("fetch_ms", fields.get("prefetch_total_ms", "0"))
        bytes_count = fields.get("bytes", "0")
        try:
            goodput = float(bytes_count) * 8.0 / (float(fetch_ms) * 1000.0)
        except (ValueError, ZeroDivisionError):
            goodput = 0.0
        return {
            "event": "large_data_fetch",
            "sample_kind": "large-data-fetch",
            "consumerProvider": current_identity,
            "producerProvider": producer_from_data_name(data_name),
            "keyScope": fields.get("scope", ""),
            "dataName": data_name,
            "elapsed_ms": fetch_ms,
            "first_byte_ms": fetch_ms,
            "encoded_bytes": bytes_count,
            "wire_bytes": bytes_count,
            "received_segments": fields.get(
                "planned_segment_count",
                fields.get("expected_segments", "0")),
            "segment_timeouts": "0",
            "nacks": "0",
            "goodput_mbps": str(goodput),
            "source": "ndnsf-di-dependency-input-timing",
        }
    fields = dict(TOKEN_RE.findall(line))
    if fields.get("event") != "large_data_fetch":
        return None
    fields.setdefault("source", "ndnsf-network-telemetry")
    return fields


def as_float(fields: dict[str, str], key: str) -> float:
    try:
        return float(fields.get(key, "0") or 0)
    except ValueError:
        return 0.0


def as_int(fields: dict[str, str], key: str) -> int:
    try:
        return int(float(fields.get(key, "0") or 0))
    except ValueError:
        return 0


def confidence(sample_count: int, timeout_count: int, nack_count: int,
               received_segments: int) -> float:
    base = min(1.0, sample_count / 5.0)
    failures = timeout_count + nack_count
    signals = failures + received_segments
    if signals > 0 and failures > 0:
        base *= max(0.0, 1.0 - failures / signals)
    return base


def export_profile(log_paths: list[Path],
                   min_confidence: float = 0.0) -> dict[str, Any]:
    buckets: dict[tuple[str, str, str], list[dict[str, str]]] = defaultdict(list)
    for path in log_paths:
        current_identity = ""
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "NDNSF_DI_NATIVE_PROVIDER_START" in line:
                start_fields = dict(TOKEN_RE.findall(line))
                current_identity = start_fields.get("identity", current_identity)
            fields = parse_line(line, current_identity)
            if not fields:
                continue
            key = (
                fields.get("consumerProvider", ""),
                fields.get("producerProvider", ""),
                fields.get("keyScope", ""),
            )
            buckets[key].append(fields)

    edges: list[dict[str, Any]] = []
    pair_overrides: list[dict[str, Any]] = []
    for (consumer, producer, key_scope), samples in sorted(buckets.items()):
        sample_count = len(samples)
        elapsed_ms = sum(as_float(item, "elapsed_ms") for item in samples) / sample_count
        first_byte_ms = sum(as_float(item, "first_byte_ms") for item in samples) / sample_count
        goodput_mbps = sum(as_float(item, "goodput_mbps") for item in samples) / sample_count
        wire_bytes = sum(as_int(item, "wire_bytes") for item in samples)
        encoded_bytes = sum(as_int(item, "encoded_bytes") for item in samples)
        received_segments = sum(as_int(item, "received_segments") for item in samples)
        timeouts = sum(as_int(item, "segment_timeouts") for item in samples)
        nacks = sum(as_int(item, "nacks") for item in samples)
        conf = confidence(sample_count, timeouts, nacks, received_segments)
        edge = {
            "consumerProvider": consumer,
            "producerProvider": producer,
            "keyScope": key_scope,
            "rttMs": first_byte_ms,
            "elapsedMs": elapsed_ms,
            "goodputMbps": goodput_mbps,
            "sampleCount": sample_count,
            "confidence": conf,
            "wireBytes": wire_bytes,
            "encodedBytes": encoded_bytes,
            "receivedSegments": received_segments,
            "timeoutCount": timeouts,
            "nackCount": nacks,
        }
        edges.append(edge)
        if conf >= min_confidence and producer and consumer and goodput_mbps > 0:
            pair_overrides.append({
                "from": producer,
                "to": consumer,
                "rttMs": first_byte_ms,
                "bandwidthMbps": goodput_mbps,
                "confidence": conf,
                "sampleCount": sample_count,
                "source": "ndnsf-network-telemetry",
                "keyScope": key_scope,
            })

    return {
        "schema": "ndnsf-dynamic-network-profile-v1",
        "edges": edges,
        "pairOverrides": pair_overrides,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", "-o", type=Path, required=True)
    parser.add_argument("--min-confidence", type=float, default=0.0)
    args = parser.parse_args()

    profile = export_profile(args.logs, args.min_confidence)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(f"wrote {args.output} edges={len(profile['edges'])} "
          f"pairOverrides={len(profile['pairOverrides'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
