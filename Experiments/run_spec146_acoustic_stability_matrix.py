#!/usr/bin/env python3
"""Single-owner, immutable, no-retry Spec 146 acoustic campaign."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import sys
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

from NDNSF_Acoustic_Stability_Minindn import run
from analyze_spec146_acoustic_stability import analyze_root
import run_spec144_uav_sensor_stream_matrix as support


GLOBAL_LOCK = Path("/tmp/ndnsf-spec146-acoustic-stability.lock")
PREDECESSOR_ROOT = REPO / "results/spec144-uav-sensor-stream-20260724T165132Z"
PREDECESSOR_HASHES = {
    "campaign-summary.json":
        "01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738",
    "campaign-manifest.json":
        "5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517",
    "campaign-cells.csv":
        "c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96",
}

HASHED_INPUTS = (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "ndn-service-framework/common.hpp",
    "ndn-service-framework/ServiceUser.cpp",
    "ndn-service-framework/ServiceProvider.cpp",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "NDNSF-UAV-APP/shared/UavSensorStreams.hpp",
    "NDNSF-UAV-APP/shared/UavSensorStreams.cpp",
    "NDNSF-UAV-APP/tools/uav_sensor_stream_node.cpp",
    "Experiments/NDNSF_UAV_Sensor_Stream_Generality_Minindn.py",
    "Experiments/analyze_spec144_uav_sensor_stream.py",
    "Experiments/NDNSF_Acoustic_Stability_Minindn.py",
    "Experiments/run_spec146_acoustic_stability_matrix.py",
    "Experiments/analyze_spec146_acoustic_stability.py",
    "NDNSF-UAV-APP/configs/uav_demo.policies",
    "examples/trust-any.conf",
    "examples/trust-schema.conf",
    "specs/146-acoustic-impaired-stability/spec.md",
    "specs/146-acoustic-impaired-stability/plan.md",
    "specs/146-acoustic-impaired-stability/tasks.md",
    "specs/146-acoustic-impaired-stability/contracts/formal-evidence-contract.md",
)
HASHED_BINARIES = (
    "build/libndn-service-framework.so",
    "build/examples/App_ServiceController",
    "build/examples/UavSensorStreamNode",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_predecessor() -> None:
    for name, expected in PREDECESSOR_HASHES.items():
        path = PREDECESSOR_ROOT / name
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"frozen Spec 144 predecessor drift: {name}")


def frozen_cells() -> list[dict]:
    cells = []
    for profile, repetitions in (
            ("zero-loss", 1), ("loss", 5),
            ("reorder", 5), ("combined", 5)):
        for repetition in range(1, repetitions + 1):
            cells.append({
                "cellId": f"acoustic-{profile}-r{repetition:02d}",
                "workload": "acoustic",
                "profile": profile,
                "repetition": repetition,
                "invocations": 0,
                "terminal": False,
            })
    return cells


def validate_manifest(manifest: dict) -> None:
    cells = manifest.get("cells", [])
    ids = [cell.get("cellId") for cell in cells]
    if len(cells) != 16 or len(set(ids)) != 16:
        raise ValueError("formal manifest must contain 16 unique cells")
    if manifest.get("formalFrozen") is not True:
        raise ValueError("formal manifest is not frozen")
    if manifest.get("automaticRetry") is not False:
        raise ValueError("automatic retry is prohibited")
    for cell in cells:
        if int(cell.get("invocations", 0)) not in {0, 1}:
            raise ValueError("cell invocation count exceeds one")


def freeze_manifest(output_root: Path) -> dict:
    inputs = {name: sha256(REPO / name) for name in HASHED_INPUTS}
    for name in HASHED_BINARIES:
        path = REPO / name
        if not path.is_file():
            raise RuntimeError(f"required built subject is missing: {name}")
        inputs[name] = sha256(path)
    bindings = sorted((REPO / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))
    if len(bindings) != 1:
        raise RuntimeError("expected exactly one built Python binding")
    binding_name = str(bindings[0].relative_to(REPO))
    inputs[binding_name] = sha256(bindings[0])
    return {
        "schemaVersion": "spec146-acoustic-freeze-v1",
        "formalFrozen": True,
        "createdUnixNs": time.time_ns(),
        "outputRoot": str(output_root.resolve()),
        "inputs": inputs,
        "environment": support.environment_identity(),
        "predecessor": {
            "root": str(PREDECESSOR_ROOT),
            "hashes": PREDECESSOR_HASHES,
        },
        "cells": frozen_cells(),
        "automaticRetry": False,
        "rerunAllowed": False,
        "thresholdSource":
            "specs/146-acoustic-impaired-stability/spec.md",
    }


def verify_frozen(manifest: dict) -> None:
    for name, expected in manifest["inputs"].items():
        path = REPO / name
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"formal frozen input drift: {name}")


def acquire_campaign_lock(path: Path = GLOBAL_LOCK):
    lock = path.open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise RuntimeError("another Spec 146 campaign owner is active") from exc
    lock.seek(0)
    lock.truncate()
    lock.write(json.dumps({
        "pid": os.getpid(), "owner": "spec146-matrix",
        "acquiredUnixNs": time.time_ns(),
    }) + "\n")
    lock.flush()
    return lock


def _write_json(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")


def _run_campaign_locked(output_root: Path) -> dict:
    if "spec144" in str(output_root).lower():
        raise RuntimeError("Spec 146 refuses every Spec 144 destination")
    if output_root.exists() and any(output_root.iterdir()):
        raise RuntimeError("refusing reused formal output root")
    verify_predecessor()
    output_root.mkdir(parents=True, exist_ok=True)
    (output_root / "cells").mkdir()
    manifest = freeze_manifest(output_root)
    validate_manifest(manifest)
    manifest_path = output_root / "campaign-manifest.json"
    _write_json(manifest_path, manifest)
    receipt = output_root / "campaign-invocations.jsonl"
    receipt.write_text("", encoding="utf-8")

    for cell in manifest["cells"]:
        verify_predecessor()
        verify_frozen(manifest)
        if cell["invocations"] != 0 or cell["terminal"]:
            raise RuntimeError("formal cell mutation/retry rejected")
        cell["invocations"] = 1
        with receipt.open("a", encoding="utf-8") as output:
            output.write(json.dumps({
                "cellId": cell["cellId"],
                "invocation": 1,
                "startedUnixNs": time.time_ns(),
                "pid": os.getpid(),
            }, sort_keys=True) + "\n")
        _write_json(manifest_path, manifest)
        destination = output_root / "cells" / cell["cellId"]
        try:
            summary = run(
                destination, cell["profile"], cell["repetition"], formal=True)
            cell["passed"] = bool(summary.get("passed"))
            cell["terminalState"] = (
                "accepted" if cell["passed"] else "failed")
            cell["error"] = summary.get("error", "")
        except Exception as exc:
            destination.mkdir(parents=True, exist_ok=True)
            failure = {
                "schemaVersion": "spec146-acoustic-stability-cell-v1",
                "cellId": cell["cellId"],
                "passed": False,
                "error": f"{type(exc).__name__}: {exc}",
                "analysis": {
                    "workload": "acoustic",
                    "profile": cell["profile"],
                    "passed": False,
                    "analysisError": "cell-runner-exception",
                },
                "automaticRetry": False,
                "rerunAllowed": False,
            }
            _write_json(destination / "summary.json", failure)
            cell["passed"] = False
            cell["terminalState"] = "incomplete"
            cell["error"] = failure["error"]
        cell["terminal"] = True
        cell["finishedUnixNs"] = time.time_ns()
        _write_json(manifest_path, manifest)

    verify_predecessor()
    verify_frozen(manifest)
    analysis = analyze_root(output_root)
    terminal = sum(bool(cell["terminal"]) for cell in manifest["cells"])
    accepted = sum(bool(cell.get("passed")) for cell in manifest["cells"])
    result = {
        "schemaVersion": "spec146-acoustic-campaign-summary-v1",
        "formalFrozen": True,
        "declaredCells": 16,
        "terminalCells": terminal,
        "acceptedCells": accepted,
        "singleInvocationCells": sum(
            cell["invocations"] == 1 for cell in manifest["cells"]),
        "automaticRetry": False,
        "rerunAllowed": False,
        "analysis": analysis,
        "complete": terminal == 16,
        "passed": (
            terminal == 16
            and analysis.get("acousticStabilityVerdict") is True),
    }
    summary_path = output_root / "campaign-summary.json"
    _write_json(summary_path, result)
    artifacts = (
        manifest_path,
        output_root / "campaign-analysis.json",
        output_root / "campaign-cells.csv",
        summary_path,
    )
    (output_root / "campaign-artifacts.sha256").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in artifacts),
        encoding="utf-8")
    return result


def run_campaign(output_root: Path) -> dict:
    if output_root.exists() and any(output_root.iterdir()):
        raise RuntimeError("refusing reused formal output root")
    global_lock = acquire_campaign_lock()
    try:
        return _run_campaign_locked(output_root)
    finally:
        fcntl.flock(global_lock, fcntl.LOCK_UN)
        global_lock.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    if args.check_only:
        manifest = {
            "formalFrozen": True,
            "cells": frozen_cells(),
            "automaticRetry": False,
        }
        validate_manifest(manifest)
        print(json.dumps(manifest, sort_keys=True))
        return 0
    result = run_campaign(args.output_root.resolve())
    print(json.dumps(result, sort_keys=True))
    return 0 if result["complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
