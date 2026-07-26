#!/usr/bin/env python3
"""Analyze one immutable Spec 148 predictive UAV MiniNDN cell."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
from pathlib import Path
import re
from statistics import mean
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LATENCY_ANALYZER = ROOT / "Experiments/analyze_stream_latency.py"
SPEC = importlib.util.spec_from_file_location("stream_latency", LATENCY_ANALYZER)
assert SPEC and SPEC.loader
stream_latency = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stream_latency)

FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
TIMESTAMP_RE = re.compile(r"^(\d+(?:\.\d+)?)\s+")
PUSH_RE = re.compile(
    r"STREAM_PUSH .*sequence=(\d+).*wire_sha256=([0-9a-f]{64})"
)
ADMIT_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*STREAM_ITEM_ADMITTED "
    r".*sequence=(\d+).*provenance=([^\s]+).*wire_sha256=([0-9a-f]{64})",
    re.MULTILINE,
)
READY_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*GS_VIDEO_STREAM_READY", re.MULTILINE
)
STOP_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*STREAM_API_STOP role=consumer", re.MULTILINE
)
CORE_RE = re.compile(r"GS_VIDEO_CORE_STATUS state=STOPPED .*$", re.MULTILINE)
PROVIDER_RE = re.compile(r"VIDEO_LIVE_STREAM_CORE_FINAL .*$", re.MULTILINE)
ACTIVE_RE = re.compile(
    r"STREAM_API_ACTIVE role=(provider|consumer) mode=predictive "
    r"stream=([^\s]+) epoch=(\d+)"
)
FAILURE_TOKENS = (
    "VIDEO_AUTHENTICATION_FAILED",
    "VIDEO_PROTECTED_PUBLICATION_FAILED",
    "FRAME_BINDING_REJECT",
    "UAV_VIDEO_PIPELINE backend=gstreamer state=failed",
    "terminate called",
    "Segmentation fault",
    "free(): invalid pointer",
)
OLD_PATH_TOKENS = (
    "mode=mapping-first",
    "NDNSF_UAV_DISCOVERY_MODE",
    "discovery_mode=mapping-first",
)


def fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def integer(values: dict[str, str], key: str) -> int:
    try:
        return int(values.get(key, "0"))
    except ValueError:
        return 0


def last_fields(pattern: re.Pattern[str], text: str) -> dict[str, str]:
    matches = pattern.findall(text)
    if not matches:
        return {}
    value = matches[-1]
    return fields(value if isinstance(value, str) else " ".join(value))


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * q
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    weight = index - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def distribution(values: list[float]) -> dict[str, float | int | None]:
    return {
        "samples": len(values),
        "mean": mean(values) if values else None,
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
    }


def analyze(cell_dir: Path, manifest: dict[str, Any],
            return_code: int) -> dict[str, Any]:
    drone = (cell_dir / "drone.log").read_text(
        encoding="utf-8", errors="replace"
    )
    ground = (cell_dir / "ground-station.log").read_text(
        encoding="utf-8", errors="replace"
    )
    controller = (cell_dir / "controller.log").read_text(
        encoding="utf-8", errors="replace"
    )
    combined = "\n".join((controller, drone, ground))

    ready_values = [float(value) for value in READY_RE.findall(ground)]
    stop_values = [float(value) for value in STOP_RE.findall(ground)]
    ready = ready_values[-1] if ready_values else None
    measurement_start = None if ready is None else ready + 5.0
    measurement_end = stop_values[-1] if stop_values else None
    measurement_seconds = (
        measurement_end - measurement_start
        if measurement_start is not None and measurement_end is not None
        else 0.0
    )

    pushes = {int(sequence): digest for sequence, digest in PUSH_RE.findall(drone)}
    admissions = [
        (float(timestamp), int(sequence), provenance, digest)
        for timestamp, sequence, provenance, digest in ADMIT_RE.findall(ground)
    ]
    measured_admissions = [
        value for value in admissions
        if measurement_start is not None and value[0] >= measurement_start
        and (measurement_end is None or value[0] <= measurement_end)
    ]
    admitted_digests = {sequence: digest for _, sequence, _, digest in admissions}
    wire_mismatches = sorted(
        sequence for sequence, digest in admitted_digests.items()
        if pushes.get(sequence) != digest
    )
    admission_times = [value[0] for value in measured_admissions]
    delivery_gaps_ms = [
        (right - left) * 1000.0
        for left, right in zip(admission_times, admission_times[1:])
    ]

    core = last_fields(CORE_RE, ground)
    provider = last_fields(PROVIDER_RE, drone)
    pushed = len(pushes)
    admitted = len(admitted_digests)
    delivered = integer(core, "delivered")
    payload_interests = integer(core, "payload_interests")
    future_interests = integer(provider, "provider_future_interests")
    future_hits = integer(provider, "provider_future_hits")
    useful = integer(core, "payload_application_useful_interests")
    protection_only = integer(core, "payload_protection_only_interests")
    nonproductive = integer(core, "payload_nonproductive_interests")
    unresolved = integer(core, "payload_unresolved_interests")
    useless = protection_only + nonproductive
    classified = useful + useless
    recovery_attempts = integer(core, "recovery_attempts")
    recovered = integer(core, "recovered")

    latency = stream_latency.analyze_texts(
        [drone, ground], warmup_ms=5000, shared_monotonic_clock=True
    )
    exact = latency["exactFrameTimeline"]
    end_to_end = exact["captureToDecodeMs"]
    active = ACTIVE_RE.findall(combined)
    roles = {role for role, _, _ in active}
    stream_keys = {(stream, epoch) for _, stream, epoch in active}
    failures = {
        token: combined.count(token)
        for token in FAILURE_TOKENS if token in combined
    }
    old_path = {
        token: combined.count(token)
        for token in OLD_PATH_TOKENS if token in combined
    }
    required_files = (
        "controller.log", "drone.log", "ground-station.log",
        "nfd-pit-samples.csv", "experiment-netem-before-apps.json",
        "experiment-netem-final.json",
    )
    missing_files = [
        name for name in required_files if not (cell_dir / name).is_file()
    ]

    metrics = {
        "attempted": pushed + sum(failures.get(token, 0)
                                  for token in ("VIDEO_PROTECTED_PUBLICATION_FAILED",)),
        "pushed": pushed,
        "flushed": drone.count("STREAM_FLUSH "),
        "admitted": admitted,
        "delivered": delivered,
        "deliveryRatio": delivered / pushed if pushed else 0.0,
        "measuredAdmissions": len(measured_admissions),
        "endToEndAoIMs": {
            "samples": end_to_end.get("samples", 0),
            "mean": end_to_end.get("mean"),
            "p50": end_to_end.get("p50"),
            "p95": end_to_end.get("p95"),
            "p99": end_to_end.get("p99"),
        },
        "deliveryInterarrivalMs": distribution(delivery_gaps_ms),
        "longestDeliveryGapMs": max(delivery_gaps_ms, default=0.0),
        "futureInterests": future_interests,
        "futureHits": future_hits,
        "futureHitRatio": future_hits / future_interests
        if future_interests else 0.0,
        "mappingInterests": integer(core, "mapping_interests"),
        "payloadInterests": payload_interests,
        "separatePayloadLookupInterests": 0,
        "retryAttempts": integer(core, "retry_attempts"),
        "timeouts": integer(core, "timeouts"),
        "nacks": integer(core, "nacks"),
        "repairAttempts": recovery_attempts,
        "recoveries": recovered,
        "recoveryRatio": recovered / recovery_attempts
        if recovery_attempts else None,
        "terminalGaps": integer(core, "terminal_missing_sources"),
        "uselessInterests": useless,
        "unresolvedInterests": unresolved,
        "uselessInterestRatio": useless / classified if classified else 0.0,
    }
    checks = {
        "processExitedNormally": return_code == 0,
        "requiredArtifactsPresent": not missing_files,
        "measurementWindowAtLeast60Seconds": measurement_seconds >= 60.0,
        "providerAndConsumerActive": roles == {"provider", "consumer"},
        "singleStreamSession": len(stream_keys) == 1,
        "providerPushFlushStop": (
            pushed > 0 and metrics["flushed"] > 0
            and "STREAM_API_STOP role=provider" in drone
        ),
        "consumerFutureAdmissionStop": (
            "STREAM_FUTURE_INTEREST" in ground
            and admitted > 0 and "STREAM_API_STOP role=consumer" in ground
        ),
        "decodedVideo": "AUTO_VIDEO_GUI_RENDER_GATE status=PASS" in ground,
        "noFailureToken": not failures,
        "noOldPathMarker": not old_path,
        "exactWireIdentity": admitted > 0 and not wire_mismatches,
        "zeroMappingLookupInterests": metrics["mappingInterests"] == 0,
        "futureHitObserved": future_hits > 0,
        "nonzeroDelivery": delivered > 0,
        "completeLatencyDistribution": (
            int(metrics["endToEndAoIMs"]["samples"] or 0) > 0
            and all(metrics["endToEndAoIMs"][key] is not None
                    for key in ("mean", "p50", "p95", "p99"))
        ),
    }
    if manifest["profile"]["lossPercent"] == 0:
        checks["zeroLossDeliveryAtLeast98Percent"] = (
            metrics["deliveryRatio"] >= 0.98
        )

    return {
        "schemaVersion": "spec148-predictive-uav-cell-v1",
        "cellId": manifest["cellId"],
        "profile": manifest["profile"],
        "returnCode": return_code,
        "measurement": {
            "readyTimestamp": ready,
            "warmupSeconds": 5.0,
            "startTimestamp": measurement_start,
            "endTimestamp": measurement_end,
            "seconds": measurement_seconds,
        },
        "metrics": metrics,
        "wireMismatchSequences": wire_mismatches,
        "failureCounts": failures,
        "oldPathCounts": old_path,
        "missingArtifacts": missing_files,
        "latencyAnalysis": latency,
        "checks": checks,
        "accepted": all(checks.values()),
    }


def write_outputs(cell_dir: Path, summary: dict[str, Any]) -> None:
    (cell_dir / "cell-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    metrics = summary["metrics"]
    row = {
        "cell_id": summary["cellId"],
        "accepted": summary["accepted"],
        "measurement_seconds": summary["measurement"]["seconds"],
        "pushed": metrics["pushed"],
        "delivered": metrics["delivered"],
        "delivery_ratio": metrics["deliveryRatio"],
        "latency_mean_ms": metrics["endToEndAoIMs"]["mean"],
        "latency_p50_ms": metrics["endToEndAoIMs"]["p50"],
        "latency_p95_ms": metrics["endToEndAoIMs"]["p95"],
        "latency_p99_ms": metrics["endToEndAoIMs"]["p99"],
        "longest_gap_ms": metrics["longestDeliveryGapMs"],
        "future_hit_ratio": metrics["futureHitRatio"],
        "mapping_interests": metrics["mappingInterests"],
        "payload_interests": metrics["payloadInterests"],
        "retry": metrics["retryAttempts"],
        "timeout": metrics["timeouts"],
        "nack": metrics["nacks"],
        "recoveries": metrics["recoveries"],
        "useless_interest_ratio": metrics["uselessInterestRatio"],
    }
    with (cell_dir / "cell-summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    lines = [
        f"# {summary['cellId']}",
        "",
        f"Accepted: **{summary['accepted']}**",
        f"Measured: {summary['measurement']['seconds']:.3f} s after 5 s warm-up",
        "",
        "| Metric | Value |",
        "|---|---:|",
    ]
    for key in (
        "pushed", "flushed", "delivered", "deliveryRatio",
        "longestDeliveryGapMs", "futureInterests", "futureHits",
        "futureHitRatio", "mappingInterests", "payloadInterests",
        "retryAttempts", "timeouts", "nacks", "repairAttempts",
        "recoveries", "uselessInterests", "uselessInterestRatio",
    ):
        lines.append(f"| {key} | {metrics[key]} |")
    lines.extend(("", "## Checks", ""))
    lines.extend(
        f"- {'PASS' if value else 'FAIL'}: `{key}`"
        for key, value in summary["checks"].items()
    )
    (cell_dir / "analysis.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cell-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--return-code", required=True, type=int)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    summary = analyze(args.cell_dir, manifest, args.return_code)
    write_outputs(args.cell_dir, summary)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
