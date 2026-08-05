#!/usr/bin/env python3
"""Freeze a Qwen generation campaign using the production stage runtime.

The distributed Qwen path executes each stage with an explicit causal mask and
``use_cache=False``.  A reference produced by ``AutoModelForCausalLM.generate``
may therefore diverge in bfloat16 even when it uses the same weights.  This
tool runs the exact stage-package loader and stage runner used by Providers so
the campaign oracle is tied to the deployed numerical path.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", required=True,
                        help="existing immutable campaign used for metadata")
    parser.add_argument("--output", required=True,
                        help="new campaign path; must not already exist")
    parser.add_argument("--campaign-id", required=True)
    parser.add_argument("--stage-root", required=True)
    parser.add_argument("--tokenizer-dir", required=True)
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--require-cuda", action="store_true")
    return parser.parse_args()


def _run_prompt(prompt: dict[str, Any], *, tokenizer: Any,
                models: list[Any], stages: int) -> dict[str, Any]:
    from llm_pipeline_lib import (
        encode_qwen_pipeline_context,
        role_name,
        run_qwen_transformer_stage,
    )

    raw_prompt = str(prompt["rawPrompt"])
    formatted = tokenizer.apply_chat_template(
        [{"role": "user", "content": raw_prompt}],
        tokenize=True,
        return_dict=False,
        add_generation_prompt=True,
        enable_thinking=False,
    )
    input_ids = [int(value) for value in formatted]
    generated: list[int] = []
    payload = encode_qwen_pipeline_context(
        [input_ids],
        attention_mask=[[1] * len(input_ids)],
        request_id="spec170-oracle",
        session_id="spec170-oracle",
        context_epoch=0,
    )
    eos_ids = {
        int(tokenizer.eos_token_id),
        *(int(value) for value in
          (getattr(tokenizer, "additional_special_tokens_ids", []) or [])),
    }
    for epoch in range(64):
        stage_payload = payload
        for stage_index in range(stages):
            stage_payload = run_qwen_transformer_stage(
                stage_payload,
                role=role_name(stage_index),
                stages=stages,
                model=models[stage_index],
            )
        token = int(json.loads(stage_payload.decode("utf-8"))["topToken"])
        generated.append(token)
        if token in eos_ids:
            break
        input_ids.append(token)
        payload = encode_qwen_pipeline_context(
            [input_ids],
            attention_mask=[[1] * len(input_ids)],
            request_id="spec170-oracle",
            session_id="spec170-oracle",
            context_epoch=epoch + 1,
        )
    if len(generated) < 8:
        raise RuntimeError(
            f"{prompt['promptId']}: stage runtime generated only "
            f"{len(generated)} tokens")
    print(f"REFERENCE_READY {prompt['promptId']} tokens={len(generated)}",
          flush=True)
    return {
        "promptId": str(prompt["promptId"]),
        "rawPrompt": raw_prompt,
        "formattedInputIds": [int(value) for value in formatted],
        "referenceGeneratedTokenIds": generated,
        "referenceDecodedText": tokenizer.decode(
            generated, skip_special_tokens=True),
        "eosTokenIds": sorted(eos_ids),
    }


def main() -> int:
    args = _parse_args()
    template_path = Path(args.template)
    output_path = Path(args.output)
    if output_path.exists():
        raise SystemExit(f"refusing to overwrite existing campaign: {output_path}")
    template = json.loads(template_path.read_text(encoding="utf-8"))
    if template.get("schemaVersion") != "ndnsf-di-qwen-generation-campaign-v1":
        raise SystemExit("template is not a Qwen generation campaign")
    from transformers import AutoTokenizer
    from llm_pipeline_lib import qwen_transformer_model_from_stage_package

    tokenizer = AutoTokenizer.from_pretrained(
        args.tokenizer_dir, local_files_only=True, trust_remote_code=False)
    stage_root = Path(args.stage_root)
    stages = int(args.stages)
    if stages != 3:
        raise SystemExit("Spec170 Qwen stage oracle currently requires three stages")
    models = [
        qwen_transformer_model_from_stage_package(
            stage_root / f"stage-{index}-qwen-transformers.pt",
            device=args.device,
            require_cuda=args.require_cuda,
        )
        for index in range(stages)
    ]
    prompts = [
        _run_prompt(prompt, tokenizer=tokenizer, models=models, stages=stages)
        for prompt in template["prompts"]
    ]
    campaign = {
        "schemaVersion": template["schemaVersion"],
        "campaignId": args.campaign_id,
        "model": dict(template["model"]),
        "workloadDigest": str(template["workloadDigest"]),
        "generation": dict(template["generation"]),
        "repetitions": dict(template["repetitions"]),
        "prompts": prompts,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("x", encoding="utf-8") as stream:
        json.dump(campaign, stream, indent=2, sort_keys=True,
                  ensure_ascii=False)
        stream.write("\n")
    print(f"CAMPAIGN_READY {output_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
