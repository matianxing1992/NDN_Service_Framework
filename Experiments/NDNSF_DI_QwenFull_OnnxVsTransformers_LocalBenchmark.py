#!/usr/bin/env python3
"""Local full-model Qwen benchmark: Transformers/PyTorch vs ONNX Runtime.

This benchmark does not use NDNSF, MiniNDN, model splitting, provider
processes, or hidden-state references. It runs the same full Qwen model and the
same input prompt through:

1. HuggingFace Transformers backed by PyTorch CPU.
2. A full-model ONNX export backed by ONNX Runtime CPU.
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
from pathlib import Path
from typing import Any

import numpy as np


DEFAULT_MODEL = "Qwen/Qwen2.5-0.5B-Instruct"
DEFAULT_PROMPT = "Explain NDNSF-DI pipeline inference."


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * (p / 100.0)
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    if lower == upper:
        return ordered[lower]
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {
            "count": 0,
            "avgMs": 0.0,
            "minMs": 0.0,
            "p50Ms": 0.0,
            "p95Ms": 0.0,
            "maxMs": 0.0,
        }
    return {
        "count": len(values),
        "avgMs": statistics.fmean(values),
        "minMs": min(values),
        "p50Ms": statistics.median(values),
        "p95Ms": percentile(values, 95),
        "maxMs": max(values),
    }


def make_qwen_logits_wrapper(model: Any) -> Any:
    import torch

    class QwenLogitsWrapper(torch.nn.Module):
        def __init__(self, wrapped_model: Any):
            super().__init__()
            self.model = wrapped_model

        def forward(self, input_ids, attention_mask):
            return self.model(
                input_ids=input_ids,
                attention_mask=attention_mask,
                use_cache=False,
            ).logits

    return QwenLogitsWrapper(model)


def benchmark_transformers(model: Any,
                           input_ids: Any,
                           attention_mask: Any,
                           warmup: int,
                           iterations: int) -> tuple[list[float], Any]:
    import torch

    logits = None
    with torch.no_grad():
        for _ in range(max(0, warmup)):
            logits = model(
                input_ids=input_ids,
                attention_mask=attention_mask,
                use_cache=False,
            ).logits
        samples: list[float] = []
        for _ in range(max(1, iterations)):
            started = time.perf_counter()
            logits = model(
                input_ids=input_ids,
                attention_mask=attention_mask,
                use_cache=False,
            ).logits
            samples.append((time.perf_counter() - started) * 1000.0)
    return samples, logits


def repeated_prompt(prompt: str, repeat: int) -> str:
    repeat = max(1, int(repeat))
    if repeat == 1:
        return prompt
    return "\n".join(f"{index + 1}. {prompt}" for index in range(repeat))


def export_onnx_if_needed(model: Any,
                          input_ids: Any,
                          attention_mask: Any,
                          onnx_path: Path,
                          opset: int) -> float:
    import torch

    if onnx_path.exists():
        return 0.0
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    wrapper = make_qwen_logits_wrapper(model)
    started = time.perf_counter()
    with torch.no_grad():
        torch.onnx.export(
            wrapper,
            (input_ids, attention_mask),
            str(onnx_path),
            input_names=["input_ids", "attention_mask"],
            output_names=["logits"],
            dynamic_axes={
                "input_ids": {0: "batch", 1: "sequence"},
                "attention_mask": {0: "batch", 1: "sequence"},
                "logits": {0: "batch", 1: "sequence"},
            },
            opset_version=opset,
            do_constant_folding=True,
        )
    return (time.perf_counter() - started) * 1000.0


def make_onnx_session(onnx_path: Path, intra_op_threads: int) -> tuple[Any, float]:
    import onnxruntime as ort

    opts = ort.SessionOptions()
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    if intra_op_threads > 0:
        opts.intra_op_num_threads = intra_op_threads
    started = time.perf_counter()
    session = ort.InferenceSession(
        str(onnx_path),
        sess_options=opts,
        providers=["CPUExecutionProvider"],
    )
    session_init_ms = (time.perf_counter() - started) * 1000.0
    return session, session_init_ms


def run_onnx_logits(session: Any, input_ids: Any, attention_mask: Any) -> np.ndarray:
    feed = {
        "input_ids": input_ids.detach().cpu().numpy().astype(np.int64),
        "attention_mask": attention_mask.detach().cpu().numpy().astype(np.int64),
    }
    return session.run(["logits"], feed)[0]


def benchmark_onnx_session(session: Any,
                           input_ids: Any,
                           attention_mask: Any,
                           warmup: int,
                           iterations: int) -> tuple[list[float], np.ndarray]:
    logits = None
    for _ in range(max(0, warmup)):
        logits = run_onnx_logits(session, input_ids, attention_mask)
    samples: list[float] = []
    for _ in range(max(1, iterations)):
        started = time.perf_counter()
        logits = run_onnx_logits(session, input_ids, attention_mask)
        samples.append((time.perf_counter() - started) * 1000.0)
    return samples, logits


def transformers_cached_decode_once(model: Any,
                                    input_ids: Any,
                                    attention_mask: Any,
                                    decode_tokens: int) -> tuple[float, Any]:
    import torch

    started = time.perf_counter()
    with torch.no_grad():
        output = model(
            input_ids=input_ids,
            attention_mask=attention_mask,
            use_cache=True,
        )
        past_key_values = output.past_key_values
        next_token = torch.argmax(output.logits[:, -1, :], dim=-1, keepdim=True)
        decode_attention = attention_mask
        for _ in range(max(0, decode_tokens)):
            decode_attention = torch.cat(
                [decode_attention, torch.ones_like(next_token)],
                dim=1,
            )
            output = model(
                input_ids=next_token,
                attention_mask=decode_attention,
                past_key_values=past_key_values,
                use_cache=True,
            )
            past_key_values = output.past_key_values
            next_token = torch.argmax(output.logits[:, -1, :], dim=-1, keepdim=True)
    return (time.perf_counter() - started) * 1000.0, output.logits


def transformers_recompute_decode_once(model: Any,
                                       input_ids: Any,
                                       attention_mask: Any,
                                       decode_tokens: int) -> tuple[float, Any]:
    import torch

    generated = input_ids
    current_attention = attention_mask
    output = None
    started = time.perf_counter()
    with torch.no_grad():
        for _ in range(max(1, decode_tokens)):
            output = model(
                input_ids=generated,
                attention_mask=current_attention,
                use_cache=False,
            )
            next_token = torch.argmax(output.logits[:, -1, :], dim=-1, keepdim=True)
            generated = torch.cat([generated, next_token], dim=1)
            current_attention = torch.cat(
                [current_attention, torch.ones_like(next_token)],
                dim=1,
            )
    return (time.perf_counter() - started) * 1000.0, output.logits


def onnx_recompute_decode_once(session: Any,
                               input_ids: Any,
                               attention_mask: Any,
                               decode_tokens: int) -> tuple[float, np.ndarray]:
    import torch

    generated = input_ids
    current_attention = attention_mask
    logits = None
    started = time.perf_counter()
    for _ in range(max(1, decode_tokens)):
        logits = run_onnx_logits(session, generated, current_attention)
        next_value = int(np.argmax(logits[:, -1, :], axis=-1)[0])
        next_token = torch.tensor([[next_value]], dtype=generated.dtype)
        generated = torch.cat([generated, next_token], dim=1)
        current_attention = torch.cat(
            [current_attention, torch.ones_like(next_token)],
            dim=1,
        )
    return (time.perf_counter() - started) * 1000.0, logits


def benchmark_decode(fn, warmup: int, iterations: int) -> tuple[list[float], Any]:
    logits = None
    for _ in range(max(0, warmup)):
        _, logits = fn()
    samples: list[float] = []
    for _ in range(max(1, iterations)):
        elapsed_ms, logits = fn()
        samples.append(elapsed_ms)
    return samples, logits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--prompt", default=DEFAULT_PROMPT)
    parser.add_argument("--prompt-repeat", type=int, default=1)
    parser.add_argument("--output-dir", default="results/qwen_full_onnx_vs_transformers")
    parser.add_argument("--allow-download", action="store_true")
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--dtype", choices=("float32", "auto"), default="float32")
    parser.add_argument("--onnx-path", default="")
    parser.add_argument("--intra-op-threads", type=int, default=0)
    parser.add_argument("--decode-tokens", type=int, default=0)
    args = parser.parse_args()

    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    output_dir = Path(args.output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = Path(args.onnx_path).expanduser().resolve() if args.onnx_path else (
        output_dir / "qwen2.5-0.5b-full.onnx"
    )
    local_files_only = not args.allow_download
    dtype = torch.float32 if args.dtype == "float32" else "auto"

    started = time.perf_counter()
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
        attn_implementation="eager",
    )
    model.eval()
    load_ms = (time.perf_counter() - started) * 1000.0

    prompt = repeated_prompt(args.prompt, args.prompt_repeat)
    encoded = tokenizer(prompt, return_tensors="pt")
    input_ids = encoded["input_ids"]
    attention_mask = encoded.get("attention_mask")
    if attention_mask is None:
        attention_mask = torch.ones_like(input_ids)

    transformers_samples, torch_logits = benchmark_transformers(
        model,
        input_ids,
        attention_mask,
        args.warmup,
        args.iterations,
    )
    export_ms = export_onnx_if_needed(
        model,
        input_ids,
        attention_mask,
        onnx_path,
        args.opset,
    )
    onnx_session, onnx_session_init_ms = make_onnx_session(
        onnx_path,
        args.intra_op_threads,
    )
    onnx_samples, onnx_logits = benchmark_onnx_session(
        onnx_session,
        input_ids,
        attention_mask,
        args.warmup,
        args.iterations,
    )
    decode_summary = {}
    if args.decode_tokens > 0:
        cached_samples, _ = benchmark_decode(
            lambda: transformers_cached_decode_once(
                model, input_ids, attention_mask, args.decode_tokens),
            args.warmup,
            args.iterations,
        )
        torch_recompute_samples, _ = benchmark_decode(
            lambda: transformers_recompute_decode_once(
                model, input_ids, attention_mask, args.decode_tokens),
            args.warmup,
            args.iterations,
        )
        onnx_recompute_samples, _ = benchmark_decode(
            lambda: onnx_recompute_decode_once(
                onnx_session, input_ids, attention_mask, args.decode_tokens),
            args.warmup,
            args.iterations,
        )
        decode_summary = {
            "decodeTokens": int(args.decode_tokens),
            "transformersCachedTotal": summarize(cached_samples),
            "transformersCachedPerTokenMs": summarize([
                value / max(1, args.decode_tokens)
                for value in cached_samples
            ]),
            "transformersRecomputeTotal": summarize(torch_recompute_samples),
            "transformersRecomputePerTokenMs": summarize([
                value / max(1, args.decode_tokens)
                for value in torch_recompute_samples
            ]),
            "onnxRecomputeTotal": summarize(onnx_recompute_samples),
            "onnxRecomputePerTokenMs": summarize([
                value / max(1, args.decode_tokens)
                for value in onnx_recompute_samples
            ]),
        }

    torch_last = torch_logits.detach().cpu().numpy()[:, -1, :]
    onnx_last = onnx_logits[:, -1, :]
    max_abs_diff = float(np.max(np.abs(torch_last - onnx_last)))
    torch_top = int(np.argmax(torch_last, axis=-1)[0])
    onnx_top = int(np.argmax(onnx_last, axis=-1)[0])
    summary = {
        "schema": "ndnsf-di-qwen-full-onnx-vs-transformers-local-benchmark-v1",
        "model": args.model,
        "prompt": prompt,
        "promptRepeat": int(args.prompt_repeat),
        "sequenceLength": int(input_ids.shape[1]),
        "warmup": max(0, args.warmup),
        "iterations": max(1, args.iterations),
        "dtype": args.dtype,
        "opset": args.opset,
        "loadMs": load_ms,
        "onnxExportMs": export_ms,
        "onnxSessionInitMs": onnx_session_init_ms,
        "onnxPath": str(onnx_path),
        "onnxSizeBytes": onnx_path.stat().st_size if onnx_path.exists() else 0,
        "transformers": summarize(transformers_samples),
        "onnxruntime": summarize(onnx_samples),
        "maxAbsDiffLastTokenLogits": max_abs_diff,
        "transformersTopToken": torch_top,
        "onnxTopToken": onnx_top,
        "topTokenMatch": torch_top == onnx_top,
        "decodeLike": decode_summary,
    }
    summary_path = output_dir / "qwen-full-onnx-vs-transformers-summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    print(
        "QWEN_FULL_ONNX_VS_TRANSFORMERS "
        f"model={args.model} seq_len={summary['sequenceLength']} "
        f"prompt_repeat={summary['promptRepeat']} "
        f"iterations={summary['iterations']} warmup={summary['warmup']} "
        f"load_ms={load_ms:.2f} export_ms={export_ms:.2f} "
        f"onnx_session_init_ms={onnx_session_init_ms:.2f} "
        f"transformers_p50_ms={summary['transformers']['p50Ms']:.2f} "
        f"onnx_p50_ms={summary['onnxruntime']['p50Ms']:.2f} "
        f"transformers_avg_ms={summary['transformers']['avgMs']:.2f} "
        f"onnx_avg_ms={summary['onnxruntime']['avgMs']:.2f} "
        f"max_abs_diff={max_abs_diff:.6g} "
        f"top_match={str(summary['topTokenMatch']).lower()} "
        f"summary={summary_path}"
    )
    if decode_summary:
        print(
            "QWEN_DECODE_LIKE_ONNX_VS_TRANSFORMERS "
            f"decode_tokens={args.decode_tokens} "
            f"transformers_cached_p50_ms="
            f"{decode_summary['transformersCachedTotal']['p50Ms']:.2f} "
            f"transformers_cached_per_token_p50_ms="
            f"{decode_summary['transformersCachedPerTokenMs']['p50Ms']:.2f} "
            f"transformers_recompute_p50_ms="
            f"{decode_summary['transformersRecomputeTotal']['p50Ms']:.2f} "
            f"onnx_recompute_p50_ms="
            f"{decode_summary['onnxRecomputeTotal']['p50Ms']:.2f} "
            f"onnx_recompute_per_token_p50_ms="
            f"{decode_summary['onnxRecomputePerTokenMs']['p50Ms']:.2f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
