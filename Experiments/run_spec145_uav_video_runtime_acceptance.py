#!/usr/bin/env python3
"""Prepare and execute the single immutable Spec 145 MiniNDN acceptance cell."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "Experiments/NDNSF_UAV_GUI_Minindn.py"
ANALYZER_PATH = ROOT / "Experiments/analyze_stream_latency.py"
RUNNER_PATH = Path(__file__).resolve()
CELL_ID = "zero-loss-20fps-run-01"
SOURCE_PATHS = tuple(Path(value) for value in (
    "NDNSF-UAV-APP/shared/UavProtocol.hpp",
    "NDNSF-UAV-APP/shared/UavProtocol.cpp",
    "NDNSF-UAV-APP/shared/UavVideoPipeline.hpp",
    "NDNSF-UAV-APP/shared/UavVideoPipeline.cpp",
    "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp",
    "NDNSF-UAV-APP/ground-station/UavGroundStationApp.cpp",
    "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp",
    "Experiments/NDNSF_UAV_GUI_Minindn.py",
    "Experiments/analyze_stream_latency.py",
    "Experiments/run_spec145_uav_video_runtime_acceptance.py",
))
BINARY_PATHS = tuple(Path(value) for value in (
    "build/examples/App_ServiceController",
    "build/examples/UavDroneApp",
    "build/examples/UavGroundStationApp",
))
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
        "ndn_service_framework.TimelineTrace=DEBUG"
    ),
}

SPEC = importlib.util.spec_from_file_location("spec145_stream_latency", ANALYZER_PATH)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")
TIMESTAMP_RE = re.compile(r"^(\d+(?:\.\d+)?)\s+")
CORE_RE = re.compile(r"GS_VIDEO_CORE_STATUS .*")
PROVIDER_FINAL_RE = re.compile(r"VIDEO_LIVE_STREAM_CORE_FINAL .*")
STOP_ACK_RE = re.compile(r"GS_VIDEO_ADAPTIVE_STATE reason=stop-ack .*")
DRONE_STATUS_RE = re.compile(r"DRONE_HEADLESS_STATUS .*")
DECODED_RE = re.compile(r"GS_DECODED_FRAMES count=(\d+)")
DECODED_SAMPLE_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*GS_DECODED_FRAMES count=(\d+)", re.MULTILINE
)
STREAM_READY_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*GS_VIDEO_STREAM_READY", re.MULTILINE
)
FAILURE_TOKENS = (
    "VIDEO_AUTHENTICATION_FAILED",
    "VIDEO_PROTECTED_PUBLICATION_FAILED",
    "FRAME_BINDING_REJECT",
    "invalid-authenticated-sample-extent",
    "encoded access-unit class does not match its future class announcement",
    "encoded frame does not match its future class announcement",
    "UAV_VIDEO_PIPELINE backend=gstreamer state=failed",
    "terminate called",
    "Segmentation fault",
    "free(): invalid pointer",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def hashes(paths: tuple[Path, ...]) -> dict[str, str]:
    result: dict[str, str] = {}
    for relative in paths:
        path = ROOT / relative
        if not path.is_file():
            raise RuntimeError(f"missing frozen input: {path}")
        result[relative.as_posix()] = sha256(path)
    return result


def topology_text() -> str:
    return (
        "[nodes]\nmemphis:\nucla:\n\n[links]\n"
        "memphis:ucla delay=1ms bw=1000 loss=0\n"
    )


def command_for(output: Path, topology: Path) -> list[str]:
    return [
        "sudo", "-n", "-E", "timeout", "210s", "xvfb-run", "-a",
        sys.executable, str(LAUNCHER),
        "--topology-file", str(topology),
        "--controller-node", "memphis",
        "--gs-node", "memphis",
        "--drone-node", "ucla",
        "--drone-headless",
        "--camera-mode", "file",
        "--no-virtual-camera",
        "--flight-controller-backend", "mock",
        "--no-start-jmavsim",
        "--no-cli",
        "--no-xhost",
        "--nfd-log-level", "WARN",
        "--video-fps", "20",
        "--video-bitrate-kbps", "1200",
        "--video-width", "320",
        "--video-fec-parity-shards", "1",
        "--live-stream-prefetch-policy", "adaptive-sample-atomic",
        "--output-dir", str(output),
        "--auto-video-test",
        "--auto-stop-seconds", "60",
        "--auto-start-delay-ms", "1000",
        "--experiment-netem-enable",
        "--experiment-netem-loss-percent", "0",
        "--experiment-netem-delay-ms", "1",
        "--experiment-netem-jitter-ms", "0",
        "--experiment-netem-reorder-percent", "0",
        "--experiment-netem-reorder-correlation-percent", "0",
        "--experiment-netem-reorder-gap", "0",
    ]


def fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def integer(values: dict[str, str], key: str) -> int:
    try:
        return int(values.get(key, "0"))
    except ValueError:
        return 0


def last_fields(pattern: re.Pattern[str], text: str) -> dict[str, str]:
    matches = pattern.findall(text)
    return fields(matches[-1]) if matches else {}


def maximum_field(pattern: re.Pattern[str], text: str, key: str) -> int:
    return max((integer(fields(line), key) for line in pattern.findall(text)), default=0)


def active_buckets(ground: str) -> dict[str, Any]:
    ready = STREAM_READY_RE.findall(ground)
    samples = [(float(timestamp), int(count))
               for timestamp, count in DECODED_SAMPLE_RE.findall(ground)]
    if not ready:
        return {"startTimestamp": None, "active": [False] * 12, "counts": [0] * 12}
    start = float(ready[-1])
    active: list[bool] = []
    counts: list[int] = []
    previous = 0
    for bucket in range(12):
        end = start + (bucket + 1) * 5.0
        count = max((value for timestamp, value in samples
                     if start <= timestamp < end), default=previous)
        active.append(count > previous)
        counts.append(count)
        previous = count
    return {"startTimestamp": start, "active": active, "counts": counts}


def summarize(cell_dir: Path, return_code: int, manifest: dict[str, Any],
              started: str, ended: str) -> dict[str, Any]:
    ground = (cell_dir / "ground-station.log").read_text(
        encoding="utf-8", errors="replace"
    ) if (cell_dir / "ground-station.log").is_file() else ""
    drone = (cell_dir / "drone.log").read_text(
        encoding="utf-8", errors="replace"
    ) if (cell_dir / "drone.log").is_file() else ""
    combined = ground + "\n" + drone
    latency = analyzer.analyze_texts(
        [drone, ground], warmup_ms=5000, shared_monotonic_clock=True
    )
    exact = latency["exactFrameTimeline"]
    capture_decode = exact["captureToDecodeMs"]
    core = last_fields(CORE_RE, ground)
    provider = last_fields(PROVIDER_FINAL_RE, drone)
    stop_ack = last_fields(STOP_ACK_RE, ground)
    buckets = active_buckets(ground)
    failure_counts = {
        token: combined.count(token) for token in FAILURE_TOKENS
        if combined.count(token)
    }

    frame_attempts = maximum_field(DRONE_STATUS_RE, drone, "fec_groups")
    frame_publications = integer(stop_ack, "published_frames") or frame_attempts
    delivered_frames = max(
        (int(value) for value in DECODED_RE.findall(ground)), default=0
    )
    payload_interests = integer(core, "payload_interests")
    mapping_interests = integer(core, "mapping_interests")
    mapping_responses = integer(core, "mapping_data_responses")
    mapping_new = integer(core, "mapping_new_data_responses")
    future_interests = integer(provider, "provider_future_interests")
    future_hits = integer(provider, "provider_future_hits")
    necessary_items = frame_publications * 2
    payload_overhead = (
        payload_interests / necessary_items - 1.0 if necessary_items else None
    )
    future_hit_ratio = (
        future_hits / future_interests if future_interests else 0.0
    )
    core_callback_observations = [
        fields(line) for line in ground.splitlines()
        if "GS_VIDEO_ADAPTIVE_STATE" in line
        and "core_fetch_decision_available=true" in line
        and "core_fetch_decision_source=core-live-stream-status" in line
    ]
    class_mismatches = sum(
        combined.count(token) for token in FAILURE_TOKENS
        if "class" in token
    )
    pipeline_failures = combined.count(
        "UAV_VIDEO_PIPELINE backend=gstreamer state=failed"
    )

    checks = {
        "processExitedNormally": return_code == 0,
        "noFailureToken": not failure_counts,
        "zeroClassMismatch": class_mismatches == 0,
        "zeroPipelineFailure": pipeline_failures == 0,
        "activeCoreStatusFromCallback": bool(core_callback_observations),
        "allTwelveFiveSecondBucketsActive": all(buckets["active"]),
        "noDuplicateApplicationDelivery": integer(stop_ack, "duplicates") == 0,
        "providerFutureHitRatioAtLeast99Percent": future_hit_ratio >= 0.99,
        "payloadInterestOverheadAtMost25Percent": (
            payload_overhead is not None and payload_overhead <= 0.25
        ),
        "captureToDecodeP95AtMost300Ms": (
            capture_decode.get("p95") is not None
            and float(capture_decode["p95"]) <= 300.0
        ),
        "captureToDecodeP99AtMost600Ms": (
            capture_decode.get("p99") is not None
            and float(capture_decode["p99"]) <= 600.0
        ),
    }
    return {
        "schemaVersion": "spec145-uav-video-runtime-acceptance-v1",
        "cellId": CELL_ID,
        "startedAt": started,
        "endedAt": ended,
        "returnCode": return_code,
        "command": manifest["command"],
        "environment": manifest["environment"],
        "automaticRetry": False,
        "rerunAllowed": False,
        "frameAttempts": frame_attempts,
        "framePublications": frame_publications,
        "deliveredFrames": delivered_frames,
        "classMismatches": class_mismatches,
        "pipelineFailures": pipeline_failures,
        "coreDecisionAvailableObservations": len(core_callback_observations),
        "coreDecisionLastObservation": (
            core_callback_observations[-1] if core_callback_observations else None
        ),
        "fiveSecondBuckets": buckets,
        "providerFutureInterests": future_interests,
        "providerFutureHits": future_hits,
        "providerFutureHitRatio": future_hit_ratio,
        "mappingInterests": mapping_interests,
        "mappingDataResponses": mapping_responses,
        "mappingNewDataResponses": mapping_new,
        "mappingNewDataRatio": (
            mapping_new / mapping_responses if mapping_responses else None
        ),
        "payloadInterests": payload_interests,
        "necessarySourceRepairItems": necessary_items,
        "payloadInterestOverheadRatio": payload_overhead,
        "retryAttempts": integer(core, "retry_attempts"),
        "timeouts": integer(core, "timeouts"),
        "nacks": integer(core, "nacks"),
        "duplicateDeliveries": integer(stop_ack, "duplicates"),
        "endToEndCaptureToDecodeMs": {
            "samples": capture_decode.get("samples", 0),
            "mean": capture_decode.get("mean"),
            "p50": capture_decode.get("p50"),
            "p95": capture_decode.get("p95"),
            "p99": capture_decode.get("p99"),
        },
        "failureCounts": failure_counts,
        "latencyAnalysis": latency,
        "checks": checks,
        "accepted": all(checks.values()),
    }


def prepare(output_root: Path) -> None:
    if output_root.exists():
        raise SystemExit(f"refusing to overwrite existing result root: {output_root}")
    output_root.mkdir(parents=True)
    topology = output_root / "topology.conf"
    topology.write_text(topology_text(), encoding="utf-8")
    cell_dir = output_root / CELL_ID
    manifest = {
        "schemaVersion": "spec145-uav-video-runtime-command-v1",
        "preparedAt": utc_now(),
        "cellId": CELL_ID,
        "formalCellCount": 1,
        "durationSeconds": 60,
        "fps": 20,
        "topology": "memphis-ucla-two-node",
        "lossPercent": 0.0,
        "environment": CAMPAIGN_ENV,
        "command": command_for(cell_dir, topology),
        "sourceHashes": hashes(SOURCE_PATHS),
        "binaryHashes": hashes(BINARY_PATHS),
    }
    (output_root / "frozen-command.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(output_root / "frozen-command.json")


def execute(output_root: Path) -> int:
    manifest_path = output_root / "frozen-command.json"
    if not manifest_path.is_file():
        raise SystemExit("prepare-only must complete before execute")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    terminal = output_root / "campaign-summary.json"
    if terminal.exists() or (output_root / ".campaign.lock").exists():
        raise SystemExit("formal Spec 145 cell already started; rerun is forbidden")
    if hashes(SOURCE_PATHS) != manifest["sourceHashes"]:
        raise SystemExit("source drift after command freeze")
    if hashes(BINARY_PATHS) != manifest["binaryHashes"]:
        raise SystemExit("binary drift after command freeze")
    lock = output_root / ".campaign.lock"
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    os.write(descriptor, f"pid={os.getpid()} started={utc_now()}\n".encode())
    os.close(descriptor)
    cell_dir = output_root / CELL_ID
    cell_dir.mkdir()
    (cell_dir / "cell-definition.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment.update(manifest["environment"])
    started = utc_now()
    with (cell_dir / "campaign-launcher.log").open("w", encoding="utf-8") as log:
        completed = subprocess.run(
            manifest["command"], cwd=ROOT, env=environment,
            stdout=log, stderr=subprocess.STDOUT, check=False
        )
    summary = summarize(
        cell_dir, completed.returncode, manifest, started, utc_now()
    )
    (cell_dir / "run-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    campaign = {
        "schemaVersion": "spec145-uav-video-runtime-campaign-v1",
        "status": "PASS" if summary["accepted"] else "FAIL",
        "formalCellCount": 1,
        "acceptedCellCount": int(summary["accepted"]),
        "automaticRetry": False,
        "rerunAllowed": False,
        "run": summary,
        "sourceHashesAfter": hashes(SOURCE_PATHS),
        "binaryHashesAfter": hashes(BINARY_PATHS),
    }
    terminal.write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(campaign, indent=2, sort_keys=True))
    return 0 if summary["accepted"] else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--prepare-only", action="store_true")
    action.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    output_root = args.output_root.resolve()
    if args.prepare_only:
        prepare(output_root)
        return 0
    return execute(output_root)


if __name__ == "__main__":
    raise SystemExit(main())
