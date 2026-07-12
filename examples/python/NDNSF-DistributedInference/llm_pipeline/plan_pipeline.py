#!/usr/bin/env python3
"""Generate a validation LLM pipeline NDNSF-DI policy."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

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
    manifest_path = Path(args.policy).parent / "qwen-onnx-service-manifest.json"
    if args.runtime == QWEN_ONNX_RUNTIME:
        if not manifest_path.exists():
            raise RuntimeError(f"Qwen ONNX service manifest was not generated: {manifest_path}")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if int(manifest.get("stageCount", 0)) != 3 or len(manifest.get("stages", [])) != 3:
            raise RuntimeError("Qwen pilot requires exactly three exported ONNX stages")
        manifest_digest = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    else:
        manifest_digest = ""
    print(
        "LLM_PIPELINE_POLICY_OK",
        f"service={args.service}",
        f"stages={args.stages}",
        f"layers={args.layers}",
        f"policy={policy}",
        f"manifest={manifest_path if manifest_digest else ''}",
        f"manifestSha256={manifest_digest}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
