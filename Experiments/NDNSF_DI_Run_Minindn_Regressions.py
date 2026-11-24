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
    "qwen06b-native": RegressionCase(
        name="qwen06b-native",
        script=REPO / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py",
        success_marker="SPEC217_CPP_DEPLOYMENT_EXECUTION_PASS",
        description="shared installed C++ Qwen first/warm execution across MiniNDN namespaces",
        extra_args=("run", "--verify"),
    ),
    "runtime-compat": RegressionCase(
        name="runtime-compat",
        script=REPO / "Experiments/NDNSF_DI_RuntimeCompatibility_Smoke.py",
        success_marker="NDNSF_DI_RUNTIME_COMPATIBILITY_SMOKE_OK",
        description="planner registry, policy YAML, and LLM stub CLI runtime compatibility contract",
        use_sudo=False,
    ),
    "llama-server-local": RegressionCase(
        name="llama-server-local",
        script=REPO / "Experiments/NDNSF_DI_LlamaServer_Smoke.py",
        success_marker="NDNSF_DI_LLAMA_SERVER_SMOKE_OK",
        description="Qwen GGUF + llama-server policy/native-plan/provider-adapter smoke",
        use_sudo=False,
    ),
    "llm-pipeline-local": RegressionCase(
        name="llm-pipeline-local",
        script=REPO / "Experiments/NDNSF_DI_LlmPipeline_Smoke.py",
        success_marker="LLM_PIPELINE_SMOKE_OK",
        description="LLM pipeline schema v2 and fake multi-stage execution smoke",
        use_sudo=False,
    ),
    "llama-server-minindn": RegressionCase(
        name="llama-server-minindn",
        script=REPO / "Experiments/NDNSF_DI_LlamaServer_Minindn.py",
        success_marker="LLAMA_SERVER_MININDN_REPO_BACKED_OK",
        description="Qwen GGUF + llama-server repo-backed MiniNDN deployment smoke",
    ),
    "llama-server-real-minindn": RegressionCase(
        name="llama-server-real-minindn",
        script=REPO / "Experiments/NDNSF_DI_LlamaServer_Minindn.py",
        success_marker="LLAMA_SERVER_REAL_MININDN_OK",
        description="Real Qwen GGUF + locally built llama-server MiniNDN inference smoke",
        extra_args=(
            "--real-artifacts",
            "--artifact-source", "local-reference",
            "--output-dir", "results/llama_server_real_minindn_localref",
            "--model-path", "third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf",
            "--llama-runtime-dir", "third_party/llama.cpp-local/bin",
            "--prompt", "Say hello in five words.",
            "--max-tokens", "16",
            "--provider-start-timeout-s", "300",
            "--timeout-ms", "180000",
            "--llama-server-extra-arg=--ctx-size",
            "--llama-server-extra-arg=512",
            "--llama-server-extra-arg=--threads",
            "--llama-server-extra-arg=2",
            "--llama-server-extra-arg=--parallel",
            "--llama-server-extra-arg=1",
            "--llama-server-extra-arg=--no-webui",
        ),
    ),
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
        success_marker="YOLO_2X2_NATIVE_REQUESTER_MININDN_OK",
        description="YOLO 2x2 installed native requester and provider dataflow",
        extra_args=(
            "--layout", "2x2",
            "--parallel-detect-scale-shards",
            "--native-providers",
            "--native-requester",
            "--cold-requests", "1",
            "--warm-requests", "0",
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
            CASES["runtime-compat"],
            CASES["llm-pipeline-local"],
            CASES["llama-server-local"],
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
    if case.name == 'qwen06b-native':
        # Use the selected installed control environment, without development
        # package overlays or the historical Python application path.
        env = {key: value for key, value in os.environ.items()
               if key not in ('PYTHONPATH', 'PYTHONHOME')}
        command[0] = sys.executable
    if case.use_sudo:
        command = (["sudo", "-n", *command] if case.name == 'qwen06b-native' else
                   ["sudo", "-E", "env", f"PYTHONPATH={env['PYTHONPATH']}", *command])
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
        choices=sorted([*CASES.keys(), "all"]),
        default="auto-split",
        help="Regression case to run. Default keeps the smoke test short.",
    )
    parser.add_argument(
        "--layout",
        default="1x3",
        help="Layout used by --case yolo-layout-local. Examples: 1x3, 2x3, 3x2, 3x3.",
    )
    parser.add_argument("--parallel-output-shards", action="store_true",
                        help="For --case yolo-layout-local, validate the experimental "
                             "true-NxM YOLO output-shard prototype")
    parser.add_argument("--parallel-detect-scale-shards", action="store_true",
                        help="For --case yolo-layout-local, validate "
                             "the YOLO Detect-scale DAG splitter")
    parser.add_argument("--parallel-detect-replicated-backbone-shards", action="store_true",
                        help="For --case yolo-layout-local, validate "
                             "the YOLO Detect-scale splitter with replicated backbone shards")
    parser.add_argument("--auto-parallel-detect-plan", action="store_true",
                        help="For --case yolo-layout-local, generate "
                             "shared/replicated Detect candidates and run the planner-selected one")
    parser.add_argument("--ack-timeout-ms", type=int,
                        help="YOLO ACK timeout for yolo-2x2")
    parser.add_argument("--timeout-ms", type=int, default=60000,
                        help="Service timeout forwarded to yolo-2x2")
    parser.add_argument("--list", action="store_true",
                        help="List available regression cases and exit")
    parser.add_argument('--deployment', type=Path,
                        help='deployment JSON required by --case qwen06b-native')
    parser.add_argument('--oracle', type=Path,
                        help='explicit native oracle candidate for qwen06b-native qualification')
    parser.add_argument("--spec201-candidate", type=Path,
                        help="frozen Spec 201 bundle for the network YOLO cases")
    parser.add_argument("--spec201-candidate-sha256",
                        help="expected SHA-256 of the Spec 201 candidate JSON")
    parser.add_argument("--model", type=Path,
                        help="YOLO weights matching the frozen candidate")
    parser.add_argument("--native-install-prefix", type=Path,
                        help="installed SDK prefix matching the frozen network YOLO candidate")
    parser.add_argument("--native-python-package", type=Path,
                        help="installed ndnsf package matching the frozen network YOLO candidate")
    parser.add_argument("--topology-file", type=Path,
                        help="MiniNDN topology matching the frozen network YOLO candidate")
    parser.add_argument("--results-dir", type=Path, help="output directory for the network YOLO case")
    parser.add_argument("--artifact-cache-root", type=Path,
                        help="artifact cache directory for the network YOLO case")
    args = parser.parse_args()

    if args.list:
        for name, case in CASES.items():
            print(f"{name}: {case.description}")
        return 0
    if args.case == 'qwen06b-native' and args.deployment is None:
        parser.error('--case qwen06b-native requires --deployment')
    if args.case != 'qwen06b-native' and (args.deployment or args.oracle):
        parser.error('--deployment/--oracle apply only to qwen06b-native; all is the legacy suite')
    selected_parallel_modes = sum([
        bool(args.parallel_output_shards),
        bool(args.parallel_detect_scale_shards),
        bool(args.parallel_detect_replicated_backbone_shards),
        bool(args.auto_parallel_detect_plan),
    ])
    if selected_parallel_modes > 1:
        raise SystemExit(
            "--parallel-output-shards, --parallel-detect-scale-shards, "
            "--parallel-detect-replicated-backbone-shards, and "
            "--auto-parallel-detect-plan are mutually exclusive")

    cases = selected_cases(args.case)
    network_yolo = any(case.name in {"yolo-2x2"} for case in cases)
    if network_yolo:
        if not (args.spec201_candidate and args.spec201_candidate_sha256 and args.model):
            parser.error("network YOLO cases require --spec201-candidate, "
                         "--spec201-candidate-sha256 and --model")
    elif any((args.native_install_prefix, args.native_python_package, args.topology_file, args.results_dir,
              args.artifact_cache_root)):
        parser.error("candidate installation and storage options require a network YOLO case")
    for case in cases:
        extra_args = ["--layout", args.layout] if case.name == "yolo-layout-local" else []
        if case.name == 'qwen06b-native':
            extra_args.extend(['--deployment', str(args.deployment.resolve())])
            if args.oracle:
                extra_args.extend(['--oracle', str(args.oracle.resolve())])
        if case.name == "yolo-layout-local" and args.parallel_output_shards:
            extra_args.append("--parallel-output-shards")
        if case.name == "yolo-layout-local" and args.parallel_detect_scale_shards:
            extra_args.append("--parallel-detect-scale-shards")
        if case.name == "yolo-layout-local" and args.parallel_detect_replicated_backbone_shards:
            extra_args.append("--parallel-detect-replicated-backbone-shards")
        if case.name == "yolo-layout-local" and args.auto_parallel_detect_plan:
            extra_args.append("--auto-parallel-detect-plan")
        if case.name in {"yolo-2x2"}:
            ack_timeout_ms = args.ack_timeout_ms
            if ack_timeout_ms is None:
                ack_timeout_ms = 1000 if case.name == "yolo-2x2" else 1500
            extra_args.extend([
                "--spec201-candidate", str(args.spec201_candidate.resolve()),
                "--spec201-candidate-sha256", args.spec201_candidate_sha256,
                "--model", str(args.model.resolve()),
                "--ack-timeout-ms", str(ack_timeout_ms),
                "--timeout-ms", str(args.timeout_ms),
            ])
            if args.native_install_prefix is not None:
                prefix = args.native_install_prefix.resolve()
                for option, relative in (
                    ("--native-requester-binary", "bin/DI_NativeRequester"),
                    ("--native-provider-binary", "bin/di-native-provider"),
                    ("--native-assembly-worker-binary", "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker"),
                    ("--native-authority-binary", "bin/DI_NativeArtifactAuthority"),
                    ("--native-repo-binary", "bin/DistributedRepoServer"),
                    ("--native-library-path", "lib"),
                ):
                    extra_args.extend((option, str(prefix / relative)))
            for option, value in (
                ("--native-python-package", args.native_python_package),
                ("--topology-file", args.topology_file),
                ("--results-dir", args.results_dir),
                ("--artifact-cache-root", args.artifact_cache_root),
            ):
                if value is not None:
                    extra_args.extend((option, str(value.resolve())))
        run_case(case, extra_args)
    print(f"NDNSF_DI_REGRESSION_SUITE_OK case={args.case}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
