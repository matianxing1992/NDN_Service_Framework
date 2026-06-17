#!/usr/bin/env python3
"""Run short health checks for current NDNSF MiniNDN experiment scripts."""

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


def python_path() -> str:
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
    deduped = []
    for entry in entries:
        if entry and entry not in deduped:
            deduped.append(entry)
    return ":".join(deduped)


@dataclass(frozen=True)
class QuickCheck:
    name: str
    command: tuple[str, ...]
    marker: str
    timeout_s: int
    description: str
    use_sudo: bool = False


def checks() -> dict[str, QuickCheck]:
    return {
        "script-sanity": QuickCheck(
            name="script-sanity",
            command=(
                "bash", "-lc",
                "python3 -m py_compile "
                "Experiments/NDNSF_AI_Collaboration_Minindn.py "
                "Experiments/NDNSF_DI_PyTorch2x2_Minindn.py "
                "Experiments/NDNSF_DI_Yolo2x2_Minindn.py "
                "Experiments/NDNSF_DI_Yolo2x2_Repo_Minindn.py "
                "Experiments/NDNSF_DI_YoloSplit_Minindn.py "
                "Experiments/NDNSF_DI_RuntimeCompatibility_Smoke.py "
                "Experiments/NDNSF_DI_LlmPipeline_Smoke.py "
                "Experiments/NDNSF_DI_LlmPipeline_Minindn.py "
                "Experiments/NDNSF_DI_TransformersPipeline_LocalSmoke.py "
                "Experiments/NDNSF_DI_QwenPipeline_LocalProof.py "
                "Experiments/NDNSF_DI_LlamaServer_Smoke.py "
                "Experiments/NDNSF_DI_LlamaServer_Minindn.py "
                "Experiments/NDNSF_DistributedRepo_Generic_Minindn.py "
                "Experiments/NDNSF_Python_Hello_Minindn.py "
                "Experiments/NDNSF_Python_Minindn_Perf.py "
                "Experiments/NDNSF_TextToImage_Status_Minindn.py "
                "Experiments/NDNSF_UAV_GUI_Minindn.py "
                "Experiments/NDNSF_Run_Minindn_Quick_Checks.py && "
                "echo NDNSF_MININDN_SCRIPT_SANITY_OK",
            ),
            marker="NDNSF_MININDN_SCRIPT_SANITY_OK",
            timeout_s=60,
            description="Syntax/import sanity for updated NDNSF/Repo/DI/UAV MiniNDN scripts",
        ),
        "script-quick-smokes": QuickCheck(
            name="script-quick-smokes",
            command=(
                "bash", "-lc",
                "python3 Experiments/NDNSF_AI_Collaboration_Minindn.py --quick-smoke && "
                "python3 Experiments/NDNSF_DI_PyTorch2x2_Minindn.py --quick-smoke && "
                "python3 Experiments/NDNSF_DI_YoloSplit_Minindn.py --quick-smoke && "
                "python3 Experiments/NDNSF_DI_Yolo2x2_Repo_Minindn.py --quick-smoke && "
                "python3 Experiments/NDNSF_TextToImage_Status_Minindn.py --quick-smoke && "
                "echo NDNSF_MININDN_SCRIPT_QUICK_SMOKES_OK",
            ),
            marker="NDNSF_MININDN_SCRIPT_QUICK_SMOKES_OK",
            timeout_s=60,
            description="No-MiniNDN quick-smoke branches for longer NDNSF/Repo/DI experiment scripts",
        ),
        "ndnsf-python-hello": QuickCheck(
            name="ndnsf-python-hello",
            command=(
                "python3", "Experiments/NDNSF_Python_Hello_Minindn.py",
                "--startup-wait-s", "2",
                "--controller-wait-s", "2",
                "--ack-timeout-ms", "1000",
                "--timeout-ms", "5000",
                "--output-dir", "results/quick_checks/python_hello",
            ),
            marker="PYTHON_HELLO_MININDN_OK",
            timeout_s=180,
            description="NDNSF Python HELLO MiniNDN smoke on AI_Lab.conf",
            use_sudo=True,
        ),
        "repo-quick": QuickCheck(
            name="repo-quick",
            command=(
                "python3", "Experiments/NDNSF_DistributedRepo_Generic_Minindn.py",
                "--quick-smoke",
                "--nlsr-wait-s", "5",
                "--repo-start-wait-s", "8",
                "--output-dir", "results/quick_checks/distributed_repo",
            ),
            marker="GENERIC_DISTRIBUTED_REPO_QUICK_MININDN_OK",
            timeout_s=240,
            description="DistributedRepo single-object quick MiniNDN smoke",
            use_sudo=True,
        ),
        "di-local": QuickCheck(
            name="di-local",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "yolo-layout-local",
                "--layout", "2x2",
                "--parallel-detect-scale-shards",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=yolo-layout-local",
            timeout_s=180,
            description="DI planner/local layout smoke without MiniNDN",
        ),
        "di-runtime-compat": QuickCheck(
            name="di-runtime-compat",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "runtime-compat",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=runtime-compat",
            timeout_s=60,
            description="DI planner/policy/LLM CLI runtime compatibility contract",
        ),
        "di-llm-pipeline": QuickCheck(
            name="di-llm-pipeline",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llm-pipeline-local",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llm-pipeline-local",
            timeout_s=60,
            description="DI LLM pipeline schema v2 and fake multi-stage execution smoke",
        ),
        "di-transformers-pipeline-local": QuickCheck(
            name="di-transformers-pipeline-local",
            command=(
                "python3", "Experiments/NDNSF_DI_TransformersPipeline_LocalSmoke.py",
                "--self-test-tiny-llama",
                "--stages", "2",
            ),
            marker="NDNSF_DI_TRANSFORMERS_PIPELINE_SMOKE_OK",
            timeout_s=60,
            description="DI local Transformers layer-pipeline correctness smoke with tiny Llama",
        ),
        "di-native-readiness-unit": QuickCheck(
            name="di-native-readiness-unit",
            command=(
                "bash", "-lc",
                "./waf build --targets=unit-tests && "
                "build/unit-tests "
                "--run_test=NativeProviderReadinessAckControlsSelectionEligibility "
                "--catch_system_errors=no && "
                "echo NDNSF_DI_NATIVE_READINESS_UNIT_OK",
            ),
            marker="NDNSF_DI_NATIVE_READINESS_UNIT_OK",
            timeout_s=180,
            description=(
                "DI native provider readiness lifecycle unit regression "
                "(installing/failed ACKs stay negative; ready ACK becomes selectable)"
            ),
        ),
        "di-llama-server": QuickCheck(
            name="di-llama-server",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llama-server-local",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llama-server-local",
            timeout_s=60,
            description="DI Qwen GGUF + llama-server policy/native-plan/provider-adapter smoke",
        ),
        "uav-quick": QuickCheck(
            name="uav-quick",
            command=("python3", "Experiments/NDNSF_UAV_GUI_Minindn.py", "--quick-smoke"),
            marker="NDNSF_UAV_GUI_MININDN_QUICK_SMOKE_OK",
            timeout_s=30,
            description="UAV launcher/config quick smoke without starting GUI",
        ),
        "di-minindn-native": QuickCheck(
            name="di-minindn-native",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "yolo-2x2",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=yolo-2x2",
            timeout_s=720,
            description="DI YOLO 2x2 native-provider MiniNDN smoke; slower optional check",
        ),
        "di-llama-server-minindn": QuickCheck(
            name="di-llama-server-minindn",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llama-server-minindn",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llama-server-minindn",
            timeout_s=300,
            description="DI Qwen GGUF + llama-server repo-backed MiniNDN smoke; slower optional check",
        ),
        "di-llm-pipeline-minindn": QuickCheck(
            name="di-llm-pipeline-minindn",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llm-pipeline-minindn",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llm-pipeline-minindn",
            timeout_s=300,
            description="DI distributed LLM pipeline MiniNDN smoke with local/distributed timing; slower optional check",
        ),
        "di-llm-transformers-minindn": QuickCheck(
            name="di-llm-transformers-minindn",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llm-pipeline-transformers-minindn",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llm-pipeline-transformers-minindn",
            timeout_s=360,
            description="DI tiny Transformers block pipeline MiniNDN smoke; slower optional check",
        ),
        "di-llm-transformers-benchmark": QuickCheck(
            name="di-llm-transformers-benchmark",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llm-pipeline-transformers-benchmark",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llm-pipeline-transformers-benchmark",
            timeout_s=420,
            description="DI tiny Transformers block pipeline repeated MiniNDN benchmark; slower optional check",
        ),
        "di-llm-qwen-minindn": QuickCheck(
            name="di-llm-qwen-minindn",
            command=(
                "python3", "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
                "--case", "llm-pipeline-qwen-minindn",
            ),
            marker="NDNSF_DI_REGRESSION_SUITE_OK case=llm-pipeline-qwen-minindn",
            timeout_s=720,
            description="DI real Qwen HF stage-package pipeline MiniNDN proof; slow optional check",
        ),
    }


