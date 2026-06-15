#!/usr/bin/env python3
"""Regression smoke for DI model-format/runtime compatibility checks.

This intentionally stays below MiniNDN.  It verifies that the same compatibility
contract is enforced through planner registry dispatch, policy YAML generation,
and the LLM stub CLI.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
PYTHONPATH = f"{REPO / 'NDNSF-DistributedInference'}:{REPO / 'pythonWrapper'}"


def _env() -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONPATH"] = f"{PYTHONPATH}:{env.get('PYTHONPATH', '')}"
    return env


def run(command: list[str], *, expect_ok: bool = True) -> str:
    print("$ " + " ".join(command))
    proc = subprocess.run(
        command,
        cwd=str(REPO),
        env=_env(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(proc.stdout, end="")
    if expect_ok and proc.returncode != 0:
        raise SystemExit(proc.returncode)
    if not expect_ok and proc.returncode == 0:
        raise SystemExit("expected command to fail but it succeeded")
    return proc.stdout


def policy_command(*args: str) -> list[str]:
    return [
        sys.executable,
        "-c",
        "from ndnsf_distributed_inference.policy import main; raise SystemExit(main())",
        *args,
    ]


def test_registry_contract() -> None:
    from ndnsf_distributed_inference import (
        PlannerKind,
        llm_planner_registry,
        llm_planner_request,
        validate_runtime_compatibility,
    )

    assert validate_runtime_compatibility("llm", "safetensors", "vllm") == "vllm"
    assert validate_runtime_compatibility("llm", "gguf", "") == "llama.cpp"
    request = llm_planner_request(
        planner_kind=PlannerKind.LLM_PREFILL_DECODE,
        model_path="/Model/Llama/Stub",
        model_format="safetensors",
        runtime_backend="vllm",
        output_dir="/tmp/ndnsf-di-runtime-compat-registry",
    )
    result = llm_planner_registry().plan(request)
    assert result.metadata["runtimeBackend"] == "vllm"

    try:
        bad = llm_planner_request(
            planner_kind=PlannerKind.LLM_PREFILL_DECODE,
            model_path="/Model/Llama/Stub",
            model_format="gguf",
            runtime_backend="vllm",
            output_dir="/tmp/ndnsf-di-runtime-compat-registry-bad",
        )
        llm_planner_registry().plan(bad)
    except ValueError as exc:
        assert "incompatible" in str(exc)
    else:
        raise AssertionError("registry accepted gguf + vllm")
    print("REGISTRY_RUNTIME_COMPAT_OK")


def write_policy(path: Path, *, runtime_backend: str) -> None:
    path.write_text(f"""application: runtime-compat-smoke
controller: /NDNSF-DistributeInference/example/controller
group: /NDNSF-DistributeInference/example/group
runtime:
  user_identity: /NDNSF-DistributeInference/example/user
  provider_prefix: /NDNSF-DistributeInference/example/provider
services:
  - name: /AI/LLM/RuntimeCompat
    model: /Model/Llama/Stub
    users:
      - /NDNSF-DistributeInference/example/user
    providers:
      - identity: /NDNSF-DistributeInference/example/provider
        roles: all
    roles:
      - /LLM/Prefill
      - /LLM/Decode
    dependencies:
      - producers: [/LLM/Prefill]
        consumers: [/LLM/Decode]
        key_scope: prefill-to-decode
        topic_prefix: /activation/llm
    metadata:
      planner:
        modelFamily: llm
        modelFormat: gguf
        runtimeBackend: {runtime_backend}
        plannerKind: llm-prefill-decode
        schemaVersion: 2
""", encoding="utf-8")


def test_policy_contract(workdir: Path) -> None:
    good = workdir / "good-policy.yaml"
    bad = workdir / "bad-policy.yaml"
    write_policy(good, runtime_backend="llama.cpp")
    write_policy(bad, runtime_backend="vllm")

    run(policy_command(
        "--config", str(good),
        "--out-dir", str(workdir / "good-generated"),
    ))
    output = run(policy_command(
        "--config", str(bad),
        "--out-dir", str(workdir / "bad-generated"),
    ), expect_ok=False)
    if "incompatible" not in output or "llama.cpp, ollama" not in output:
        raise SystemExit("policy compatibility failure did not explain supported backends")
    print("POLICY_RUNTIME_COMPAT_OK")


def test_llm_stub_cli_contract(workdir: Path) -> None:
    run([
        sys.executable,
        "examples/python/NDNSF-DistributedInference/llm_stub/plan_stub.py",
        "--model-format", "safetensors",
        "--runtime-backend", "vllm",
        "--out-dir", str(workdir / "cli-good"),
    ])
    output = run([
        sys.executable,
        "examples/python/NDNSF-DistributedInference/llm_stub/plan_stub.py",
        "--model-format", "gguf",
        "--runtime-backend", "vllm",
        "--out-dir", str(workdir / "cli-bad"),
    ], expect_ok=False)
    if "incompatible" not in output or "llama.cpp, ollama" not in output:
        raise SystemExit("LLM stub CLI failure did not explain supported backends")
    print("LLM_STUB_CLI_RUNTIME_COMPAT_OK")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="ndnsf-di-runtime-compat-") as tmp:
        workdir = Path(tmp)
        test_registry_contract()
        test_policy_contract(workdir)
        test_llm_stub_cli_contract(workdir)
    print("NDNSF_DI_RUNTIME_COMPATIBILITY_SMOKE_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
