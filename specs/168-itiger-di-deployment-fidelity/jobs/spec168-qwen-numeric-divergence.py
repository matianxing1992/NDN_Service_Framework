#!/usr/bin/env python3
"""Classify a pinned Qwen bf16 reference/staged token divergence.

This is a diagnostic gate, not a semantic-output bypass.  It reconstructs the
full model from the same immutable stage packages, records full-model top-k
logits for the pinned reference prefix, and independently runs the staged path.
"""

from __future__ import annotations

import argparse
import gc
import json
from pathlib import Path
from typing import Any


def _sha256(path: Path) -> str:
    import hashlib

    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _load_packages(stage_root: Path) -> list[dict[str, Any]]:
    import torch

    return [
        torch.load(
            stage_root / f"stage-{index}-qwen-transformers.pt",
            map_location="cpu",
            weights_only=True,
        )
        for index in range(3)
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--prompt-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--steps", type=int, default=5)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--tie-epsilon", type=float, default=0.125)
    args = parser.parse_args()

    if args.steps < 1 or args.top_k < 2 or args.tie_epsilon < 0:
        raise SystemExit("invalid bounded divergence diagnostic arguments")

    import torch
    from transformers.models.qwen3.configuration_qwen3 import Qwen3Config
    from transformers.models.qwen3.modeling_qwen3 import Qwen3ForCausalLM

    from llm_pipeline_lib import (
        encode_qwen_input_ids,
        qwen_transformer_model_from_stage_package,
        run_qwen_transformer_stage,
    )

    reference_doc = json.loads(args.reference.read_text(encoding="utf-8"))
    prompt = next(
        item for item in reference_doc["prompts"]
        if item["promptId"] == args.prompt_id
    )
    input_ids = [int(token) for token in prompt["formattedInputIds"]]
    reference_tokens = [int(token) for token in prompt["referenceGeneratedTokenIds"]]
    stage_root = args.artifact_dir / "qwen-transformers-stage-artifacts"
    stage_paths = [stage_root / f"stage-{index}-qwen-transformers.pt" for index in range(3)]
    for path in stage_paths:
        if not path.is_file():
            raise SystemExit(f"missing stage package: {path}")

    packages = _load_packages(stage_root)
    config = Qwen3Config.from_dict(packages[0]["config"])
    config._attn_implementation = str(
        packages[0].get("attnImplementation") or "sdpa"
    )
    full_model = Qwen3ForCausalLM(config).to(dtype=torch.bfloat16)
    merged: dict[str, Any] = {}
    for package in packages:
        merged.update(package["state_dict"])
    full_model.load_state_dict(merged, strict=True)
    full_model.to(args.device).eval()
    del merged, packages
    gc.collect()

    full_context = list(input_ids)
    full_steps: list[dict[str, Any]] = []
    full_tokens: list[int] = []
    with torch.no_grad():
        for index in range(args.steps):
            ids = torch.tensor([full_context], dtype=torch.long, device=args.device)
            logits = full_model(
                input_ids=ids,
                attention_mask=torch.ones_like(ids),
                use_cache=False,
            ).logits[0, -1].float()
            values, indices = torch.topk(logits, k=args.top_k)
            token = int(torch.argmax(logits).item())
            candidates = [
                {"tokenId": int(candidate), "logit": float(value)}
                for candidate, value in zip(indices.tolist(), values.tolist())
            ]
            full_steps.append({
                "tokenIndex": index,
                "selectedTokenId": token,
                "topCandidates": candidates,
                "topTwoMargin": float(values[0] - values[1]),
            })
            full_tokens.append(token)
            full_context.append(token)
    del full_model, logits
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    stage_models = [
        qwen_transformer_model_from_stage_package(
            path,
            device=args.device,
            require_cuda=args.device.startswith("cuda"),
        )
        for path in stage_paths
    ]
    staged_context = list(input_ids)
    staged_tokens: list[int] = []
    for index in range(args.steps):
        payload = encode_qwen_input_ids(
            [staged_context], request_id=f"spec168-numeric-{args.prompt_id}"
        )
        for stage_index, model in enumerate(stage_models):
            payload = run_qwen_transformer_stage(
                payload,
                role=f"/LLM/Pipeline/Stage/{stage_index}",
                stages=3,
                model=model,
            )
        token = int(json.loads(payload)["topToken"])
        staged_tokens.append(token)
        staged_context.append(token)

    first_divergence = next(
        (
            index for index, (expected, actual) in enumerate(
                zip(reference_tokens, staged_tokens)
            ) if expected != actual
        ),
        None,
    )
    classification = "EXACT_MATCH"
    divergence: dict[str, Any] | None = None
    if first_divergence is not None:
        step = full_steps[first_divergence]
        expected = reference_tokens[first_divergence]
        actual = staged_tokens[first_divergence]
        candidates = {
            int(item["tokenId"]): float(item["logit"])
            for item in step["topCandidates"]
        }
        best = max(candidates.values())
        both_near_top = (
            expected in candidates
            and actual in candidates
            and best - candidates[expected] <= args.tie_epsilon
            and best - candidates[actual] <= args.tie_epsilon
        )
        classification = (
            "NUMERICALLY_EQUIVALENT_DIVERGENCE"
            if both_near_top else "UNEXPLAINED_TOKEN_DIVERGENCE"
        )
        divergence = {
            "tokenIndex": first_divergence,
            "referenceTokenId": expected,
            "stagedTokenId": actual,
            "fullModelSelectedTokenId": step["selectedTokenId"],
            "topCandidates": step["topCandidates"],
            "topTwoMargin": step["topTwoMargin"],
            "tieEpsilon": args.tie_epsilon,
        }

    result = {
        "schema": "ndnsf-di.spec168-qwen-numeric-divergence.v1",
        "status": "PASS" if classification != "UNEXPLAINED_TOKEN_DIVERGENCE" else "FAIL",
        "classification": classification,
        "promptId": args.prompt_id,
        "device": str(args.device),
        "dtype": "bfloat16",
        "attentionImplementation": str(config._attn_implementation),
        "modelDigest": reference_doc["modelDigest"],
        "revision": reference_doc["revision"],
        "referenceDigest": _sha256(args.reference),
        "stageManifestDigest": _sha256(args.artifact_dir / "stage-manifest.json"),
        "stagePackageDigests": [_sha256(path) for path in stage_paths],
        "referenceTokens": reference_tokens[:args.steps],
        "fullModelTokens": full_tokens,
        "stagedTokens": staged_tokens,
        "fullModelSteps": full_steps,
        "firstDivergence": divergence,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "SPEC168_QWEN_NUMERIC_DIVERGENCE_"
        f"{result['status']} classification={classification} output={args.output}"
    )
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
