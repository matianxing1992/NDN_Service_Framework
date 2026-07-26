#!/usr/bin/env python3
"""Single-owner, immutable, no-retry Spec 144 32-cell campaign."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))
from NDNSF_UAV_Sensor_Stream_Generality_Minindn import run
from analyze_spec144_uav_sensor_stream import analyze_root

GLOBAL_LOCK = Path("/tmp/ndnsf-spec144-uav-sensor-stream.lock")


HASHED_INPUTS = (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "ndn-service-framework/TimelineTrace.hpp",
    "ndn-service-framework/TimelineTrace.cpp",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "NDNSF-UAV-APP/shared/UavSensorStreams.hpp",
    "NDNSF-UAV-APP/shared/UavSensorStreams.cpp",
    "NDNSF-UAV-APP/tools/uav_sensor_stream_node.cpp",
    "Experiments/NDNSF_UAV_Sensor_Stream_Generality_Minindn.py",
    "Experiments/run_spec144_uav_sensor_stream_matrix.py",
    "Experiments/analyze_spec144_uav_sensor_stream.py",
    "NDNSF-UAV-APP/configs/uav_demo.policies",
    "examples/trust-any.conf",
    "examples/trust-schema.conf",
    "specs/144-uav-sensor-stream-generality/spec.md",
    "specs/144-uav-sensor-stream-generality/experiment-plan.md",
    "specs/144-uav-sensor-stream-generality/contracts/workload-contract.md",
    "specs/144-uav-sensor-stream-generality/contracts/evidence-contract.md",
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


def command_identity(*command: str) -> dict:
    try:
        completed = subprocess.run(
            command, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=10, check=False)
        return {
            "command": list(command), "returnCode": completed.returncode,
            "output": completed.stdout.strip(),
        }
    except Exception as exc:
        return {
            "command": list(command), "error": f"{type(exc).__name__}: {exc}",
        }


def environment_identity() -> dict:
    boost = ""
    boost_header = Path("/usr/include/boost/version.hpp")
    if boost_header.is_file():
        boost = "\n".join(
            line.strip() for line in boost_header.read_text(
                encoding="utf-8", errors="replace").splitlines()
            if "BOOST_LIB_VERSION" in line or "BOOST_VERSION " in line)
    try:
        import minindn
        minindn_identity = {
            "version": getattr(minindn, "__version__", "unknown"),
            "path": str(Path(minindn.__file__).resolve()),
        }
    except Exception as exc:
        minindn_identity = {"error": f"{type(exc).__name__}: {exc}"}
    return {
        "platform": platform.platform(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "compiler": command_identity("c++", "--version"),
        "boostHeader": boost,
        "ndnCxx": command_identity("pkg-config", "--modversion", "libndn-cxx"),
        "nfd": command_identity("nfd", "--version"),
        "nfdc": command_identity("nfdc", "--version"),
        "python": command_identity(sys.executable, "--version"),
        "tc": command_identity("tc", "-V"),
        "miniNdn": minindn_identity,
        "gitHead": command_identity("git", "rev-parse", "HEAD"),
    }


def frozen_cells() -> list[dict]:
    values = []
    for workload in ("telemetry", "acoustic"):
        for profile, repetitions in (
                ("zero-loss", 1), ("loss", 5),
                ("reorder", 5), ("combined", 5)):
            for repetition in range(1, repetitions + 1):
                values.append({
                    "cellId": f"{workload}-{profile}-r{repetition:02d}",
                    "workload": workload,
                    "profile": profile,
                    "repetition": repetition,
                    "invocations": 0,
                    "terminal": False,
                })
    return values


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
        "schemaVersion": "spec144-uav-sensor-freeze-v1",
        "formalFrozen": True,
        "createdUnixNs": time.time_ns(),
        "outputRoot": str(output_root.resolve()),
        "inputs": inputs,
        "environment": environment_identity(),
        "cells": frozen_cells(),
        "automaticRetry": False,
        "rerunAllowed": False,
        "thresholdSource": "specs/144-uav-sensor-stream-generality/spec.md",
    }


def verify_frozen(manifest: dict) -> None:
    for name, expected in manifest["inputs"].items():
        path = REPO / name
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"formal frozen input drift: {name}")


def validate_manifest(manifest: dict) -> None:
    cells = manifest.get("cells", [])
    ids = [cell.get("cellId") for cell in cells]
    if len(cells) != 32 or len(set(ids)) != 32:
        raise ValueError("formal manifest must contain 32 unique cells")
    if manifest.get("formalFrozen") is not True:
        raise ValueError("formal manifest is not frozen")
    if manifest.get("automaticRetry") is not False:
        raise ValueError("automatic retry is prohibited")
    for cell in cells:
        if int(cell.get("invocations", 0)) not in {0, 1}:
            raise ValueError("cell invocation count exceeds one")


def acquire_campaign_lock(path: Path = GLOBAL_LOCK):
    lock = path.open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise RuntimeError("another Spec 144 campaign owner is active") from exc
    lock.seek(0)
    lock.truncate()
    lock.write(json.dumps({
        "pid": os.getpid(), "owner": "spec144-matrix",
        "acquiredUnixNs": time.time_ns(),
    }) + "\n")
    lock.flush()
    return lock


def _run_campaign_locked(output_root: Path) -> dict:
    if output_root.exists() and any(output_root.iterdir()):
        raise RuntimeError("refusing reused formal output root")
    output_root.mkdir(parents=True, exist_ok=True)
    lock_path = output_root / ".campaign.lock"
    lock = lock_path.open("w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise RuntimeError("another Spec 144 campaign owner is active") from exc
    lock.write(json.dumps({"pid": os.getpid(), "owner": "spec144-matrix"}) + "\n")
    lock.flush()

    manifest = freeze_manifest(output_root)
    validate_manifest(manifest)
    manifest_path = output_root / "campaign-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    receipt = output_root / "campaign-invocations.jsonl"
    receipt.write_text("", encoding="utf-8")
    (output_root / "cells").mkdir()
    try:
        for cell in manifest["cells"]:
            verify_frozen(manifest)
            if cell["invocations"] != 0 or cell["terminal"]:
                raise RuntimeError("formal cell mutation/retry rejected")
            cell["invocations"] = 1
            receipt.open("a", encoding="utf-8").write(json.dumps({
                "cellId": cell["cellId"], "invocation": 1,
                "startedUnixNs": time.time_ns(), "pid": os.getpid(),
            }, sort_keys=True) + "\n")
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            destination = output_root / "cells" / cell["cellId"]
            try:
                summary = run(
                    destination, cell["workload"], cell["profile"],
                    cell["repetition"], formal=True)
                cell["passed"] = bool(summary.get("passed"))
                cell["terminalState"] = (
                    "accepted" if summary.get("passed") else "failed")
                cell["error"] = summary.get("error", "")
            except Exception as exc:
                destination.mkdir(parents=True, exist_ok=True)
                failure = {
                    "schemaVersion": "spec144-uav-sensor-cell-v1",
                    "cellId": cell["cellId"], "passed": False,
                    "error": f"{type(exc).__name__}: {exc}",
                    "analysis": {
                        "workload": cell["workload"],
                        "profile": cell["profile"], "passed": False,
                        "analysisError": "cell-runner-exception",
                    },
                    "automaticRetry": False, "rerunAllowed": False,
                }
                (destination / "summary.json").write_text(
                    json.dumps(failure, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
                cell["passed"] = False
                cell["terminalState"] = "incomplete"
                cell["error"] = failure["error"]
            cell["terminal"] = True
            cell["finishedUnixNs"] = time.time_ns()
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        analysis = analyze_root(output_root)
        completed = sum(cell["terminal"] for cell in manifest["cells"])
        accepted = sum(bool(cell.get("passed")) for cell in manifest["cells"])
        result = {
            "schemaVersion": "spec144-uav-sensor-campaign-summary-v1",
            "formalFrozen": True, "declaredCells": 32,
            "terminalCells": completed, "acceptedCells": accepted,
            "singleInvocationCells": sum(
                cell["invocations"] == 1 for cell in manifest["cells"]),
            "automaticRetry": False, "rerunAllowed": False,
            "analysis": analysis,
            "complete": completed == 32,
            "passed": (
                completed == 32
                and analysis.get("sharedGeneralityVerdict") is True),
        }
        (output_root / "campaign-summary.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        return result
    finally:
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()


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
            "formalFrozen": True, "cells": frozen_cells(),
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
