#!/usr/bin/env python3
"""Generate a validation LLM pipeline NDNSF-DI policy."""

from __future__ import annotations

from llm_pipeline_lib import (
    QWEN_ONNX_RUNTIME,
    QWEN_TRANSFORMERS_RUNTIME,
    SERVICE,
    TINY_TRANSFORMERS_RUNTIME,
    write_policy,
)


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", default="/tmp/ndnsf-di-llm-pipeline-policy.yaml")
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--model", default="/Model/LLM/Pipeline/Fake")
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--layers", type=int, default=24)
    parser.add_argument("--controller", default="/NDNSF-DistributeInference/example/controller")
    parser.add_argument("--group", default="/NDNSF-DistributeInference/example/group")
    parser.add_argument("--user", default="/NDNSF-DistributeInference/example/user")
    parser.add_argument("--provider-prefix", default="/NDNSF-DistributeInference/example/provider")
    parser.add_argument(
        "--runtime",
        choices=("fake", TINY_TRANSFORMERS_RUNTIME, QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME),
        default="fake",
    )
    parser.add_argument("--transformer-layers", type=int, default=4)
    parser.add_argument("--qwen-model", default="Qwen/Qwen2.5-0.5B-Instruct")
    parser.add_argument("--qwen-prompt", default="")
    parser.add_argument("--qwen-allow-download", action="store_true")
    parser.add_argument("--qwen-dtype", choices=("float32", "auto"), default="float32")
    args = parser.parse_args()

    policy = write_policy(
        args.policy,
        service=args.service,
        model=args.model,
        stages=args.stages,
        layers=args.layers,
        controller=args.controller,
        group=args.group,
        user=args.user,
        provider_prefix=args.provider_prefix,
        runtime=args.runtime,
        transformer_layers=args.transformer_layers,
        qwen_model=args.qwen_model,
        qwen_prompt=args.qwen_prompt,
        qwen_allow_download=args.qwen_allow_download,
        qwen_dtype=args.qwen_dtype,
    )
    print(
        "LLM_PIPELINE_POLICY_OK",
        f"service={args.service}",
        f"stages={args.stages}",
        f"layers={args.layers}",
        f"policy={policy}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
