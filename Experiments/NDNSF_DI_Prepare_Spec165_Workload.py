#!/usr/bin/env python3
"""Prepare the immutable Spec 165 Qwen3 workload and reference tokens."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

try:
    from .ndnsf_validation.workload import (
        DEFAULT_MODEL_SNAPSHOT,
        canonical_workload,
        write_workload,
    )
except ImportError:
    from ndnsf_validation.workload import (
        DEFAULT_MODEL_SNAPSHOT,
        canonical_workload,
        write_workload,
    )


def build_campaign(workload: dict, snapshot: Path) -> dict:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(
        snapshot, local_files_only=True, trust_remote_code=False
    )
    model = AutoModelForCausalLM.from_pretrained(
        snapshot,
        local_files_only=True,
        trust_remote_code=False,
        dtype=torch.float32,
        device_map="cpu",
    )
    model.eval()
    maximum = int(workload["maximumGeneratedTokens"])
    minimum = int(workload["minimumGeneratedTokens"])
    prompt_rows = []
    for prompt in workload["prompts"]:
        messages = [{"role": "user", "content": prompt["text"]}]
        formatted = tokenizer.apply_chat_template(
            messages,
            tokenize=False,
            add_generation_prompt=True,
            enable_thinking=False,
        )
        encoded = tokenizer(formatted, return_tensors="pt")
        with torch.inference_mode():
            output = model.generate(
                **encoded,
                do_sample=False,
                min_new_tokens=minimum,
                max_new_tokens=maximum,
                pad_token_id=tokenizer.eos_token_id,
            )
        input_ids = encoded["input_ids"][0].tolist()
        generated = output[0, len(input_ids) :].tolist()
        if len(generated) < minimum:
            raise RuntimeError(
                f"reference generation for {prompt['promptId']} produced "
                f"{len(generated)} tokens, below {minimum}"
            )
        prompt_rows.append(
            {
                "promptId": prompt["promptId"],
                "rawPrompt": prompt["text"],
                "formattedInputIds": input_ids,
                "referenceGeneratedTokenIds": generated,
                "referenceDecodedText": tokenizer.decode(
                    generated, skip_special_tokens=True
                ),
                "eosTokenIds": sorted(
                    {
                        int(tokenizer.eos_token_id),
                        *(
                            int(item)
                            for item in (
                                getattr(
                                    tokenizer,
                                    "additional_special_tokens_ids",
                                    [],
                                ) or []
                            )
                        ),
                    }
                ),
            }
        )
    return {
        "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
        "campaignId": "spec165-qwen3-minimum",
        "model": workload["modelIdentity"],
        "workloadDigest": workload["workloadDigest"],
        "generation": {
            "strategy": "greedy",
            "maxNewTokens": maximum,
            "minimumGeneratedTokens": minimum,
            "requireEos": False,
        },
        "repetitions": {
            "warmupPerPrompt": workload["warmupPerPrompt"],
            "measuredPerPrompt": workload["measuredPerPrompt"],
        },
        "prompts": prompt_rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--model-snapshot", default=str(DEFAULT_MODEL_SNAPSHOT))
    parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--minimum-generated-tokens", type=int, default=8)
    parser.add_argument("--maximum-generated-tokens", type=int, default=8)
    args = parser.parse_args()

    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    snapshot = Path(args.model_snapshot).expanduser().resolve()
    workload = canonical_workload(
        snapshot=snapshot,
        backend=args.backend,
        minimum_generated_tokens=args.minimum_generated_tokens,
        maximum_generated_tokens=args.maximum_generated_tokens,
    )
    write_workload(output / "workload.json", workload)
    campaign = build_campaign(workload, snapshot)
    (output / "generation-campaign.json").write_text(
        json.dumps(campaign, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(
        "NDNSF_SPEC165_WORKLOAD_PREPARED",
        f"workload={output / 'workload.json'}",
        f"campaign={output / 'generation-campaign.json'}",
        f"digest={workload['workloadDigest']}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
