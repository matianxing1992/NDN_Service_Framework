#!/usr/bin/env python3
"""Run the shared NDNSF-DI Qwen ONNX stage runner on immutable artifacts.

This is a bounded artifact/runtime gate.  It deliberately does not start NFD,
Providers, or a distributed request; its purpose is to validate that the
shared ``run_qwen_onnx_stage`` helper consumes the same real stage manifest
that the NDNSF provider path will use.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import time


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def load_runner(path: Path):
    spec = importlib.util.spec_from_file_location("spec170_qwen_runner", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load stage runner: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--device", choices=("cpu", "cuda:0"), default="cpu")
    parser.add_argument("--prompt", default="Explain NDNSF in one sentence.")
    parser.add_argument("--max-new-tokens", type=int, default=2)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not 1 <= args.max_new_tokens <= 4:
        raise SystemExit("--max-new-tokens must be between 1 and 4")

    import numpy as np  # type: ignore
    import onnxruntime as ort  # type: ignore
    from transformers import AutoTokenizer  # type: ignore

    runner = load_runner(Path(args.runner).resolve())
    manifest_path = Path(args.manifest).resolve()
    artifact_root = Path(args.artifact_root).resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stages = sorted(manifest["stages"], key=lambda item: int(item["stageIndex"]))
    if len(stages) != int(manifest["stageCount"]):
        raise RuntimeError("QWEN_STAGE_COUNT_MISMATCH")

    is_cuda = args.device.startswith("cuda:")
    provider_name = "CUDAExecutionProvider" if is_cuda else "CPUExecutionProvider"
    if provider_name not in ort.get_available_providers():
        raise RuntimeError(f"QWEN_REQUIRED_PROVIDER_UNAVAILABLE:{provider_name}")

    sessions = []
    artifact_records = []
    for stage in stages:
        path = artifact_root / Path(stage["path"]).name
        expected = "sha256:" + str(stage["sha256"])
        actual = sha256_file(path)
        if actual != expected:
            raise RuntimeError(
                f"QWEN_ARTIFACT_DIGEST_MISMATCH role={stage['role']} "
                f"expected={expected} actual={actual}")
        if path.stat().st_size != int(stage["bytes"]):
            raise RuntimeError(f"QWEN_ARTIFACT_SIZE_MISMATCH role={stage['role']}")
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        if is_cuda:
            options.add_session_config_entry(
                "session.disable_cpu_ep_fallback", "1")
            providers = [(provider_name, {"device_id": 0})]
        else:
            providers = [provider_name]
        session = ort.InferenceSession(
            str(path), sess_options=options, providers=providers)
        active = tuple(session.get_providers())
        if not active or active[0] != provider_name:
            raise RuntimeError(
                f"QWEN_PROVIDER_NOT_SELECTED role={stage['role']} active={active}")
        sessions.append(session)
        artifact_records.append({
            "role": stage["role"],
            "path": str(path),
            "sha256": actual,
            "providers": list(active),
        })

    tokenizer = AutoTokenizer.from_pretrained(
        args.model, local_files_only=True, trust_remote_code=True)
    prompt_ids = tokenizer(args.prompt, return_tensors="np")["input_ids"].astype(
        np.int64)

    def decode_once() -> tuple[list[int], list[float]]:
        context_ids = prompt_ids.copy()
        tokens: list[int] = []
        timings: list[float] = []
        for epoch in range(args.max_new_tokens):
            payload = runner.encode_qwen_pipeline_context(
                context_ids.tolist(),
                request_id=f"spec170-stage-runner-{epoch}",
                context_epoch=epoch,
            )
            started = time.perf_counter()
            for index, (stage, session) in enumerate(zip(stages, sessions)):
                metadata = dict(stage)
                metadata.setdefault("stageCount", len(stages))
                payload = runner.run_qwen_onnx_stage(
                    payload,
                    role=str(stage["role"]),
                    stages=len(stages),
                    session=session,
                    metadata=metadata,
                    timing={},
                )
            result = json.loads(payload.decode("utf-8"))
            token = int(result["topToken"])
            tokens.append(token)
            timings.append((time.perf_counter() - started) * 1000.0)
            context_ids = np.concatenate(
                [context_ids, np.asarray([[token]], dtype=np.int64)], axis=1)
        return tokens, timings

    warmup_tokens, _ = decode_once()
    first_tokens, first_timings = decode_once()
    second_tokens, second_timings = decode_once()
    if first_tokens != second_tokens:
        raise RuntimeError("QWEN_GREEDY_NONDETERMINISTIC")

    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    summary = {
        "schema": "spec170-qwen-onnx-stage-runner-probe-v1",
        "manifestSha256": sha256_file(manifest_path),
        "model": args.model,
        "modelRevision": manifest.get("modelRevision", ""),
        "device": args.device,
        "runtimeProvider": provider_name,
        "cpuFallbackDisabled": is_cuda,
        "promptTokenCount": int(prompt_ids.shape[1]),
        "maxNewTokens": args.max_new_tokens,
        "warmupTokens": warmup_tokens,
        "generatedTokens": first_tokens,
        "firstTimingsMs": first_timings,
        "secondTimingsMs": second_timings,
        "artifacts": artifact_records,
        "runnerSha256": sha256_file(Path(args.runner).resolve()),
    }
    output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(
        "SPEC170_QWEN_STAGE_RUNNER_PROBE_PASS "
        f"device={args.device} stages={len(stages)} "
        f"tokens={json.dumps(first_tokens, separators=(',', ':'))} "
        f"summary={output}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
