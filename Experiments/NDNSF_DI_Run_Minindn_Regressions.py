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


CASES = {
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
}


def selected_cases(selection: str) -> list[RegressionCase]:
    if selection == "all":
        return [CASES["auto-split"], CASES["yolo-2x2"]]
    return [CASES[selection]]


def run_case(case: RegressionCase) -> None:
    start = time.time()
    print(f"NDNSF_DI_REGRESSION_START case={case.name} script={case.script}")
    proc = subprocess.run(
        ["sudo", "-E", "python3", str(case.script)],
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
        choices=["auto-split", "yolo-2x2", "all"],
        default="auto-split",
        help="Regression case to run. Default keeps the smoke test short.",
    )
    parser.add_argument("--list", action="store_true",
                        help="List available regression cases and exit")
    args = parser.parse_args()

    if args.list:
        for name, case in CASES.items():
            print(f"{name}: {case.description}")
        return 0

    for case in selected_cases(args.case):
        run_case(case)
    print(f"NDNSF_DI_REGRESSION_SUITE_OK case={args.case}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
