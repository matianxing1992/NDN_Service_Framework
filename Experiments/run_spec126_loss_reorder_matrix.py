#!/usr/bin/env python3
"""Run the frozen Spec 126 loss/reordering boundary matrix exactly once."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from typing import Any

from scipy.stats import beta


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "Experiments/NDNSF_UAV_GUI_Minindn.py"
ANALYZER_PATH = ROOT / "Experiments/analyze_stream_latency.py"
SPEC125_EVIDENCE = (
    ROOT / "results/spec125-adaptive-sample-atomic-20260719-acceptance",
    ROOT / "results/spec125-adaptive-sample-atomic-20260719-confirm06",
)
FROZEN_SOURCE_PATHS = tuple(Path(value) for value in (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp",
    "NDNSF-UAV-APP/shared/UavProtocol.hpp",
    "NDNSF-UAV-APP/shared/UavProtocol.cpp",
    "Experiments/NDNSF_UAV_GUI_Minindn.py",
    "Experiments/analyze_stream_latency.py",
    "Experiments/run_spec126_loss_reorder_matrix.py",
))
SPEC = importlib.util.spec_from_file_location("spec126_stream_latency", ANALYZER_PATH)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")
DECODED_RE = re.compile(r"GS_DECODED_FRAMES count=(\d+)")
DECODED_SAMPLE_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*GS_DECODED_FRAMES count=(\d+)", re.MULTILINE)
DECODER_OUTPUT_RE = re.compile(r"event=decoder-output steady_us=(\d+)")
FEC_GROUP_RE = re.compile(r"DRONE_HEADLESS_STATUS .*fec_groups=(\d+)")
CORE_RE = re.compile(r"GS_VIDEO_CORE_STATUS .*")
PROVIDER_FINAL_RE = re.compile(r"VIDEO_LIVE_STREAM_CORE_FINAL .*")
FAILURE_TOKENS = (
    "VIDEO_AUTHENTICATION_FAILED", "VIDEO_PROTECTED_PUBLICATION_FAILED",
    "FRAME_BINDING_REJECT", "invalid-authenticated-sample-extent",
    "retry-budget-exhausted", "nonce reuse", "free(): invalid pointer",
)
CAMPAIGN_ENV = {
    "NDNSF_TIMELINE_TRACE": "1",
    "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "0.05",
    "NDNSF_STREAM_PACKET_TIMELINE_TRACE": "1",
    "NDNSF_UAV_VIDEO_PIPELINE": "gstreamer",
    "NDNSF_UAV_GSTREAMER_SOURCE": "videotestsrc",
    "NDNSF_APP_NDN_LOG": (
        "ndn_service_framework.*=WARN:"
        "ndn_service_framework.examples.*=INFO:"
        "nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN:"
        "ndn_service_framework.TimelineTrace=DEBUG"),
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def frozen_cells() -> list[dict[str, Any]]:
    cells: list[dict[str, Any]] = [{
        "id": "zero-loss-run-01", "treatment": "zero-loss", "repetition": 1,
        "lossPercent": 0.0, "delayMs": 1.0, "jitterMs": 0.0,
        "reorderPercent": 0.0, "reorderCorrelationPercent": 0.0,
        "reorderGap": 0,
    }]
    profiles = (
        ("isolated-loss", 1.0, 1.0, 0.0, 0.0, 0.0, 0),
        ("reorder", 0.0, 20.0, 10.0, 25.0, 50.0, 5),
        ("combined", 1.0, 20.0, 10.0, 25.0, 50.0, 5),
    )
    for treatment, loss, delay, jitter, reorder, correlation, gap in profiles:
        for repetition in range(1, 6):
            cells.append({
                "id": f"{treatment}-run-{repetition:02d}",
                "treatment": treatment, "repetition": repetition,
                "lossPercent": loss, "delayMs": delay, "jitterMs": jitter,
                "reorderPercent": reorder,
                "reorderCorrelationPercent": correlation, "reorderGap": gap,
            })
    return cells


def hash_evidence(paths=SPEC125_EVIDENCE) -> dict[str, str]:
    manifest: dict[str, str] = {}
    for root in paths:
        if not root.is_dir():
            raise RuntimeError(f"missing frozen Spec 125 evidence: {root}")
        for path in sorted(value for value in root.rglob("*") if value.is_file()):
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            manifest[str(path.relative_to(ROOT))] = digest
    return manifest


def hash_frozen_sources(paths=FROZEN_SOURCE_PATHS) -> dict[str, str]:
    manifest: dict[str, str] = {}
    for relative in paths:
        path = ROOT / relative
        if not path.is_file():
            raise RuntimeError(f"missing frozen source: {path}")
        manifest[relative.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return manifest


def topology_text() -> str:
    # The runner replaces the automatically-created per-interface netem qdisc
    # before starting NFD, so this topology only establishes the TC hierarchy.
    return ("[nodes]\nmemphis:\nucla:\n\n[links]\n"
            "memphis:ucla delay=1ms bw=1000 loss=0\n")


def command_for(cell: dict[str, Any], output: Path, topology: Path,
                duration: int) -> list[str]:
    return [
        "sudo", "-n", "-E", "timeout", f"{duration + 150}s", "xvfb-run", "-a",
        sys.executable, str(LAUNCHER), "--topology-file", str(topology),
        "--controller-node", "memphis", "--gs-node", "memphis",
        "--drone-node", "ucla", "--drone-headless", "--camera-mode", "file",
        "--no-virtual-camera", "--flight-controller-backend", "mock",
        "--no-start-jmavsim", "--no-cli", "--no-xhost", "--nfd-log-level", "WARN",
        "--video-bitrate-kbps", "1200", "--video-width", "320",
        "--video-fec-parity-shards", "1", "--live-stream-prefetch-policy",
        "adaptive-sample-atomic", "--output-dir", str(output),
        "--auto-video-test", "--auto-stop-seconds", str(duration),
        "--auto-start-delay-ms", "1000", "--experiment-netem-enable",
        "--experiment-netem-loss-percent", f"{cell['lossPercent']:g}",
        "--experiment-netem-delay-ms", f"{cell['delayMs']:g}",
        "--experiment-netem-jitter-ms", f"{cell['jitterMs']:g}",
        "--experiment-netem-reorder-percent", f"{cell['reorderPercent']:g}",
        "--experiment-netem-reorder-correlation-percent",
        f"{cell['reorderCorrelationPercent']:g}",
        "--experiment-netem-reorder-gap", str(cell["reorderGap"]),
    ]


def fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def last_fields(pattern: re.Pattern[str], text: str) -> dict[str, str]:
    matches = pattern.findall(text)
    return fields(matches[-1]) if matches else {}


def integer(values: dict[str, str], key: str) -> int:
    try:
        return int(values.get(key, "0"))
    except ValueError:
        return 0


def percentile_value(distribution: dict[str, Any], key: str) -> float | None:
    value = distribution.get(key)
    return float(value) if value is not None else None


def traffic_metrics(core: dict[str, str], provider: dict[str, str],
                    fec_groups: int) -> dict[str, Any]:
    """Return disjoint Interest/control outcomes from the final status lines."""
    payload_interests = integer(core, "payload_interests")
    mapping_interests = integer(core, "mapping_interests")
    mapping_responses = integer(core, "mapping_data_responses")
    mapping_new = integer(core, "mapping_new_data_responses")
    necessary_items = fec_groups * 2  # one source plus one repair per accepted sample
    payload_overhead = ((payload_interests / necessary_items) - 1.0
                        if necessary_items else None)
    mapping_new_ratio = (mapping_new / mapping_responses
                         if mapping_responses else None)
    future = integer(provider, "provider_future_interests")
    hits = integer(provider, "provider_future_hits")
    return {
        "payloadInterests": payload_interests,
        "necessarySourceRepairItems": necessary_items,
        "payloadInterestOverheadRatio": payload_overhead,
        # Keep the v1 key for compatibility with the frozen formal01 reader.
        "interestOverheadRatio": payload_overhead,
        "mappingInterests": mapping_interests,
        "mappingDataResponses": mapping_responses,
        "mappingNewDataResponses": mapping_new,
        "mappingNewDataRatio": mapping_new_ratio,
        "mappingBytes": integer(core, "mapping_bytes"),
        "retryAttempts": integer(core, "retry_attempts"),
        "timeouts": integer(core, "timeouts"),
        "nacks": integer(core, "nacks"),
        "providerFutureInterests": future,
        "providerFutureHits": hits,
        "providerFutureHitRatio": hits / future if future else 0.0,
    }


def no_duplicate_application_delivery(stop_ack: dict[str, str]) -> bool:
    """Use the application duplicate counter, not Core terminal skips.

    Core ``rejected`` includes explicit live-deadline skips, which are an
    expected bounded loss outcome and are not application delivery.  The UAV
    boundary separately reports packets rejected as duplicates after its
    canonical media-sequence admission check.
    """
    return integer(stop_ack, "duplicates") == 0


CAMPAIGN_CSV_FIELDS = (
    "cellId", "treatment", "repetition", "returnCode", "decodedFrames",
    "payloadInterests", "necessarySourceRepairItems", "payloadInterestOverheadRatio",
    "mappingInterests", "mappingDataResponses", "mappingNewDataResponses",
    "mappingNewDataRatio", "mappingBytes", "retryAttempts", "timeouts", "nacks",
    "providerFutureInterests", "providerFutureHits", "providerFutureHitRatio",
    "lateArrivals", "deadlineSkips", "retryExhaustions", "maxReorderDepth",
    "accepted",
)


def write_campaign_runs_csv(destination, runs: list[dict[str, Any]]) -> None:
    writer = csv.DictWriter(destination, fieldnames=CAMPAIGN_CSV_FIELDS,
                            extrasaction="ignore")
    writer.writeheader()
    for run in runs:
        row = dict(run)
        cell = run["cell"]
        row.update({"cellId": cell["id"], "treatment": cell["treatment"],
                    "repetition": cell["repetition"]})
        writer.writerow(row)


def persist_campaign_runs_csv(path: Path, runs: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as destination:
        write_campaign_runs_csv(destination, runs)


def summarize(cell: dict[str, Any], output: Path, return_code: int,
              command: list[str], started: str, ended: str) -> dict[str, Any]:
    ground = (output / "ground-station.log").read_text(
        encoding="utf-8", errors="replace") if (output / "ground-station.log").exists() else ""
    drone = (output / "drone.log").read_text(
        encoding="utf-8", errors="replace") if (output / "drone.log").exists() else ""
    latency = analyzer.analyze_texts(
        [drone, ground], warmup_ms=5000, shared_monotonic_clock=True)
    exact = latency["exactFrameTimeline"]
    capture_decode = exact["captureToDecodeMs"]
    decoded = max((int(value) for value in DECODED_RE.findall(ground)), default=0)
    core = last_fields(CORE_RE, ground)
    provider = last_fields(PROVIDER_FINAL_RE, drone)
    fec_groups = max((int(value) for value in FEC_GROUP_RE.findall(drone)), default=0)
    traffic = traffic_metrics(core, provider, fec_groups)
    overhead = traffic["payloadInterestOverheadRatio"]
    future_ratio = traffic["providerFutureHitRatio"]
    decoded_samples = [(float(timestamp), int(count))
                       for timestamp, count in DECODED_SAMPLE_RE.findall(ground)]
    tail = ([value for value in decoded_samples
             if value[0] >= decoded_samples[-1][0] - 10.0]
            if decoded_samples else [])
    tail_gaps_ms = [(right[0] - left[0]) * 1000.0
                    for left, right in zip(tail, tail[1:])]
    final_tail_continuous = (
        len(tail) >= 9 and tail[-1][0] - tail[0][0] >= 8.0 and
        max(tail_gaps_ms, default=0.0) <= 1500.0 and
        all(right[1] > left[1] for left, right in zip(tail, tail[1:])))
    qdisc_evidence = output / "experiment-netem-before-apps.json"
    qdisc_final = output / "experiment-netem-final.json"
    failures = {token: ground.count(token) + drone.count(token)
                for token in FAILURE_TOKENS}
    failures = {key: value for key, value in failures.items() if value}
    lifecycle_ok = (return_code == 0 and not failures and
                    core.get("state") == "STOPPED" and
                    integer(core, "retry_exhaustions") == 0)
    stop_ack = last_fields(
        re.compile(r"GS_VIDEO_ADAPTIVE_STATE reason=stop-ack .*"), ground)
    no_duplicate_delivery = no_duplicate_application_delivery(stop_ack)
    impaired = cell["treatment"] != "zero-loss"
    latency_p95 = percentile_value(capture_decode, "p95")
    latency_p99 = percentile_value(capture_decode, "p99")
    checks = {
        "processAndLifecycle": lifecycle_ok,
        "effectiveQdiscCapturedBeforeApps": qdisc_evidence.is_file(),
        "effectiveQdiscCapturedAtEnd": qdisc_final.is_file(),
        "identityCoverageAtLeast99Percent": exact["identityCoverage"] >= 0.99,
        "noDuplicateApplicationDelivery": no_duplicate_delivery,
        "finalTenSecondsContinuous": final_tail_continuous,
        "decodedFrameGapZero": integer(stop_ack, "decoded_frame_gap") == 0,
        "interestOverheadWithinBound": (
            overhead is not None and overhead <= (0.25 if impaired else 0.15)),
        "futureHitRatioWithinBound": future_ratio >= (0.95 if impaired else 0.99),
        "captureToDecodeP95WithinBound": (
            latency_p95 is not None and latency_p95 <= (300.0 if impaired else 250.0)),
        "captureToDecodeP99WithinBound": (
            latency_p99 is not None and latency_p99 <= (600.0 if impaired else 500.0)),
    }
    return {
        "schemaVersion": "spec126-loss-reorder-run-v1", "cell": cell,
        "startedAt": started, "endedAt": ended, "returnCode": return_code,
        "command": command, "automaticRetry": False, "failureCounts": failures,
        "decodedFrames": decoded, "fecGroups": fec_groups,
        **traffic,
        "lateArrivals": integer(core, "late_arrivals"),
        "deadlineSkips": integer(core, "deadline_skips"),
        "retryExhaustions": integer(core, "retry_exhaustions"),
        "maxReorderDepth": max(
            (integer(fields(line), "max_reorder_depth") for line in ground.splitlines()
             if "GS_VIDEO_ADAPTIVE_STATE" in line), default=0),
        "latency": latency, "tailDecoderSamples": len(tail),
        "tailMaximumGapMs": max(tail_gaps_ms, default=None),
        "checks": checks, "accepted": all(checks.values()), "rerunAllowed": False,
    }


def exact_interval(successes: int, count: int, confidence: float = 0.95) -> list[float]:
    alpha = 1.0 - confidence
    lower = 0.0 if successes == 0 else float(beta.ppf(alpha / 2, successes,
                                                       count - successes + 1))
    upper = 1.0 if successes == count else float(beta.ppf(
        1 - alpha / 2, successes + 1, count - successes))
    return [lower, upper]


def aggregate(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output = []
    for treatment in ("zero-loss", "isolated-loss", "reorder", "combined"):
        group = [run for run in runs if run["cell"]["treatment"] == treatment]
        successes = sum(bool(run["accepted"]) for run in group)
        required = len(group) if treatment in {"zero-loss", "reorder"} else 4
        output.append({
            "treatment": treatment, "runCount": len(group), "acceptedRuns": successes,
            "requiredAcceptedRuns": required,
            "acceptanceRate": successes / len(group) if group else 0.0,
            "clopperPearson95": exact_interval(successes, len(group)) if group else None,
            "passed": successes >= required,
        })
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=int, default=60)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.duration_seconds < 60:
        raise SystemExit("Spec 126 formal cells require at least 60 seconds")
    output_root = args.output_root.resolve()
    if output_root.exists():
        raise SystemExit(f"refusing to append to existing Spec 126 matrix: {output_root}")
    output_root.mkdir(parents=True)
    cells = frozen_cells()
    topology = output_root / "topology.conf"
    topology.write_text(topology_text(), encoding="utf-8")
    before = hash_evidence()
    source_before = hash_frozen_sources()
    (output_root / "spec125-hashes-before.json").write_text(
        json.dumps(before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output_root / "source-hashes-before.json").write_text(
        json.dumps(source_before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    commands = [{"cell": cell, "environment": CAMPAIGN_ENV, "command": command_for(
        cell, output_root / cell["id"], topology, args.duration_seconds)} for cell in cells]
    manifest_text = json.dumps(commands, indent=2) + "\n"
    (output_root / "frozen-commands.json").write_text(
        manifest_text, encoding="utf-8")
    (output_root / "campaign-manifest.json").write_text(
        manifest_text, encoding="utf-8")
    if args.dry_run:
        summary = {"status": "DRY_RUN", "runCount": len(cells), "runs": commands,
                   "spec125Hashes": before, "sourceHashes": source_before}
        (output_root / "campaign-summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(summary, indent=2))
        return 0

    lock = output_root / ".campaign.lock"
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    os.write(descriptor, f"pid={os.getpid()} started={utc_now()}\n".encode())
    os.close(descriptor)
    runs: list[dict[str, Any]] = []
    env = os.environ.copy()
    env.update(CAMPAIGN_ENV)
    try:
        for entry in commands:
            cell = entry["cell"]
            command = entry["command"]
            output = output_root / cell["id"]
            output.mkdir()
            (output / "cell-definition.json").write_text(
                json.dumps(entry, indent=2) + "\n", encoding="utf-8")
            started = utc_now()
            with (output / "campaign-launcher.log").open("w", encoding="utf-8") as log:
                completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                           stderr=subprocess.STDOUT, check=False)
            run = summarize(cell, output, completed.returncode, command, started, utc_now())
            (output / "run-summary.json").write_text(
                json.dumps(run, indent=2) + "\n", encoding="utf-8")
            runs.append(run)
            persist_campaign_runs_csv(output_root / "campaign-runs.csv", runs)
    finally:
        lock.unlink(missing_ok=True)

    after = hash_evidence()
    source_after = hash_frozen_sources()
    unchanged = before == after
    source_unchanged = source_before == source_after
    (output_root / "spec125-hashes-after.json").write_text(
        json.dumps(after, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output_root / "source-hashes-after.json").write_text(
        json.dumps(source_after, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output_root / "evidence-hashes.json").write_text(
        json.dumps({
            "spec125": {"before": before, "after": after, "unchanged": unchanged},
            "source": {"before": source_before, "after": source_after,
                       "unchanged": source_unchanged},
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    treatments = aggregate(runs)
    passed = (len(runs) == len(cells) and unchanged and source_unchanged and
              all(value["passed"] for value in treatments))
    summary = {
        "schemaVersion": "spec126-loss-reorder-campaign-v1",
        "status": "PASS" if passed else "FAIL", "runCount": len(runs),
        "expectedRunCount": len(cells), "automaticRetry": False,
        "spec125EvidenceUnchanged": unchanged, "sourceUnchanged": source_unchanged,
        "treatments": treatments, "runs": runs,
    }
    (output_root / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
