#!/usr/bin/env python3
"""Run NDNSF-DI MiniNDN regression scripts through one entry point."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class RegressionCase:
    name: str
    script: Path
    success_marker: str
    description: str
    use_sudo: bool = True
    extra_args: tuple[str, ...] = ()


CASES = {
    "app-api": RegressionCase(
        name="app-api",
        script=REPO / "Experiments/NDNSF_DI_AppApi_Smoke.py",
        success_marker="APP_API_SERVICE_PLAN_OK",
        description="APP service-level API dynamic provisioning plan smoke",
        use_sudo=False,
    ),
    "onnx-executor": RegressionCase(
        name="onnx-executor",
        script=REPO / "Experiments/NDNSF_DI_OnnxExecutor_Smoke.py",
        success_marker="ONNX_EXECUTOR_FANIN_FANOUT_OK",
        description="local ONNX executor fan-in/fan-out tensor-bundle smoke",
        use_sudo=False,
    ),
    "auto-split": RegressionCase(
        name="auto-split",
        script=REPO / "Experiments/NDNSF_DI_YoloSplit_Minindn.py",
        success_marker="YOLO_SPLIT_MININDN_OK",
        description="2-stage YOLO auto split policy and network execution",
    ),
    "yolo-2x2": RegressionCase(
        name="yolo-2x2",
        script=REPO / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py",
        success_marker="YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK",
        description="YOLO 2x2 chunk graph, repo-backed artifacts, and cache reuse",
    ),
    "yolo-layout": RegressionCase(
        name="yolo-layout",
        script=REPO / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py",
        success_marker="YOLO_LAYOUT_DYNAMIC_PROVISIONING_MININDN_OK",
        description="YOLO custom layout chunk graph, repo-backed artifacts, and cache reuse",
    ),
    "yolo-layout-local": RegressionCase(
        name="yolo-layout-local",
        script=REPO / "Experiments/NDNSF_DI_YoloLayout_Smoke.py",
        success_marker="YOLO_LAYOUT_SMOKE_OK",
        description="YOLO custom layout export, local ONNX correctness, and policy validation",
        use_sudo=False,
    ),
}


def selected_cases(selection: str) -> list[RegressionCase]:
    if selection == "all":
        return [
            CASES["app-api"],
            CASES["onnx-executor"],
            CASES["auto-split"],
            CASES["yolo-2x2"],
        ]
    return [CASES[selection]]


def run_case(case: RegressionCase, extra_args: list[str] | None = None) -> None:
    start = time.time()
    print(f"NDNSF_DI_REGRESSION_START case={case.name} script={case.script}")
    command = ["python3", str(case.script), *case.extra_args, *(extra_args or [])]
    if case.use_sudo:
        command = ["sudo", "-E", *command]
    proc = subprocess.run(
        command,
        cwd=str(REPO),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(proc.stdout, end="")
    elapsed = time.time() - start
    if proc.returncode != 0 or case.success_marker not in proc.stdout:
        print(
            "NDNSF_DI_REGRESSION_FAIL "
            f"case={case.name} returncode={proc.returncode} elapsed_s={elapsed:.1f}",
            file=sys.stderr,
        )
        raise SystemExit(proc.returncode or 1)
    print(
        "NDNSF_DI_REGRESSION_OK "
        f"case={case.name} marker={case.success_marker} elapsed_s={elapsed:.1f}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--case",
        choices=[
            "app-api", "onnx-executor", "auto-split", "yolo-2x2",
            "yolo-layout", "yolo-layout-local", "all",
        ],
        default="auto-split",
        help="Regression case to run. Default keeps the smoke test short.",
    )
    parser.add_argument(
        "--layout",
        default="1x3",
        help="Layout used by --case yolo-layout. Examples: 1x3, 2x3, 3x2, 3x3.",
    )
    parser.add_argument("--list", action="store_true",
                        help="List available regression cases and exit")
    args = parser.parse_args()

    if args.list:
        for name, case in CASES.items():
            print(f"{name}: {case.description}")
        return 0

    for case in selected_cases(args.case):
        extra_args = ["--layout", args.layout] if case.name.startswith("yolo-layout") else []
        run_case(case, extra_args)
    print(f"NDNSF_DI_REGRESSION_SUITE_OK case={args.case}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
