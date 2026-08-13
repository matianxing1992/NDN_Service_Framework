#!/usr/bin/env python3
"""Run the registered Spec 171 Provider-transition comparison."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Sequence


REPO = Path(__file__).resolve().parents[1]
HARNESS = REPO / "Experiments/WifiRouterMobilityReliability.py"
DEFAULT_TRACE = (
    REPO / "specs/171-four-provider-mobility-advantage/evidence/"
    "provider-transition-registration-20260809/trace.csv")
CELLS = (
    ("ndnsf", "ndnsf", ""),
    ("grpc-static-3", "grpc", "ucla,wustl,uiuc"),
    ("grpc-preregistered-4", "grpc", ""),
    ("nsc-static-3", "nsc", "ucla,wustl,uiuc"),
    ("nsc-preregistered-4", "nsc", ""),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_command(
        label: str, system: str, provider_scope: str, output_dir: Path,
        trace: Path, build_dir: Path, seed: int) -> list[str]:
    command = [
        "sudo", "-n", "env", f"NDNSF_MOBILITY_BUILD_DIR={build_dir}",
        sys.executable, str(HARNESS),
        "--profile", "four-provider-single-ap",
        "--speed-mps", "2",
        "--trace-profile", "random-waypoint",
        "--ranges", "100",
        "--systems", system,
        "--duration-s", "60",
        "--rate-rps", "5",
        "--processing-delay-ms", "5",
        "--service-workers", "4",
        "--timeout-ms", "5000",
        "--ack-timeout-ms", "1000",
        "--attempt-timeout-ms", "1000",
        "--health-interval-ms", "1000",
        "--ndnsf-strategy", "first-responding",
        "--grpc-no-health-routing",
        "--block-network",
        "--traffic-start-delay-s", "4",
        "--settle-seconds", "8",
        "--seed", str(seed),
        "--single-run",
        "--trace-replay", str(trace),
        "--campaign-id", f"spec171-provider-transition-{label}",
        "--output-dir", str(output_dir),
    ]
    if system == "ndnsf":
        command.append("--ndnsf-response-retry")
    if provider_scope:
        command.extend(("--provider-scope", provider_scope))
    return command


def build_manifest(
        output_root: Path, trace: Path, build_dir: Path, seed: int,
        replay: int) -> dict:
    replay_root = output_root / f"replay-{replay}"
    cells = []
    for label, system, scope in CELLS:
        cell_dir = replay_root / label
        cells.append({
            "label": label,
            "system": system,
            "provider_scope": scope.split(",") if scope else [
                "ucla", "wustl", "uiuc", "arizona"],
            "output_dir": str(cell_dir.resolve()),
            "command": build_command(
                label, system, scope, cell_dir, trace, build_dir, seed),
        })
    return {
        "schema": "spec171-provider-transition-campaign-v1",
        "replay": replay,
        "seed": seed,
        "trace": str(trace.resolve()),
        "trace_sha256": sha256(trace),
        "build_dir": str(build_dir.resolve()),
        "configuration": {
            "duration_s": 60,
            "rate_rps": 5,
            "measurement_trace_start_s": 4,
            "provider_d_reachable_s": 20,
            "initial_providers_retire_s": 40,
            "attempt_timeout_ms": 1000,
            "global_deadline_ms": 5000,
            "health_routing": False,
            "admission_control": False,
            "block_network": True,
        },
        "cells": cells,
    }


def run_replay(
        output_root: Path, trace: Path, build_dir: Path, seed: int,
        replay: int) -> int:
    replay_root = output_root / f"replay-{replay}"
    replay_root.mkdir(parents=True, exist_ok=True)
    manifest = build_manifest(output_root, trace, build_dir, seed, replay)
    manifest_path = replay_root / "campaign-manifest.json"
    if manifest_path.exists():
        retained = json.loads(manifest_path.read_text())
        if retained != manifest:
            raise RuntimeError(f"replay manifest changed: {manifest_path}")
    else:
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    for cell in manifest["cells"]:
        cell_dir = Path(cell["output_dir"])
        if cell_dir.exists():
            if (cell_dir / "summary.json").is_file():
                print(f"SKIP {cell['label']} retained terminal summary", flush=True)
                continue
            raise RuntimeError(f"incomplete cell already exists: {cell_dir}")
        print(f"START {cell['label']}", flush=True)
        completed = subprocess.run(
            cell["command"], cwd=REPO, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=os.environ.copy(), timeout=240)
        (replay_root / f"{cell['label']}.driver.log").write_text(completed.stdout)
        print(f"DONE {cell['label']} returncode={completed.returncode}", flush=True)
        if completed.returncode != 0:
            return completed.returncode
    (replay_root / "COMPLETE").write_text("all registered cells completed\n")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--trace", type=Path, default=DEFAULT_TRACE)
    parser.add_argument(
        "--build-dir", type=Path,
        default=REPO / "build-new-svs-20260808")
    parser.add_argument("--seed", type=int, default=171)
    parser.add_argument("--replay", type=int, required=True)
    args = parser.parse_args(argv)
    return run_replay(
        args.output_root.resolve(), args.trace.resolve(),
        args.build_dir.resolve(), args.seed, args.replay)


if __name__ == "__main__":
    raise SystemExit(main())
