#!/usr/bin/env python3
"""Bounded standalone reference for the Spec170 three-stage Qwen ONNX graph.

This is deliberately not an NDNSF network path.  It validates the immutable
stage artifacts, tokenizer, ONNX Runtime provider selection, KV-cache wiring,
and a short greedy multi-token decode before the artifacts are used by a
Provider.  The manifest is rewritten only in memory so its source paths do
not become container paths or evidence identities.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cpu")
    parser.add_argument("--prompt", default="Explain NDNSF in one sentence.")
    parser.add_argument("--max-new-tokens", type=int, default=2)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    if not 1 <= args.max_new_tokens <= 8:
        raise SystemExit("--max-new-tokens must be between 1 and 8")

    import numpy as np  # type: ignore
    import onnxruntime as ort  # type: ignore
    from transformers import AutoTokenizer  # type: ignore

    manifest_path = Path(args.manifest).resolve()
    artifact_root = Path(args.artifact_root).resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stages = sorted(manifest["stages"], key=lambda item: int(item["stageIndex"]))
    if len(stages) != int(manifest["stageCount"]):
        raise RuntimeError("QWEN_STAGE_COUNT_MISMATCH")

    artifacts = []
    for stage in stages:
        path = artifact_root / Path(stage["path"]).name
        actual = sha256_file(path)
        declared = "sha256:" + str(stage["sha256"])
        if actual != declared:
            raise RuntimeError(
                f"QWEN_ARTIFACT_DIGEST_MISMATCH role={stage['role']} "
                f"expected={declared} actual={actual}")
        if path.stat().st_size != int(stage["bytes"]):
            raise RuntimeError(f"QWEN_ARTIFACT_SIZE_MISMATCH role={stage['role']}")
        artifacts.append({"role": stage["role"], "path": str(path), "sha256": actual})

    is_cuda = args.device.startswith("cuda:")
    runtime_provider = "CUDAExecutionProvider" if is_cuda else "CPUExecutionProvider"
    if runtime_provider not in ort.get_available_providers():
        raise RuntimeError(f"QWEN_REQUIRED_PROVIDER_UNAVAILABLE:{runtime_provider}")

    sessions = []
    for stage in stages:
        options = ort.SessionOptions()
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        if is_cuda:
            options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
            provider_spec = (runtime_provider, {"device_id": 0})
        else:
            provider_spec = runtime_provider
        session = ort.InferenceSession(
            artifacts[len(sessions)]["path"],
            sess_options=options,
            providers=[provider_spec],
        )
        active = tuple(session.get_providers())
        if not active or active[0] != runtime_provider:
            raise RuntimeError(
                f"QWEN_PROVIDER_NOT_SELECTED role={stage['role']} active={active}")
        sessions.append((stage, session))

    tokenizer = AutoTokenizer.from_pretrained(
        args.model, local_files_only=True, trust_remote_code=True)
    prompt_ids = tokenizer(args.prompt, return_tensors="np")["input_ids"].astype(np.int64)

    def decode_once() -> tuple[list[int], list[float]]:
        generated: list[int] = []
        timings: list[float] = []
        context_ids = prompt_ids.copy()
        for _ in range(args.max_new_tokens):
            started = time.perf_counter()
            # The current NDNSF-DI stage runner deliberately uses a bounded
            # stateless path: it carries the full attention mask but does not
            # feed present_key/value back as the next request's past cache.
            caches: list[dict[str, np.ndarray]] = [dict() for _ in sessions]
            position_ids = np.arange(
                context_ids.shape[1], dtype=np.int64,
            )[None, :]
            current_ids = context_ids
            attention_mask = np.ones_like(context_ids, dtype=np.int64)
            hidden_states = None
            logits = None
            for index, (stage, session) in enumerate(sessions):
                feed: dict[str, np.ndarray] = {
                    "attention_mask": attention_mask,
                    "position_ids": position_ids,
                }
                if index == 0:
                    feed["input_ids"] = current_ids
                else:
                    if hidden_states is None:
                        raise RuntimeError("QWEN_HIDDEN_STATE_CHAIN_MISSING")
                    feed["hidden_states"] = hidden_states
                for name in stage["cacheInputs"]:
                    value = caches[index].get(name)
                    if value is None:
                        shape = stage["tensorContracts"][name]["shape"]
                        value = np.empty(
                            (current_ids.shape[0], int(shape[1]), 0, int(shape[3])),
                            dtype=np.float32,
                        )
                    feed[name] = value
                names = [item.name for item in session.get_outputs()]
                values = session.run(None, feed)
                outputs = dict(zip(names, values))
                print(
                    "SPEC170_QWEN_STAGE_EXECUTION "
                    f"role={stage['role']} device={args.device} "
                    f"inputSeq={current_ids.shape[1]} "
                    f"attentionSeq={attention_mask.shape[1]}",
                    flush=True,
                )
                for input_name, output_name in zip(
                        stage["cacheInputs"], stage["cacheOutputs"]):
                    caches[index][input_name] = outputs[output_name]
                if index + 1 == len(sessions):
                    logits = outputs["logits"]
                else:
                    hidden_states = outputs["hidden_states_out"]
            if logits is None:
                raise RuntimeError("QWEN_LOGITS_MISSING")
            token = int(np.argmax(logits[:, -1, :], axis=-1)[0])
            generated.append(token)
            timings.append((time.perf_counter() - started) * 1000.0)
            context_ids = np.concatenate(
                [context_ids, np.asarray([[token]], dtype=np.int64)], axis=1)
        return generated, timings

    warmup_tokens, _ = decode_once()
    first_tokens, first_timings = decode_once()
    second_tokens, second_timings = decode_once()
    if first_tokens != second_tokens:
        raise RuntimeError("QWEN_GREEDY_NONDETERMINISTIC")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    summary = {
        "schema": "spec170-qwen-onnx-standalone-v1",
        "device": args.device,
        "runtimeProvider": runtime_provider,
        "runtimeVersion": ort.__version__,
        "model": args.model,
        "modelRevision": manifest.get("modelRevision", ""),
        "manifestSha256": sha256_file(manifest_path),
        "prompt": args.prompt,
        "promptTokenCount": int(prompt_ids.shape[1]),
        "maxNewTokens": args.max_new_tokens,
        "warmupTokens": warmup_tokens,
        "generatedTokens": first_tokens,
        "firstTimingsMs": first_timings,
        "secondTimingsMs": second_timings,
        "artifacts": artifacts,
        "activeProviders": [list(session.get_providers()) for _, session in sessions],
        "cpuFallbackDisabled": bool(is_cuda),
        "cacheMode": "stateless-zero-full-context-recompute",
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(
        "SPEC170_QWEN_STANDALONE_OK "
        f"device={args.device} stages={len(stages)} "
        f"tokens={json.dumps(first_tokens, separators=(',', ':'))} "
        f"summary={summary_path}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
