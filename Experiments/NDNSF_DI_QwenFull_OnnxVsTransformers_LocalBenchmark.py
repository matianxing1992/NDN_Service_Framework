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
import hashlib
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


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def make_staged_sessions(manifest: dict[str, Any], intra_op_threads: int):
    import onnxruntime as ort

    sessions = []
    for stage in sorted(manifest["stages"], key=lambda item: int(item["stageIndex"])):
        opts = ort.SessionOptions()
        opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        if intra_op_threads > 0:
            opts.intra_op_num_threads = intra_op_threads
        sessions.append((
            stage,
            ort.InferenceSession(
                str(stage["path"]), sess_options=opts,
                providers=["CPUExecutionProvider"],
            ),
        ))
    return sessions


def staged_kv_generate_once(sessions, prompt_ids: np.ndarray,
                            max_new_tokens: int) -> tuple[list[int], list[float]]:
    generated: list[int] = []
    step_ms: list[float] = []
    attention_mask = np.ones_like(prompt_ids, dtype=np.int64)
    cache_by_stage: list[dict[str, np.ndarray]] = [dict() for _ in sessions]
    current_ids = prompt_ids
    for token_index in range(max_new_tokens):
        started = time.perf_counter()
        position_ids = np.arange(
            attention_mask.shape[1] - current_ids.shape[1],
            attention_mask.shape[1], dtype=np.int64,
        )[None, :]
        hidden_states = None
        logits = None
        for stage_index, (stage, session) in enumerate(sessions):
            feed: dict[str, np.ndarray] = {
                "attention_mask": attention_mask,
                "position_ids": position_ids,
            }
            if stage_index == 0:
                feed["input_ids"] = current_ids
            else:
                if hidden_states is None:
                    raise RuntimeError("staged baseline has no hidden-state input")
                feed["hidden_states"] = hidden_states
            for name in stage["cacheInputs"]:
                cached = cache_by_stage[stage_index].get(name)
                if cached is None:
                    shape = stage["tensorContracts"][name]["shape"]
                    cached = np.empty(
                        (current_ids.shape[0], int(shape[1]), 0, int(shape[3])),
                        dtype=np.float32,
                    )
                feed[name] = cached
            output_names = list(stage["outputNames"])
            values = session.run(output_names, feed)
            outputs = dict(zip(output_names, values))
            for input_name, output_name in zip(
                    stage["cacheInputs"], stage["cacheOutputs"]):
                cache_by_stage[stage_index][input_name] = outputs[output_name]
            if stage_index + 1 == len(sessions):
                logits = outputs["logits"]
            else:
                hidden_states = outputs["hidden_states_out"]
        if logits is None:
            raise RuntimeError("staged baseline produced no logits")
        token = int(np.argmax(logits[:, -1, :], axis=-1)[0])
        generated.append(token)
        step_ms.append((time.perf_counter() - started) * 1000.0)
        current_ids = np.asarray([[token]], dtype=np.int64)
        attention_mask = np.concatenate(
            [attention_mask, np.ones((attention_mask.shape[0], 1), dtype=np.int64)],
            axis=1,
        )
    return generated, step_ms


def run_matched_staged_baseline(args) -> int:
    import onnxruntime as ort
    from transformers import AutoTokenizer

    manifest_path = Path(args.qwen_service_manifest).expanduser().resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    tokenizer = AutoTokenizer.from_pretrained(
        args.model,
        local_files_only=not args.allow_download,
        trust_remote_code=True,
    )
    prompt = repeated_prompt(args.prompt, args.prompt_repeat)
    prompt_ids = tokenizer(prompt, return_tensors="np")["input_ids"].astype(np.int64)
    sessions = make_staged_sessions(manifest, args.intra_op_threads)
    for _ in range(max(0, args.warmup)):
        staged_kv_generate_once(sessions, prompt_ids, args.max_new_tokens)
    totals: list[float] = []
    ttft: list[float] = []
    inter_token: list[float] = []
    sequences: list[list[int]] = []
    for _ in range(max(1, args.iterations)):
        generated, steps = staged_kv_generate_once(
            sessions, prompt_ids, args.max_new_tokens)
        sequences.append(generated)
        totals.append(sum(steps))
        ttft.append(steps[0])
        inter_token.extend(steps[1:])
    if any(sequence != sequences[0] for sequence in sequences[1:]):
        raise RuntimeError("matched staged baseline is not token deterministic")
    artifacts = []
    for stage in manifest["stages"]:
        path = Path(stage["path"])
        artifacts.append({
            "role": stage["role"],
            "path": str(path),
            "sha256": sha256_file(path),
            "declaredSha256": "sha256:" + str(stage["sha256"]),
        })
    summary = {
        "schema": "ndnsf-di-qwen-matched-single-node-baseline-v1",
        "model": args.model,
        "modelRevision": manifest.get("modelRevision", ""),
        "tokenizer": manifest.get("tokenizer", args.model),
        "prompt": prompt,
        "inputTokenIds": prompt_ids.tolist(),
        "inputTokenCount": int(prompt_ids.shape[1]),
        "generation": {"strategy": "greedy", "batch": 1,
                       "maxNewTokens": int(args.max_new_tokens)},
        "generatedTokens": sequences[0],
        "warmup": max(0, args.warmup),
        "iterations": max(1, args.iterations),
        "backend": "onnxruntime-cpu",
        "runtimeVersion": ort.__version__,
        "providers": ort.get_available_providers(),
        "logging": "INFO",
        "serviceManifest": str(manifest_path),
        "serviceManifestSha256": sha256_file(manifest_path),
        "artifacts": artifacts,
        "total": summarize(totals),
        "ttft": summarize(ttft),
        "interToken": summarize(inter_token),
    }
    output_dir = Path(args.output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / "qwen-matched-single-node-summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "QWEN_MATCHED_SINGLE_NODE_OK",
        f"tokens={json.dumps(sequences[0], separators=(',', ':'))}",
        f"total_p50_ms={summary['total']['p50Ms']:.2f}",
        f"ttft_p50_ms={summary['ttft']['p50Ms']:.2f}",
        f"inter_token_p50_ms={summary['interToken']['p50Ms']:.2f}",
        f"summary={summary_path}",
    )
    return 0


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
    parser.add_argument("--qwen-service-manifest", default="")
    parser.add_argument("--max-new-tokens", type=int, default=32)
    args = parser.parse_args()

    if args.max_new_tokens < 1 or args.max_new_tokens > 32:
        raise SystemExit("--max-new-tokens must be between 1 and 32")
    if args.qwen_service_manifest:
        return run_matched_staged_baseline(args)

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
