#!/usr/bin/env python3
"""Validate local HuggingFace decoder-only layer pipeline execution.

This is a local correctness smoke for the future NDNSF-DI LLM pipeline runtime.
It does not use MiniNDN and does not claim networked LLM distribution.  It
checks one important assumption first: for Llama/Qwen-style decoder-only
Transformers, a plan that assigns contiguous layer ranges to pipeline stages can
produce the same next-token logits as a normal full-model forward pass.

The script is optional because ``transformers`` and a local HuggingFace model
may not be installed in every NDNSF checkout.  In that case it prints a SKIPPED
marker and exits successfully so quick suites can compile/run the entrypoint
without forcing a heavyweight model dependency.
"""

from __future__ import annotations

import argparse
import inspect
import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
sys.path.insert(0, str(REPO / "examples/python/NDNSF-DistributedInference/llm_pipeline"))

from ndnsf_distributed_inference.plan import PlannerKind  # noqa: E402
from llm_pipeline_lib import (  # noqa: E402
    decode_payload,
    encode_prompt,
    run_local_tiny_transformer_artifact_pipeline,
    run_local_tiny_transformer_pipeline,
)


SKIP_MARKER = "NDNSF_DI_TRANSFORMERS_PIPELINE_SMOKE_SKIPPED"
OK_MARKER = "NDNSF_DI_TRANSFORMERS_PIPELINE_SMOKE_OK"


