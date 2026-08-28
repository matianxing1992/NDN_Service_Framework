#!/usr/bin/env python3
"""Run the Spec176 sequence against standard PX4/jMAVSim entry points.

The repository's GUI launcher owns MiniNDN topology, NFD setup, identities,
and PX4 instance startup.  This adapter only performs candidate preflight,
invokes that canonical launcher with the real UDP backend, and validates the
stage markers emitted by ``--auto-spec176-sitl-test``.  It intentionally does
not fall back to the mock backend or to a different PX4 tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


STAGES = (
    "SPEC176_SITL_STAGE stage=stream result=true",
    "SPEC176_SITL_STAGE stage=patrol result=true",
    "SPEC176_SITL_STAGE stage=incident-success result=true",
    "SPEC176_SITL_STAGE stage=incident-failure result=true",
    "SPEC176_SITL_STAGE stage=command-reconciliation result=true",
    "SPEC176_SITL_RESULT ok=true",
    "GS_SPEC176_SITL_EXIT ok=true",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def command_output(*args: str, cwd: Path | None = None) -> str:
    try:
        return subprocess.check_output(
            list(args), cwd=cwd, text=True, stderr=subprocess.STDOUT
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        return f"UNAVAILABLE:{' '.join(args)}:{exc}"


def git_value(repo: Path, *args: str) -> str:
    return command_output("git", *args, cwd=repo)


def preflight(repo: Path, px4: Path, output: Path) -> tuple[dict, list[str]]:
    required = {
        "groundBinary": repo / "build/examples/UavGroundStationApp",
        "droneBinary": repo / "build/examples/UavDroneApp",
        "controllerBinary": repo / "build/examples/App_ServiceController",
        "guiLauncher": repo / "Experiments/NDNSF_UAV_GUI_Minindn.py",
        "topology": repo / "Experiments/Topology/AI_Lab.conf",
        "px4JmavsimScript": px4 / "Tools/simulation/jmavsim/jmavsim_run.sh",
        "px4Binary": px4 / "build/px4_sitl_default/bin/px4",
    }
    missing = [str(path) for path in required.values() if not path.exists()]
    not_executable = [str(path) for path in required.values()
                      if path.exists() and path.suffix not in {".py", ".conf"}
                      and not os.access(path, os.X_OK)]
    report = {
        "schemaVersion": "spec176-uav-px4-sitl-preflight-v1",
        "repoRoot": str(repo),
        "px4Root": str(px4),
        "gitHead": git_value(repo, "rev-parse", "HEAD"),
        "gitTree": git_value(repo, "rev-parse", "HEAD^{tree}"),
        "px4Head": git_value(px4, "rev-parse", "HEAD") if (px4 / ".git").exists() else "UNAVAILABLE",
        "required": {name: str(path) for name, path in required.items()},
        "hashes": {
            name: sha256_file(path) for name, path in required.items()
            if path.is_file()
        },
        "toolchain": {
            "python": command_output(sys.executable, "--version"),
            "xvfbRun": command_output("xvfb-run", "--help").splitlines()[0]
            if shutil.which("xvfb-run") else "UNAVAILABLE",
        },
        "flightControllerBackend": "udp",
        "requiresRoot": True,
        "runningAsRoot": os.geteuid() == 0,
        "topology": "AI_Lab.conf: memphis controller/GS; ucla,wustl,arizona drones",
        "droneIds": ["A", "B", "C"],
        "measuredWindowSeconds": 60,
        "applicationContract": "named producer Data; no IP/host/port application fields",
    }
    report["missing"] = missing
    report["notExecutable"] = not_executable
    if not shutil.which("xvfb-run"):
        missing.append("xvfb-run")
    if not os.environ.get("DISPLAY") and not shutil.which("xvfb-run"):
        missing.append("DISPLAY-or-xvfb-run")
    output.mkdir(parents=True, exist_ok=True)
    (output / "preflight.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report, missing


def run(repo: Path, px4: Path, output: Path, timeout_seconds: int) -> int:
    report, missing = preflight(repo, px4, output)
    if missing or report["notExecutable"] or not report["runningAsRoot"]:
        print("SPEC176_PX4_SITL_BLOCKED " + json.dumps({
            "missing": missing,
            "notExecutable": report["notExecutable"],
            "runningAsRoot": report["runningAsRoot"],
            "manifest": str(output / "preflight.json"),
        }, sort_keys=True), file=sys.stderr)
        return 2

    gui_output = output / "gui"
    gui_output.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable,
        str(repo / "Experiments/NDNSF_UAV_GUI_Minindn.py"),
        "--topology-file", str(repo / "Experiments/Topology/AI_Lab.conf"),
        "--controller-node", "memphis",
        "--gs-node", "memphis",
        "--patrol-drone-nodes", "ucla,wustl,arizona",
        "--patrol-drone-ids", "A,B,C",
        "--start-jmavsim",
        "--jmavsim-headless",
        "--drone-headless",
        # Keep the SITL fixture bounded like the CPU/DI integration fixtures:
        # validate lifecycle interaction without flooding MiniNDN while PX4
        # is bootstrapping three independent processes.
        "--video-fps", "5",
        "--video-width", "320",
        "--video-fec-parity-shards", "0",
        "--live-stream-prefetch-policy", "mapped-live-v1-future-off",
        "--auto-spec176-sitl-test",
        "--spec176-timeout-seconds", str(timeout_seconds),
        "--no-cli",
        "--no-xhost",
        "--minindn-work-dir", str(gui_output / "minindn"),
        "--flight-controller-backend", "udp",
        "--px4-dir", str(px4),
        "--output-dir", str(gui_output),
    ]
    (output / "command.json").write_text(
        json.dumps({"command": command, "cwd": str(repo)}, indent=2) + "\n",
        encoding="utf-8",
    )
    log_path = output / "scenario.log"
    env = os.environ.copy()
    env["PX4_SITL_ROOT"] = str(px4)
    env["NDNSF_UAV_FLIGHT_CONTROLLER"] = "udp"
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(
            ["xvfb-run", "-a", *command], cwd=repo, env=env,
            stdout=log, stderr=subprocess.STDOUT,
            timeout=max(30, timeout_seconds + 180), check=False,
            text=True,
        )
    # The GUI launcher owns the child-process logs and does not forward the
    # Ground Station's stage markers to its own stdout.  Scan the launcher log
    # plus every persisted child log; otherwise a successful acceptance run is
    # reported as a false negative merely because the marker lives in
    # ``gui/ground-station.log``.
    log_paths = [log_path]
    log_paths.extend(sorted(gui_output.glob("*.log")))
    marker_sources: dict[str, list[str]] = {}
    for path in log_paths:
        if not path.is_file():
            continue
        child_text = path.read_text(encoding="utf-8", errors="replace")
        for marker in STAGES:
            if marker in child_text:
                marker_sources.setdefault(marker, []).append(str(path))
    missing_stages = [marker for marker in STAGES if marker not in marker_sources]
    summary = {
        "schemaVersion": "spec176-uav-px4-sitl-summary-v1",
        "preflight": str(output / "preflight.json"),
        "command": str(output / "command.json"),
        "log": str(log_path),
        "returnCode": completed.returncode,
        "missingStageMarkers": missing_stages,
        "markerSources": marker_sources,
        "scannedLogs": [str(path) for path in log_paths if path.is_file()],
        "status": "PASS" if completed.returncode == 0 and not missing_stages else "FAIL",
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print("SPEC176_PX4_SITL_RESULT " + json.dumps(summary, sort_keys=True))
    return 0 if summary["status"] == "PASS" else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--px4-root", type=Path,
                        default=(Path(os.environ["PX4_SITL_ROOT"])
                                 if os.environ.get("PX4_SITL_ROOT") else None))
    parser.add_argument("--timeout-seconds", type=int, default=60)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    if args.px4_root is None:
        parser.error("--px4-root or PX4_SITL_ROOT is required")
    px4 = args.px4_root.resolve()
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    return run(repo, px4, args.output_dir.resolve(), args.timeout_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