def command_with_environment(check: QuickCheck) -> list[str]:
    env_prefix = ["env", f"PYTHONPATH={python_path()}"]
    command = [*env_prefix, *check.command]
    if check.use_sudo:
        command = ["sudo", "-E", *command]
    return command


def run_check(check: QuickCheck) -> None:
    print(f"NDNSF_QUICK_CHECK_START case={check.name} desc={check.description}", flush=True)
    start = time.time()
    proc = subprocess.run(
        ["timeout", str(check.timeout_s), *command_with_environment(check)],
        cwd=str(REPO),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    elapsed = time.time() - start
    print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
    if proc.returncode != 0 or check.marker not in proc.stdout:
        print(
            f"NDNSF_QUICK_CHECK_FAIL case={check.name} "
            f"returncode={proc.returncode} elapsed_s={elapsed:.1f}",
            file=sys.stderr,
        )
        raise SystemExit(proc.returncode or 1)
    print(
        f"NDNSF_QUICK_CHECK_OK case={check.name} "
        f"marker={check.marker} elapsed_s={elapsed:.1f}",
        flush=True,
    )


def selected_checks(selection: str, include_di_minindn: bool) -> list[QuickCheck]:
    all_checks = checks()
    if selection != "all":
        return [all_checks[selection]]
    names = [
        "script-sanity",
        "script-quick-smokes",
        "ndnsf-python-hello",
        "repo-quick",
        "di-runtime-compat",
        "di-llm-pipeline",
        "di-transformers-pipeline-local",
        "di-native-readiness-unit",
        "di-llama-server",
        "di-local",
        "uav-quick",
    ]
    if include_di_minindn:
        names.append("di-minindn-native")
        names.append("di-llama-server-minindn")
        names.append("di-llm-pipeline-minindn")
        names.append("di-llm-transformers-minindn")
    return [all_checks[name] for name in names]


def main() -> int:
    all_checks = checks()
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--case",
        choices=[*all_checks.keys(), "all"],
        default="all",
        help="Quick check to run. Default excludes the slower DI MiniNDN native smoke.",
    )
    parser.add_argument("--include-di-minindn", action="store_true",
                        help="With --case all, also run the slower DI native-provider MiniNDN smoke.")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, check in all_checks.items():
            print(f"{name}: {check.description}")
        return 0

    for check in selected_checks(args.case, args.include_di_minindn):
        run_check(check)
    print(f"NDNSF_QUICK_CHECK_SUITE_OK case={args.case}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
