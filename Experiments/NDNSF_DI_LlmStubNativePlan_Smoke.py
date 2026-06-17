#!/usr/bin/env python3
"""Smoke-test LLM stub planner output against the C++ native plan parser."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def run(command: list[str], env: dict[str, str]) -> str:
    print("$ " + " ".join(command))
    proc = subprocess.run(
        command,
        cwd=str(REPO),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(proc.stdout, end="")
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)
    return proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--planner-kind", default="llm-prefill-decode",
                        choices=[
                            "llm-pipeline",
                            "llm-prefill-decode",
                            "llm-tensor-parallel",
                        ])
    parser.add_argument("--model-format", default="hf-transformers")
    parser.add_argument("--runtime-backend", default="",
                        help="Validate the LLM artifact/runtime pairing. "
                             "Examples: safetensors->vllm, "
                             "gguf->llama.cpp, "
                             "tensorrt-engine->tensorrt-llm.")
    parser.add_argument("--model", default="/Model/Llama/Stub")
    parser.add_argument("--service", default="/AI/LLM/StubInference")
    parser.add_argument("--stages", type=int, default=2)
    parser.add_argument("--shards", type=int, default=2)
    parser.add_argument("--layers", type=int, default=0)
    parser.add_argument("--out-dir", default="/tmp/ndnsf-di-llm-stub-native-plan-smoke")
    args = parser.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    policy = out / "llm_policy.yaml"
    generated = out / "generated-policy"
    native_plan = generated / "native-execution-plan.json"

    env = os.environ.copy()
    env["PYTHONPATH"] = (
        f"{REPO / 'NDNSF-DistributedInference'}:"
        f"{REPO / 'pythonWrapper'}:"
        f"{env.get('PYTHONPATH', '')}"
    )

    run([
        sys.executable,
        "examples/python/NDNSF-DistributedInference/llm_stub/plan_stub.py",
        "--planner-kind", args.planner_kind,
        "--model", args.model,
        "--model-format", args.model_format,
        "--runtime-backend", args.runtime_backend,
        "--service", args.service,
        "--stages", str(args.stages),
        "--shards", str(args.shards),
        "--layers", str(args.layers),
        "--out-dir", str(out),
        "--policy", str(policy),
    ], env)

    run([
        sys.executable,
        "-c",
        "from ndnsf_distributed_inference.policy import main; raise SystemExit(main())",
        "--config", str(policy),
        "--out-dir", str(generated),
        "--print-summary",
    ], env)

    doc = json.loads(native_plan.read_text(encoding="utf-8"))
    services = [svc for svc in doc.get("services", [])
                if svc.get("service") == args.service]
    if len(services) != 1:
        raise SystemExit(f"expected one service {args.service} in {native_plan}")
    service = services[0]
    if service.get("modelFamily") != "llm":
        raise SystemExit("LLM native plan did not carry modelFamily=llm")
    if service.get("modelFormat") != args.model_format:
        raise SystemExit("LLM native plan did not carry requested modelFormat")
    if service.get("plannerKind") != args.planner_kind:
        raise SystemExit("LLM native plan did not carry requested plannerKind")
    runtime_backend = service.get("planner", {}).get("runtimeBackend", "")
    if not runtime_backend:
        raise SystemExit("LLM native plan did not carry planner.runtimeBackend")
    if args.runtime_backend and runtime_backend != args.runtime_backend:
        raise SystemExit(
            "LLM native plan runtimeBackend mismatch: "
            f"expected {args.runtime_backend}, got {runtime_backend}")
    if args.planner_kind == "llm-pipeline":
        if service.get("executionMode") != "pipeline-parallel":
            raise SystemExit("LLM pipeline plan missing executionMode=pipeline-parallel")
        role_metadata = service.get("roleMetadata", {})
        if len(role_metadata) != args.stages:
            raise SystemExit(
                f"LLM pipeline roleMetadata mismatch: expected {args.stages}, "
                f"got {len(role_metadata)}")
        for index in range(args.stages):
            role = f"/LLM/Pipeline/Stage/{index}"
            metadata = role_metadata.get(role)
            if not metadata:
                raise SystemExit(f"LLM pipeline missing metadata for {role}")
            if metadata.get("stageIndex") != index:
                raise SystemExit(f"LLM pipeline stageIndex mismatch for {role}")
            if metadata.get("stageCount") != args.stages:
                raise SystemExit(f"LLM pipeline stageCount mismatch for {role}")
            if metadata.get("roleKind") != "llm-pipeline-stage":
                raise SystemExit(f"LLM pipeline roleKind mismatch for {role}")
        pipeline = service.get("llmPipeline", {})
        if pipeline.get("stageCount") != args.stages:
            raise SystemExit("LLM pipeline stage count metadata mismatch")
        if args.layers and pipeline.get("layerCount") != args.layers:
            raise SystemExit("LLM pipeline layer count metadata mismatch")

    run([
        "build/examples/di-native-plan-schema-smoke",
        str(native_plan),
        args.service,
        "llm",
        args.model_format,
        args.planner_kind,
    ], env)

    print(
        "LLM_STUB_NATIVE_PLAN_SMOKE_OK",
        f"planner_kind={args.planner_kind}",
        f"model_format={args.model_format}",
        f"runtime_backend={runtime_backend}",
        f"roles={len(service.get('roles', []))}",
        f"dependencies={len(service.get('dependencies', []))}",
        f"execution_mode={service.get('executionMode', '')}",
        f"native_plan={native_plan}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
