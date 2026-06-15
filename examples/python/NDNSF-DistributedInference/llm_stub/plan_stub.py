#!/usr/bin/env python3
"""Generate an abstract LLM NDNSF-DI policy without executing inference."""

from __future__ import annotations

import argparse
from pathlib import Path

from ndnsf_distributed_inference import (
    PlannerKind,
    llm_planner_registry,
    llm_planner_request,
    llm_splitter_output_from_result,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a stub LLM execution plan for NDNSF-DI planner testing")
    parser.add_argument("--planner-kind", default=PlannerKind.LLM_PIPELINE.value,
                        choices=[
                            PlannerKind.LLM_PIPELINE.value,
                            PlannerKind.LLM_PREFILL_DECODE.value,
                            PlannerKind.LLM_TENSOR_PARALLEL.value,
                        ])
    parser.add_argument("--model", default="/Model/LLM/Stub")
    parser.add_argument("--model-format", default="hf-transformers",
                        help="artifact format such as hf-transformers, gguf, "
                             "safetensors, onnx, or custom")
    parser.add_argument("--service", default="/AI/LLM/StubInference")
    parser.add_argument("--stages", type=int, default=2)
    parser.add_argument("--shards", type=int, default=2)
    parser.add_argument("--out-dir", default="/tmp/ndnsf-di-llm-stub")
    parser.add_argument("--policy", default="")
    args = parser.parse_args()

    request = llm_planner_request(
        planner_kind=args.planner_kind,
        model_path=args.model,
        model_format=args.model_format,
        output_dir=args.out_dir,
        service=args.service,
        stages=args.stages,
        shards=args.shards,
    )
    result = llm_planner_registry().plan(request)
    output = llm_splitter_output_from_result(result)
    policy = Path(args.policy) if args.policy else Path(args.out_dir) / "llm_policy.yaml"
    output.write_policy_config(policy)
    print(
        "LLM_STUB_POLICY",
        f"planner_kind={result.normalized_planner_kind()}",
        f"model_format={request.normalized_model_format()}",
        f"roles={len(result.split_plan['roles'])}",
        f"dependencies={len(result.split_plan['dependencies'])}",
        f"policy={policy}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