def _split_ranges(layer_count: int, stages: int) -> list[tuple[int, int]]:
    if layer_count <= 0:
        raise ValueError("layer_count must be positive")
    if stages <= 0:
        raise ValueError("stages must be positive")
    if stages > layer_count:
        raise ValueError("stages cannot exceed layer_count")
    return [
        ((index * layer_count) // stages, ((index + 1) * layer_count) // stages)
        for index in range(stages)
    ]


def _call_with_supported_kwargs(fn: Any, **kwargs: Any) -> Any:
    signature = inspect.signature(fn)
    supported = {
        name: value
        for name, value in kwargs.items()
        if name in signature.parameters
    }
    return fn(**supported)


def _build_causal_mask(base_model: Any, input_ids: Any, hidden_states: Any) -> Any:
    attention_mask = None
    position_ids = _position_ids_like(input_ids)
    cache_position = position_ids[0]
    updater = getattr(base_model, "_update_causal_mask", None)
    if updater is None:
        return None
    try:
        return _call_with_supported_kwargs(
            updater,
            attention_mask=attention_mask,
            input_tensor=hidden_states,
            inputs_embeds=hidden_states,
            cache_position=cache_position,
            past_key_values=None,
            output_attentions=False,
        )
    except Exception:
        return None


def _position_ids_like(input_ids: Any) -> Any:
    import torch

    seq_len = int(input_ids.shape[1])
    return torch.arange(
        seq_len,
        device=input_ids.device,
        dtype=torch.long,
    ).unsqueeze(0)


def _rotary_embeddings(base_model: Any, hidden_states: Any, position_ids: Any) -> Any:
    rotary = getattr(base_model, "rotary_emb", None)
    if rotary is None:
        return None
    try:
        return rotary(hidden_states, position_ids)
    except TypeError:
        return None


def _run_layer(layer: Any, hidden_states: Any, *, attention_mask: Any,
               position_ids: Any, position_embeddings: Any) -> Any:
    kwargs = {
        "hidden_states": hidden_states,
        "attention_mask": attention_mask,
        "position_ids": position_ids,
        "past_key_value": None,
        "output_attentions": False,
        "use_cache": False,
        "cache_position": position_ids[0],
        "position_embeddings": position_embeddings,
    }
    output = _call_with_supported_kwargs(layer.forward, **kwargs)
    if isinstance(output, tuple):
        return output[0]
    return output


def _staged_logits(model: Any, input_ids: Any, *, stages: int) -> tuple[Any, list[tuple[int, int]]]:
    base = getattr(model, "model", None)
    if base is None:
        raise RuntimeError("model has no .model decoder backbone; only Llama/Qwen-style CausalLM is supported")
    layers = list(getattr(base, "layers", []))
    if not layers:
        raise RuntimeError("model.model.layers is empty; only decoder-layer models are supported")
    embed_tokens = getattr(base, "embed_tokens", None)
    norm = getattr(base, "norm", None)
    lm_head = getattr(model, "lm_head", None)
    if embed_tokens is None or norm is None or lm_head is None:
        raise RuntimeError("model is missing embed_tokens/norm/lm_head required for staged execution")

    position_ids = _position_ids_like(input_ids)
    hidden_states = embed_tokens(input_ids)
    attention_mask = _build_causal_mask(base, input_ids, hidden_states)
    ranges = _split_ranges(len(layers), stages)

    for start, end in ranges:
        for layer in layers[start:end]:
            position_embeddings = _rotary_embeddings(base, hidden_states, position_ids)
            hidden_states = _run_layer(
                layer,
                hidden_states,
                attention_mask=attention_mask,
                position_ids=position_ids,
                position_embeddings=position_embeddings,
            )

    hidden_states = norm(hidden_states)
    return lm_head(hidden_states), ranges


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model",
        default=os.environ.get("NDNSF_DI_TRANSFORMERS_MODEL", ""),
        help="Local HuggingFace model path/name. Defaults to NDNSF_DI_TRANSFORMERS_MODEL.",
    )
    parser.add_argument("--prompt", default="NDNSF distributed inference")
    parser.add_argument("--stages", type=int, default=2)
    parser.add_argument("--max-diff", type=float, default=1e-3)
    parser.add_argument("--allow-download", action="store_true")
    parser.add_argument("--dtype", choices=("float32", "auto"), default="float32")
    parser.add_argument(
        "--self-test-tiny-llama",
        action="store_true",
        help="Construct a tiny random LlamaForCausalLM locally and verify staged execution.",
    )
    args = parser.parse_args()

    if not args.model and not args.self_test_tiny_llama:
        print(SKIP_MARKER, "reason=no-model-path")
        return 0

    try:
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer, LlamaConfig, LlamaForCausalLM
    except Exception as exc:  # noqa: BLE001
        print(SKIP_MARKER, f"reason=missing-transformers detail={type(exc).__name__}:{exc}")
        return 0

    started_load = time.perf_counter()
    if args.self_test_tiny_llama:
        torch.manual_seed(7)
        config = LlamaConfig(
            vocab_size=257,
            hidden_size=32,
            intermediate_size=64,
            num_hidden_layers=4,
            num_attention_heads=4,
            num_key_value_heads=4,
            max_position_embeddings=64,
            rope_theta=10000.0,
            pad_token_id=0,
            bos_token_id=1,
            eos_token_id=2,
        )
        model = LlamaForCausalLM(config)
        input_ids = torch.tensor([[1, 42, 77, 13, 2]], dtype=torch.long)
        model_label = "tiny-random-llama"
    else:
        local_files_only = not args.allow_download
        dtype = torch.float32 if args.dtype == "float32" else "auto"
        tokenizer = AutoTokenizer.from_pretrained(
            args.model,
            local_files_only=local_files_only,
            trust_remote_code=True,
        )
        model = AutoModelForCausalLM.from_pretrained(
            args.model,
            local_files_only=local_files_only,
            trust_remote_code=True,
            torch_dtype=dtype,
        )
        inputs = tokenizer(args.prompt, return_tensors="pt")
        input_ids = inputs["input_ids"]
        model_label = args.model
    model.eval()
    load_ms = (time.perf_counter() - started_load) * 1000.0

    with torch.no_grad():
        started_full = time.perf_counter()
        full_logits = model(input_ids=input_ids, use_cache=False).logits
        full_ms = (time.perf_counter() - started_full) * 1000.0

        started_staged = time.perf_counter()
        staged_logits, ranges = _staged_logits(model, input_ids, stages=args.stages)
        staged_ms = (time.perf_counter() - started_staged) * 1000.0

    diff = torch.max(torch.abs(full_logits[:, -1, :] - staged_logits[:, -1, :])).item()
    full_top = int(torch.argmax(full_logits[:, -1, :], dim=-1).item())
    staged_top = int(torch.argmax(staged_logits[:, -1, :], dim=-1).item())
    if full_top != staged_top or diff > args.max_diff:
        print(
            "NDNSF_DI_TRANSFORMERS_PIPELINE_SMOKE_FAILED",
            f"plannerKind={PlannerKind.LLM_PIPELINE.value}",
            f"model={model_label}",
            f"stages={args.stages}",
            f"ranges={ranges}",
            f"full_top={full_top}",
            f"staged_top={staged_top}",
            f"max_diff={diff:.6g}",
            f"max_allowed={args.max_diff}",
        )
        return 2

    artifact_ms = -1.0
    pipeline_top = full_top
    artifact_top = full_top
    if args.self_test_tiny_llama:
        prompt_payload = encode_prompt(args.prompt, request_id="local-artifact-smoke")
        shared_local = run_local_tiny_transformer_pipeline(
            prompt_payload,
            stages=args.stages,
            layer_count=4,
            compute_delay_ms=0.0,
        )
        with tempfile.TemporaryDirectory(prefix="ndnsf-di-llm-stage-artifacts-") as tmpdir:
            artifact_local = run_local_tiny_transformer_artifact_pipeline(
                prompt_payload,
                stages=args.stages,
                layer_count=4,
                artifact_dir=tmpdir,
                compute_delay_ms=0.0,
            )
        shared_doc = decode_payload(shared_local.payload)
        artifact_doc = decode_payload(artifact_local.payload)
        pipeline_top = int(shared_doc.get("topToken", -2))
        artifact_top = int(artifact_doc.get("topToken", -1))
        artifact_ms = artifact_local.elapsed_ms
        if artifact_top != pipeline_top:
            print(
                "NDNSF_DI_TRANSFORMERS_STAGE_ARTIFACT_SMOKE_FAILED",
                f"pipeline_top={pipeline_top}",
                f"artifact_top={artifact_doc.get('topToken')}",
            )
            return 3

    print(
        OK_MARKER,
        f"plannerKind={PlannerKind.LLM_PIPELINE.value}",
        f"model={model_label}",
        f"stages={args.stages}",
        f"ranges={ranges}",
        f"load_ms={load_ms:.2f}",
        f"full_ms={full_ms:.2f}",
        f"staged_ms={staged_ms:.2f}",
        f"artifact_ms={artifact_ms:.2f}",
        f"max_diff={diff:.6g}",
        f"full_top={full_top}",
        f"staged_top={staged_top}",
        f"pipeline_top={pipeline_top}",
        f"artifact_top={artifact_top}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
