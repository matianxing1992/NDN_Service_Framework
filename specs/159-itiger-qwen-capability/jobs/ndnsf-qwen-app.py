#!/usr/bin/env python3
"""One-stage secured NDNSF-DI Qwen capability application."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import time


SERVICE = "/AI/LLM/Qwen"
GROUP = "/example/hello/group"
CONTROLLER = "/example/hello/controller"
PROVIDER = "/example/hello/provider"
USER = "/example/hello/user"
MODEL = Path("/models/qwen")
MODEL_REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
MODEL_WEIGHT_SHA256 = "fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe"


def run_provider(trust_schema: str) -> int:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer
    from ndnsf import ServiceProvider

    if not torch.cuda.is_available():
        raise RuntimeError("NDNSF_QWEN_CUDA_UNAVAILABLE")
    torch.manual_seed(0)
    torch.cuda.manual_seed_all(0)
    tokenizer = AutoTokenizer.from_pretrained(
        MODEL, local_files_only=True, trust_remote_code=False
    )
    model = AutoModelForCausalLM.from_pretrained(
        MODEL,
        local_files_only=True,
        trust_remote_code=False,
        torch_dtype=torch.float16,
        attn_implementation="eager",
    )
    model.eval()
    model.to("cuda:0")
    if model.device.type != "cuda":
        raise RuntimeError("NDNSF_QWEN_MODEL_CPU_FALLBACK")

    provider = ServiceProvider(
        group=GROUP,
        controller=CONTROLLER,
        provider_prefix=PROVIDER,
        trust_schema=trust_schema,
        handler_threads=1,
        ack_threads=1,
    )

    @provider.handler(SERVICE)
    def qwen(request: bytes) -> bytes:
        started = time.perf_counter()
        doc = json.loads(request.decode("utf-8"))
        prompt = str(doc["prompt"])
        request_id = str(doc["requestId"])
        messages = [
            {"role": "system", "content": "You are a concise validation assistant."},
            {"role": "user", "content": prompt},
        ]
        rendered = tokenizer.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=True
        )
        encoded = tokenizer(rendered, return_tensors="pt")
        encoded = {name: value.to("cuda:0") for name, value in encoded.items()}
        if any(value.device.type != "cuda" for value in encoded.values()):
            raise RuntimeError("NDNSF_QWEN_INPUT_CPU_FALLBACK")
        with torch.inference_mode():
            generated = model.generate(
                **encoded,
                do_sample=False,
                max_new_tokens=24,
                pad_token_id=tokenizer.eos_token_id,
            )
        torch.cuda.synchronize()
        suffix = generated[0, encoded["input_ids"].shape[1]:]
        text = tokenizer.decode(suffix, skip_special_tokens=True).strip()
        if suffix.numel() == 0 or not text:
            raise RuntimeError("NDNSF_QWEN_EMPTY_GENERATION")
        result = {
            "schemaVersion": "spec159-ndnsf-di-qwen-response-v1",
            "status": "PASS",
            "service": SERVICE,
            "requestId": request_id,
            "repository": "Qwen/Qwen2.5-0.5B-Instruct",
            "revision": MODEL_REVISION,
            "modelWeightSha256": "sha256:" + MODEL_WEIGHT_SHA256,
            "backend": "transformers-cuda",
            "cpuFallback": False,
            "gpuUuid": _gpu_uuid(),
            "gpuName": torch.cuda.get_device_name(0),
            "generatedText": text,
            "generatedTokenIds": suffix.tolist(),
            "generatedTokens": int(suffix.numel()),
            "providerInferenceMs": round((time.perf_counter() - started) * 1000, 3),
        }
        print(
            "SPEC159_QWEN_PROVIDER_EXEC "
            f"requestId={request_id} backend=transformers-cuda "
            f"gpuUuid={result['gpuUuid']} generatedTokens={result['generatedTokens']}",
            flush=True,
        )
        return json.dumps(result, sort_keys=True).encode("utf-8")

    print(
        "SPEC159_NDNSF_PROVIDER_READY "
        f"service={SERVICE} backend=transformers-cuda gpuUuid={_gpu_uuid()}",
        flush=True,
    )
    return provider.run(SERVICE)


def _gpu_uuid() -> str:
    import subprocess

    query = subprocess.run(
        ["nvidia-smi", "--query-gpu=uuid", "--format=csv,noheader"],
        text=True,
        capture_output=True,
        check=True,
    )
    rows = [line.strip() for line in query.stdout.splitlines() if line.strip()]
    if len(rows) != 1:
        raise RuntimeError("NDNSF_QWEN_GPU_ALLOCATION_AMBIGUOUS")
    return rows[0]


def run_user(trust_schema: str, output: str, request_id: str) -> int:
    from ndnsf import ServiceUser

    prompt = "Reply with one short sentence confirming secured NDNSF-DI Qwen inference."
    payload = json.dumps(
        {
            "schemaVersion": "spec159-ndnsf-di-qwen-request-v1",
            "requestId": request_id,
            "prompt": prompt,
        },
        sort_keys=True,
    ).encode("utf-8")
    user = ServiceUser(
        group=GROUP,
        controller=CONTROLLER,
        user=USER,
        trust_schema=trust_schema,
        permission_wait_ms=5000,
        handler_threads=1,
        ack_threads=1,
    )
    started = time.perf_counter()
    response = user.request_service(
        SERVICE,
        payload,
        ack_timeout_ms=500,
        timeout_ms=30000,
        strategy="first-responding",
        request_id=request_id,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000
    if not response.status:
        raise RuntimeError("NDNSF_QWEN_REQUEST_FAILED:" + response.error)
    doc = json.loads(bytes(response.payload).decode("utf-8"))
    doc["requesterElapsedMs"] = round(elapsed_ms, 3)
    doc["requestPayloadSha256"] = "sha256:" + hashlib.sha256(payload).hexdigest()
    Path(output).write_text(
        json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "SPEC159_QWEN_REQUESTER_RESULT "
        f"requestId={request_id} backend={doc.get('backend')} "
        f"gpuUuid={doc.get('gpuUuid')} generatedText={json.dumps(doc.get('generatedText'))}",
        flush=True,
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=("provider", "user"))
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--output", default="/scratch/ndnsf-result.json")
    parser.add_argument("--request-id", default="spec159-qwen-live-001")
    args = parser.parse_args()
    if args.role == "provider":
        return run_provider(args.trust_schema)
    return run_user(args.trust_schema, args.output, args.request_id)


if __name__ == "__main__":
    raise SystemExit(main())
