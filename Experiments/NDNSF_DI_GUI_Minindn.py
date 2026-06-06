#!/usr/bin/env python3
"""Preflight and optional MiniNDN smoke launcher for the NDNSF-DI GUI.

Default behavior is intentionally interactive-friendly: verify that the GUI and
policy tooling can be imported, then launch the GUI. Use --run-minindn when you
want the script to run a distributed-inference regression before opening the
GUI. The auto-split, yolo-2x2, and yolo-layout cases exercise MiniNDN.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
DI_ROOT = REPO / "NDNSF-DistributedInference"
GUI_SCRIPT = REPO / "Experiments/NDNSF_DI_GUI.py"
REGRESSION_RUNNER = REPO / "Experiments/NDNSF_DI_Run_Minindn_Regressions.py"
DEFAULT_POLICY = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"


def build_env() -> dict[str, str]:
    env = dict(os.environ)
    pieces = [str(DI_ROOT)]
    if env.get("PYTHONPATH"):
        pieces.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = ":".join(pieces)
    return env


def run_preflight(env: dict[str, str]) -> None:
    code = (
        "import tkinter\n"
        "from ndnsf_distributed_inference.gui import "
        "DistributedInferenceGui, DeploymentRunnerTab\n"
        "from ndnsf_distributed_inference.policy import load_config\n"
        f"load_config({str(DEFAULT_POLICY)!r})\n"
        "assert 'yolo-2x2' in DeploymentRunnerTab.REGRESSION_CASES\n"
        "print('NDNSF_DI_GUI_PREFLIGHT_OK')\n"
    )
    proc = subprocess.run(
        [sys.executable, "-c", code],
        cwd=str(REPO),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(proc.stdout, end="")
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def run_regression(case: str, env: dict[str, str]) -> None:
    command = [sys.executable, str(REGRESSION_RUNNER), "--case", case]
    if case in {"auto-split", "yolo-2x2", "yolo-layout", "all"} and os.geteuid() != 0:
        command = ["sudo", "-E", *command]
    print("$ " + " ".join(command))
    proc = subprocess.Popen(
        command,
        cwd=str(REPO),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        print(line, end="")
    proc.wait()
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def launch_gui(env: dict[str, str]) -> int:
    print("$ " + " ".join([sys.executable, str(GUI_SCRIPT)]))
    return subprocess.call([sys.executable, str(GUI_SCRIPT)], cwd=str(REPO), env=env)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify and launch the NDNSF-DI GUI, optionally after a MiniNDN smoke test.")
    parser.add_argument(
        "--case",
        choices=[
            "app-api", "onnx-executor", "auto-split", "yolo-2x2",
            "yolo-layout", "yolo-layout-local", "all",
        ],
        default="app-api",
        help="Regression case used with --run-minindn. Default: app-api; use yolo-2x2 or yolo-layout for full MiniNDN.",
    )
    parser.add_argument("--run-minindn", action="store_true",
                        help="Run the selected regression before launching the GUI.")
    parser.add_argument("--preflight-only", action="store_true",
                        help="Only verify imports/config; do not run MiniNDN or launch the GUI.")
    parser.add_argument("--no-gui", action="store_true",
                        help="Do not launch the GUI after preflight/regression.")
    args = parser.parse_args()

    env = build_env()
    run_preflight(env)
    if args.run_minindn:
        run_regression(args.case, env)
    if args.preflight_only or args.no_gui:
        return 0
    return launch_gui(env)


if __name__ == "__main__":
    raise SystemExit(main())
