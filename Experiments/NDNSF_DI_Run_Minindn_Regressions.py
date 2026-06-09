#!/usr/bin/env python3
"""Run NDNSF-DI MiniNDN regression scripts through one entry point."""

from __future__ import annotations

import argparse
import os
import pwd
import site
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def python_path_entries() -> list[str]:
    entries = [
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(REPO / "Experiments"),
        site.getusersitepackages(),
    ]
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        try:
            sudo_home = pwd.getpwnam(sudo_user).pw_dir
            version = f"python{sys.version_info.major}.{sys.version_info.minor}"
            entries.append(str(Path(sudo_home) / ".local/lib" / version / "site-packages"))
        except KeyError:
            pass
    if os.environ.get("PYTHONPATH"):
        entries.append(os.environ["PYTHONPATH"])
    return entries


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
        success_marker="YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK",
        description="YOLO 2x2 native-provider dataflow, repo-backed artifacts, and cache reuse",
        extra_args=(
            "--layout", "2x2",
            "--parallel-detect-scale-shards",
            "--native-providers",
            "--cold-requests", "1",
            "--warm-requests", "1",
            "--ack-timeout-ms", "300",
            "--timeout-ms", "10000",
            "--quiet-perf-logs",
        ),
    ),
    "yolo-layout": RegressionCase(
        name="yolo-layout",
        script=REPO / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py",
        success_marker="YOLO_LAYOUT_NATIVE_PROVIDERS_MININDN_OK",
        description="YOLO custom-layout native-provider dataflow, repo-backed artifacts, and cache reuse",
        extra_args=(
            "--native-providers",
            "--parallel-detect-scale-shards",
            "--quiet-perf-logs",
        ),
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
    env = {
        **os.environ,
        "PYTHONPATH": ":".join(python_path_entries()),
    }
    command = ["python3", str(case.script), *case.extra_args, *(extra_args or [])]
    if case.use_sudo:
        command = ["sudo", "-E", "env", f"PYTHONPATH={env['PYTHONPATH']}", *command]
    proc = subprocess.run(
        command,
        cwd=str(REPO),
        env=env,
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
    parser.add_argument("--parallel-output-shards", action="store_true",
                        help="For --case yolo-layout-local, validate the experimental "
                             "true-NxM YOLO output-shard prototype")
    parser.add_argument("--parallel-detect-scale-shards", action="store_true",
                        help="For --case yolo-layout-local/yolo-layout, validate "
                             "the YOLO Detect-scale DAG splitter")
    parser.add_argument("--parallel-detect-replicated-backbone-shards", action="store_true",
                        help="For --case yolo-layout-local/yolo-layout, validate "
                             "the YOLO Detect-scale splitter with replicated backbone shards")
    parser.add_argument("--cold-requests", type=int, default=1,
                        help="Sequential cold requests for --case yolo-layout")
    parser.add_argument("--warm-requests", type=int, default=1,
                        help="Sequential warm requests for --case yolo-layout")
    parser.add_argument("--warm-duration-s", type=float, default=0.0,
                        help="Warm run duration for --case yolo-layout; 0 uses --warm-requests")
    parser.add_argument("--warm-interval-ms", type=int, default=0,
                        help="Minimum interval between warm request starts for --case yolo-layout")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500,
                        help="ACK timeout forwarded to --case yolo-layout")
    parser.add_argument("--timeout-ms", type=int, default=60000,
                        help="Service timeout forwarded to --case yolo-layout")
    parser.add_argument("--provider-handler-workers", type=int, default=2,
                        help="Provider Python worker count forwarded to --case yolo-layout")
    parser.add_argument("--user-async-workers", type=int, default=1,
                        help="User async worker count forwarded to --case yolo-layout")
    parser.add_argument("--list", action="store_true",
                        help="List available regression cases and exit")
    args = parser.parse_args()

    if args.list:
        for name, case in CASES.items():
            print(f"{name}: {case.description}")
        return 0

    for case in selected_cases(args.case):
        extra_args = ["--layout", args.layout] if case.name.startswith("yolo-layout") else []
        if case.name == "yolo-layout-local" and args.parallel_output_shards:
            extra_args.append("--parallel-output-shards")
        if case.name == "yolo-layout-local" and args.parallel_detect_scale_shards:
            extra_args.append("--parallel-detect-scale-shards")
        if case.name == "yolo-layout-local" and args.parallel_detect_replicated_backbone_shards:
            extra_args.append("--parallel-detect-replicated-backbone-shards")
        if case.name == "yolo-layout":
            if args.parallel_output_shards:
                extra_args.append("--parallel-output-shards")
            if args.parallel_detect_scale_shards:
                extra_args.append("--parallel-detect-scale-shards")
            if args.parallel_detect_replicated_backbone_shards:
                extra_args.append("--parallel-detect-replicated-backbone-shards")
            extra_args.extend([
                "--cold-requests", str(args.cold_requests),
                "--warm-requests", str(args.warm_requests),
                "--warm-duration-s", str(args.warm_duration_s),
                "--warm-interval-ms", str(args.warm_interval_ms),
                "--ack-timeout-ms", str(args.ack_timeout_ms),
                "--timeout-ms", str(args.timeout_ms),
                "--provider-handler-workers", str(args.provider_handler_workers),
                "--user-async-workers", str(args.user_async_workers),
            ])
        run_case(case, extra_args)
    print(f"NDNSF_DI_REGRESSION_SUITE_OK case={args.case}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
