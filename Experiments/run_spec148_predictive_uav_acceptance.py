#!/usr/bin/env python3
"""Freeze and execute the two immutable Spec 148 UAV acceptance cells."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "Experiments/NDNSF_UAV_GUI_Minindn.py"
ANALYZER = ROOT / "Experiments/analyze_spec148_predictive_uav.py"
RUNNER = Path(__file__).resolve()
TOPOLOGY = ROOT / "Experiments/Topology/AI_Lab.conf"
CAMPAIGN_ENV = {
    "NDNSF_TIMELINE_TRACE": "1",
    "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "0.05",
    "NDNSF_STREAM_PACKET_TIMELINE_TRACE": "1",
    "NDNSF_UAV_VIDEO_PIPELINE": "gstreamer",
    "NDNSF_APP_NDN_LOG": (
        "ndn_service_framework.*=WARN:"
        "ndn_service_framework.StreamFacade=INFO:"
        "ndn_service_framework.examples.*=INFO:"
        "ndn_service_framework.TimelineTrace=DEBUG:"
        "nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN"
    ),
}
PROFILES = (
    {
        "cellId": "zero-loss",
        "lossPercent": 0.0,
        "delayMs": 0.0,
        "jitterMs": 0.0,
        "reorderPercent": 0.0,
        "reorderCorrelationPercent": 0.0,
        "reorderGap": 0,
    },
    {
        "cellId": "light-loss-reorder",
        "lossPercent": 1.0,
        "delayMs": 5.0,
        "jitterMs": 2.0,
        "reorderPercent": 1.0,
        "reorderCorrelationPercent": 25.0,
        "reorderGap": 5,
    },
)
SOURCE_PATHS = tuple(Path(value) for value in (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "ndn-service-framework/StreamFacade.hpp",
    "ndn-service-framework/StreamFacade.cpp",
    "ndn-service-framework/ServiceProvider.hpp",
    "ndn-service-framework/ServiceProvider.cpp",
    "ndn-service-framework/ServiceUser.hpp",
    "ndn-service-framework/ServiceUser.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "pythonWrapper/ndnsf/service.py",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "NDNSF-UAV-APP/shared/UavProtocol.hpp",
    "NDNSF-UAV-APP/shared/UavProtocol.cpp",
    "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp",
    "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp",
    "Experiments/NDNSF_UAV_GUI_Minindn.py",
    "Experiments/analyze_stream_latency.py",
    "Experiments/analyze_spec148_predictive_uav.py",
    "Experiments/run_spec148_predictive_uav_acceptance.py",
    "specs/148-stream-discovery-mode-design/spec.md",
    "specs/148-stream-discovery-mode-design/plan.md",
    "specs/148-stream-discovery-mode-design/tasks.md",
    "specs/148-stream-discovery-mode-design/contracts/high-level-api.md",
    "specs/148-stream-discovery-mode-design/contracts/uav-minindn-acceptance.md",
    "Experiments/Topology/AI_Lab.conf",
    "NDNSF-UAV-APP/configs/uav_runtime.conf",
    "NDNSF-UAV-APP/configs/drone-A.conf",
    "NDNSF-UAV-APP/configs/ground-station.conf",
    "NDNSF-UAV-APP/configs/uav_demo.policies",
    "NDNSF-UAV-APP/videos/drone.mp4",
))


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def hash_paths(paths: tuple[Path, ...]) -> dict[str, str]:
    result: dict[str, str] = {}
    for relative in paths:
        path = ROOT / relative
        if not path.is_file():
            raise RuntimeError(f"missing frozen input: {path}")
        result[relative.as_posix()] = sha256(path)
    return result


def binary_paths() -> tuple[Path, ...]:
    binding = sorted(
        (ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so")
    )
    if len(binding) != 1:
        raise RuntimeError(
            f"expected one rebuilt Python binding, found {len(binding)}"
        )
    return (
        Path("build/libndn-service-framework.so"),
        Path("build/examples/App_ServiceController"),
        Path("build/examples/UavDroneApp"),
        Path("build/examples/UavGroundStationApp"),
        binding[0].relative_to(ROOT),
    )


def command_for(cell_dir: Path, profile: dict[str, Any]) -> list[str]:
    return [
        "sudo", "-n", "-E", "timeout", "180s", "xvfb-run", "-a",
        sys.executable, str(LAUNCHER),
        "--topology-file", str(TOPOLOGY),
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
        "--video-fps", "30",
        "--video-bitrate-kbps", "8000",
        "--video-width", "480",
        "--video-fec-parity-shards", "1",
        "--live-stream-prefetch-policy", "mapped-pressure",
        "--output-dir", str(cell_dir),
        "--auto-video-test",
        "--auto-stop-seconds", "80",
        "--auto-start-delay-ms", "3000",
        "--experiment-netem-enable",
        "--experiment-netem-loss-percent", str(profile["lossPercent"]),
        "--experiment-netem-delay-ms", str(profile["delayMs"]),
        "--experiment-netem-jitter-ms", str(profile["jitterMs"]),
        "--experiment-netem-reorder-percent", str(profile["reorderPercent"]),
        "--experiment-netem-reorder-correlation-percent",
        str(profile["reorderCorrelationPercent"]),
        "--experiment-netem-reorder-gap", str(profile["reorderGap"]),
    ]


def environment_snapshot() -> dict[str, Any]:
    commands: dict[str, str] = {}
    for name, command in {
        "git": ["git", "rev-parse", "HEAD"],
        "compiler": ["g++", "--version"],
        "boost": ["bash", "-lc", "dpkg-query -W libboost-dev 2>/dev/null || true"],
        "minindn": ["python3", "-c", "import minindn; print(minindn.__file__)"],
    }.items():
        completed = subprocess.run(
            command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False,
        )
        commands[name] = completed.stdout.strip()
    return {
        "capturedAt": utc_now(),
        "hostname": platform.node(),
        "platform": platform.platform(),
        "python": sys.version,
        "cwd": str(ROOT),
        "commands": commands,
        "campaignEnvironment": CAMPAIGN_ENV,
    }


def prepare(output_root: Path) -> None:
    if output_root.exists():
        raise SystemExit(f"refusing to overwrite result root: {output_root}")
    output_root.mkdir(parents=True)
    sources = hash_paths(SOURCE_PATHS)
    binaries = hash_paths(binary_paths())
    environment = environment_snapshot()
    cells: list[dict[str, Any]] = []
    for profile in PROFILES:
        cell_dir = output_root / profile["cellId"]
        cell = {
            "schemaVersion": "spec148-predictive-uav-command-v1",
            "cellId": profile["cellId"],
            "profile": profile,
            "formalMeasuredSeconds": 60,
            "warmupSeconds": 5,
            "applicationRunSeconds": 80,
            "topology": str(TOPOLOGY.relative_to(ROOT)),
            "roles": {
                "memphis": ["NFD", "App_ServiceController",
                            "UavGroundStationApp"],
                "ucla": ["NFD", "UavDroneApp"],
            },
            "command": command_for(cell_dir, profile),
            "environment": CAMPAIGN_ENV,
            "sourceHashes": sources,
            "binaryHashes": binaries,
            "preparedAt": utc_now(),
            "automaticRetry": False,
            "rerunAllowed": False,
        }
        cells.append(cell)
    campaign = {
        "schemaVersion": "spec148-predictive-uav-campaign-v1",
        "preparedAt": utc_now(),
        "formalCellCount": 2,
        "executionOrder": [value["cellId"] for value in cells],
        "cells": cells,
        "sourceHashes": sources,
        "binaryHashes": binaries,
        "environment": environment,
        "automaticRetry": False,
        "rerunAllowed": False,
    }
    (output_root / "frozen-campaign.json").write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(output_root / "frozen-campaign.json")


def write_cell_inputs(cell_dir: Path, cell: dict[str, Any],
                      environment: dict[str, Any]) -> Path:
    cell_dir.mkdir()
    manifest = cell_dir / "manifest.json"
    manifest.write_text(
        json.dumps(cell, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (cell_dir / "command.txt").write_text(
        " ".join(cell["command"]) + "\n", encoding="utf-8"
    )
    (cell_dir / "environment.json").write_text(
        json.dumps(environment, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (cell_dir / "hashes.json").write_text(
        json.dumps({
            "sourceHashes": cell["sourceHashes"],
            "binaryHashes": cell["binaryHashes"],
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def execute(output_root: Path) -> int:
    campaign_path = output_root / "frozen-campaign.json"
    if not campaign_path.is_file():
        raise SystemExit("run --prepare-only before --execute")
    terminal = output_root / "campaign-summary.json"
    lock = output_root / ".campaign.lock"
    if terminal.exists() or lock.exists():
        raise SystemExit("Spec 148 formal campaign already started; rerun forbidden")
    campaign = json.loads(campaign_path.read_text(encoding="utf-8"))
    if hash_paths(SOURCE_PATHS) != campaign["sourceHashes"]:
        raise SystemExit("source drift after campaign freeze")
    if hash_paths(binary_paths()) != campaign["binaryHashes"]:
        raise SystemExit("binary drift after campaign freeze")
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    os.write(descriptor, f"pid={os.getpid()} started={utc_now()}\n".encode())
    os.close(descriptor)

    environment = os.environ.copy()
    environment.update(CAMPAIGN_ENV)
    summaries: list[dict[str, Any]] = []
    for cell in campaign["cells"]:
        if hash_paths(SOURCE_PATHS) != campaign["sourceHashes"]:
            raise SystemExit("source drift during formal campaign")
        if hash_paths(binary_paths()) != campaign["binaryHashes"]:
            raise SystemExit("binary drift during formal campaign")
        cell_dir = output_root / cell["cellId"]
        manifest = write_cell_inputs(cell_dir, cell, campaign["environment"])
        started = utc_now()
        with (cell_dir / "campaign-launcher.log").open(
            "w", encoding="utf-8"
        ) as log:
            completed = subprocess.run(
                cell["command"], cwd=ROOT, env=environment,
                stdout=log, stderr=subprocess.STDOUT, check=False,
            )
        (cell_dir / "run-boundary.json").write_text(
            json.dumps({
                "startedAt": started,
                "endedAt": utc_now(),
                "returnCode": completed.returncode,
            }, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        analyzed = subprocess.run(
            [
                sys.executable, str(ANALYZER),
                "--cell-dir", str(cell_dir),
                "--manifest", str(manifest),
                "--return-code", str(completed.returncode),
            ],
            cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False,
        )
        (cell_dir / "analyzer.log").write_text(
            analyzed.stdout, encoding="utf-8"
        )
        summary_path = cell_dir / "cell-summary.json"
        if summary_path.is_file():
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        else:
            summary = {
                "cellId": cell["cellId"],
                "accepted": False,
                "analyzerReturnCode": analyzed.returncode,
                "analyzerFailure": analyzed.stdout[-4000:],
            }
        summaries.append(summary)

    result = {
        "schemaVersion": "spec148-predictive-uav-campaign-summary-v1",
        "status": "PASS" if all(value.get("accepted") for value in summaries)
                  else "FAIL",
        "formalCellCount": len(summaries),
        "acceptedCellCount": sum(
            bool(value.get("accepted")) for value in summaries
        ),
        "automaticRetry": False,
        "rerunAllowed": False,
        "cells": summaries,
        "sourceHashesAfter": hash_paths(SOURCE_PATHS),
        "binaryHashesAfter": hash_paths(binary_paths()),
        "completedAt": utc_now(),
    }
    terminal.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "status": result["status"],
        "formalCellCount": result["formalCellCount"],
        "acceptedCellCount": result["acceptedCellCount"],
        "outputRoot": str(output_root),
    }, indent=2))
    return 0 if result["status"] == "PASS" else 1


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
