#!/usr/bin/env python3
"""User for the validation LLM pipeline distributed inference example."""

from __future__ import annotations

import json
import statistics
import time
from pathlib import Path

from ndnsf_distributed_inference import APPClient

from llm_pipeline_lib import (
    QWEN_ONNX_RUNTIME,
    QWEN_TRANSFORMERS_RUNTIME,
    SERVICE,
    TINY_TRANSFORMERS_RUNTIME,
    decode_payload,
    decode_qwen_pipeline_context,
    encode_qwen_pipeline_delta,
    encode_qwen_pipeline_context,
    encode_prompt,
    merge_qwen_pipeline_delta,
    parse_common_args,
    run_qwen_onnx_stage,
    run_local_pipeline,
    run_local_tiny_transformer_pipeline,
    _decode_native_tensor_bundle,
    _native_tensor_bundle_payload,
)


def _percentile(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    index = (len(ordered) - 1) * percentile
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    if lower == upper:
        return ordered[lower]
    weight = index - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _parse_delta_token_ids(raw: str) -> list[int]:
    values = [part.strip() for part in raw.split(",") if part.strip()]
    if not values:
        return []
    return [int(value, 0) for value in values]


def _empty_delta_like(input_ids):
    return [[] for _ in input_ids]


class _LocalQwenOnnxRunner:
    def __init__(self, service_policy, stages: int):
        import onnxruntime as ort

        self._stages = int(stages)
        artifacts = [
            artifact for artifact in service_policy.artifacts
            if artifact.kind == "onnx-model" and
            (artifact.metadata or {}).get("runtime") == QWEN_ONNX_RUNTIME
        ]
        artifacts.sort(key=lambda item: int((item.metadata or {}).get("stageIndex", 0)))
        if len(artifacts) != self._stages:
            raise RuntimeError(
                f"expected {self._stages} Qwen ONNX artifacts, found {len(artifacts)}")
        self._runners = [
            (
                artifact.role,
                dict(artifact.metadata or {}),
                ort.InferenceSession(artifact.path, providers=["CPUExecutionProvider"]),
            )
            for artifact in artifacts
        ]

    def run(self, context_payload: bytes) -> dict:
        payload = context_payload
        for role, metadata, session in self._runners:
            payload = run_qwen_onnx_stage(
                payload,
                role=role,
                stages=self._stages,
                session=session,
                metadata=metadata,
                compute_delay_ms=0.0,
            )
        return decode_payload(payload)


def main() -> int:
    parser = parse_common_args("Run validation LLM pipeline user")
    parser.add_argument("--prompt", default="Explain NDNSF-DI pipeline inference.")
    parser.add_argument("--request-id", default="manual")
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--compute-delay-ms", type=float, default=1.0)
    parser.add_argument(
        "--runtime",
        choices=("fake", TINY_TRANSFORMERS_RUNTIME, QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME),
        default="fake",
    )
    parser.add_argument("--transformer-layers", type=int, default=4)
    parser.add_argument("--qwen-runtime-summary", default="")
    parser.add_argument("--native-cpu-provider", action="store_true")
    parser.add_argument("--qwen-service-manifest", default="")
    parser.add_argument("--max-new-tokens", type=int, default=1)
    parser.add_argument("--session-id", default="")
    parser.add_argument("--context-epoch", type=int, default=0)
    parser.add_argument(
        "--context-input-mode",
        choices=("full", "append-empty-delta-after-first", "append-token-delta-after-first"),
        default="full",
        help=(
            "Qwen context request shape. append-empty-delta-after-first keeps "
            "the expected output fixed; append-token-delta-after-first appends "
            "real token IDs and computes a per-request local ONNX expected token."
        ),
    )
    parser.add_argument(
        "--delta-token-ids",
        default="2",
        help="Comma-separated token IDs used by append-token-delta-after-first.",
    )
    parser.add_argument(
        "--publish-input-reference",
        action="store_true",
        help=(
            "Publish Qwen token_ids/attention_mask context through NDNSF "
            "large-data and send only the standard reference payload."
        ),
    )
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--warmup-requests", type=int, default=0)
    parser.add_argument("--measured-requests", type=int, default=1)
    parser.add_argument("--measured-duration-s", type=float, default=0.0)
    parser.add_argument("--request-interval-ms", type=float, default=0.0)
    parser.add_argument("--metrics-csv", default="")
    args = parser.parse_args()
    if args.max_new_tokens < 1 or args.max_new_tokens > 32:
        raise SystemExit("--max-new-tokens must be between 1 and 32")
    if args.native_cpu_provider and args.publish_input_reference:
        raise SystemExit("native CPU pilot does not yet accept referenced request bundles")

    qwen_summary = {}
    if args.runtime in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME) and args.qwen_runtime_summary:
        qwen_summary = json.loads(Path(args.qwen_runtime_summary).read_text(encoding="utf-8"))
    if args.runtime == QWEN_ONNX_RUNTIME:
        # Inter-stage transport is the NDITB001 typed tensor bundle. Legacy NPZ
        # decoding exists only inside the library as a labeled comparison fixture.
        print("LLM_PIPELINE_TENSOR_TRANSPORT typed-tensor-bundle", flush=True)
    if args.runtime in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
        if not qwen_summary:
            raise SystemExit("--qwen-runtime-summary is required for Qwen runtimes")
        session_id = args.session_id or (
            args.request_id if args.context_input_mode != "full" else ""
        )
        payload = encode_qwen_pipeline_context(
            qwen_summary["inputIds"],
            attention_mask=qwen_summary.get("attentionMask"),
            request_id=args.request_id,
            session_id=session_id,
            context_epoch=args.context_epoch,
        )
    else:
        payload = encode_prompt(args.prompt, request_id=args.request_id)
    if args.runtime == TINY_TRANSFORMERS_RUNTIME:
        local = run_local_tiny_transformer_pipeline(
            payload,
            stages=args.stages,
            layer_count=args.transformer_layers,
            compute_delay_ms=args.compute_delay_ms,
        )
    elif args.runtime in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
        local = type("LocalResult", (), {
            "payload": json.dumps({
                "schema": "ndnsf-di-qwen-transformer-response-v1",
                "runtime": args.runtime,
                "topToken": int(qwen_summary["expectedTopToken"]),
            }).encode("utf-8"),
            "elapsed_ms": float(qwen_summary.get("fullMs", 0.0)),
        })()
    else:
        local = run_local_pipeline(
            payload,
            stages=args.stages,
            compute_delay_ms=args.compute_delay_ms,
        )
    local_doc = decode_payload(local.payload)
    if args.dry_run:
        print(
            "LLM_PIPELINE_USER_DRY_RUN",
            f"local_ms={local.elapsed_ms:.2f}",
            json.dumps(local_doc, sort_keys=True),
        )
        return 0

    client = APPClient.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        group=args.group,
    )
    local_qwen_onnx = None
    if (
        args.runtime == QWEN_ONNX_RUNTIME and
        (args.native_cpu_provider or
         args.context_input_mode == "append-token-delta-after-first")
    ):
        local_qwen_onnx = _LocalQwenOnnxRunner(
            client.deployment.service_policy(SERVICE),
            stages=args.stages,
        )
    metrics_path = Path(args.metrics_csv) if args.metrics_csv else None
    metrics_file = None
    if metrics_path:
        metrics_path.parent.mkdir(parents=True, exist_ok=True)
        metrics_file = metrics_path.open("w", encoding="utf-8")
        metrics_file.write("phase,index,distributed_ms,status,error\n")

    measured_latencies: list[float] = []
    measured_count = 0
    deadline = (
        time.perf_counter() + args.measured_duration_s
        if args.measured_duration_s > 0 else
        None
    )
    total_limit = args.warmup_requests + max(1, args.measured_requests)
    index = 0
    qwen_session_id = args.session_id or (
        args.request_id if args.context_input_mode != "full" else ""
    )
    qwen_cached_epoch = args.context_epoch
    qwen_sent_full_context = False
    qwen_full_context_doc = None
    delta_token_ids = _parse_delta_token_ids(args.delta_token_ids)
    try:
        while True:
            phase = "warmup" if index < args.warmup_requests else "measured"
            if phase == "measured" and deadline is None and measured_count >= args.measured_requests:
                break
            if phase == "measured" and deadline is not None and measured_count > 0 and time.perf_counter() >= deadline:
                break
            if deadline is None and index >= total_limit:
                break

            if args.runtime in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
                request_id = f"{args.request_id}-{index}"
                if (
                    args.context_input_mode in {
                        "append-empty-delta-after-first",
                        "append-token-delta-after-first",
                    } and
                    qwen_sent_full_context
                ):
                    if qwen_full_context_doc is None:
                        raise RuntimeError("Qwen delta mode has no local full-context cache")
                    delta_ids = (
                        _empty_delta_like(qwen_full_context_doc["inputIds"])
                        if args.context_input_mode == "append-empty-delta-after-first" else
                        [list(delta_token_ids) for _ in qwen_full_context_doc["inputIds"]]
                    )
                    request_payload = encode_qwen_pipeline_delta(
                        delta_ids,
                        request_id=request_id,
                        session_id=qwen_session_id,
                        base_context_epoch=qwen_cached_epoch,
                        context_epoch=qwen_cached_epoch + 1,
                    )
                    delta_doc = decode_qwen_pipeline_context(request_payload)
                    qwen_full_context_doc = merge_qwen_pipeline_delta(
                        qwen_full_context_doc,
                        delta_doc,
                    )
                    qwen_cached_epoch += 1
                else:
                    request_payload = encode_qwen_pipeline_context(
                        qwen_summary["inputIds"],
                        attention_mask=qwen_summary.get("attentionMask"),
                        request_id=request_id,
                        session_id=qwen_session_id,
                        context_epoch=qwen_cached_epoch,
                    )
                    qwen_full_context_doc = decode_qwen_pipeline_context(request_payload)
                    qwen_sent_full_context = True
                expected_doc = local_doc
                if (
                    args.runtime == QWEN_ONNX_RUNTIME and
                    not args.native_cpu_provider and
                    local_qwen_onnx is not None and
                    qwen_full_context_doc is not None
                ):
                    expected_doc = local_qwen_onnx.run(encode_qwen_pipeline_context(
                        qwen_full_context_doc["inputIds"],
                        attention_mask=qwen_full_context_doc.get("attentionMask"),
                        position_ids=qwen_full_context_doc.get("positionIds"),
                        request_id=f"{request_id}-expected",
                        session_id=qwen_session_id,
                        context_epoch=int(qwen_full_context_doc.get("contextEpoch", 0) or 0),
                    ))
                if args.publish_input_reference:
                    request_payload = client.publish_large_payload_reference(
                        SERVICE,
                        request_payload,
                        object_label="qwen-context-input",
                        object_type="application/x-ndnsf-di-qwen-context+json",
                        freshness_ms=120000,
                    )
            else:
                request_payload = encode_prompt(
                    args.prompt,
                    request_id=f"{args.request_id}-{index}",
                )
                expected_doc = local_doc
            started = time.perf_counter()
            if args.native_cpu_provider:
                import numpy as np

                if local_qwen_onnx is None:
                    raise RuntimeError("native CPU pilot requires the local ONNX oracle")
                context_doc = decode_qwen_pipeline_context(request_payload)
                manifest = json.loads(
                    Path(args.qwen_service_manifest).read_text(encoding="utf-8"))
                logical_session = qwen_session_id or f"{args.request_id}-{index}"
                expected_tokens: list[int] = []
                generated_tokens: list[int] = []
                response = {}
                result = None
                for token_index in range(args.max_new_tokens):
                    oracle_payload = encode_qwen_pipeline_context(
                        context_doc["inputIds"],
                        attention_mask=context_doc.get("attentionMask"),
                        request_id=f"{args.request_id}-{index}-oracle-{token_index}",
                    )
                    oracle = local_qwen_onnx.run(oracle_payload)
                    expected_token = int(oracle["topToken"])
                    expected_tokens.append(expected_token)

                    full_ids = np.asarray(context_doc["inputIds"], dtype=np.int64)
                    full_mask = np.asarray(context_doc["attentionMask"], dtype=np.int64)
                    if token_index == 0:
                        ids = full_ids
                        positions = np.arange(
                            full_ids.shape[1], dtype=np.int64)[None, :]
                    else:
                        ids = full_ids[:, -1:]
                        positions = np.asarray(
                            [[full_ids.shape[1] - 1]], dtype=np.int64)
                    tensors = {
                        "input_ids": ids,
                        "attention_mask": full_mask,
                        "position_ids": positions,
                    }
                    if token_index == 0:
                        for stage in manifest["stages"]:
                            for name in stage["cacheInputs"]:
                                shape = stage["tensorContracts"][name]["shape"]
                                tensors[name] = np.empty(
                                    (ids.shape[0], int(shape[1]), 0, int(shape[3])),
                                    dtype=np.float32,
                                )
                    native_payload = _native_tensor_bundle_payload(tensors)
                    kv_mode = "full-context" if token_index == 0 else "cache-hit"
                    requirement = (
                        f"kvMode={kv_mode};kvSessionId={logical_session};"
                        f"kvContextEpoch={token_index};"
                        f"kvNextContextEpoch={token_index + 1};"
                        "kvSecurityEpoch=0;"
                    ).encode("utf-8")
                    result = client.distributed_inference(
                        SERVICE,
                        native_payload,
                        dynamic_provisioning=False,
                        ack_timeout_ms=args.ack_timeout_ms,
                        timeout_ms=args.timeout_ms,
                        role_app_requirements={
                            str(stage["role"]): requirement
                            for stage in manifest["stages"]
                        },
                    )
                    if not result.status:
                        break
                    native_response = _decode_native_tensor_bundle(result.payload)
                    logits = np.asarray(native_response["logits"])
                    token = int(np.argmax(logits[:, -1, :], axis=-1)[0])
                    generated_tokens.append(token)
                    if token != expected_token:
                        print("LLM_PIPELINE_USER_TOKEN_MISMATCH",
                              f"tokenIndex={token_index}",
                              f"expected={expected_token}", f"actual={token}")
                        return 3
                    context_doc["inputIds"] = [
                        [*row, token] for row in context_doc["inputIds"]
                    ]
                    context_doc["attentionMask"] = [
                        [*row, 1] for row in context_doc["attentionMask"]
                    ]
                if result is None:
                    raise RuntimeError("native bounded generation executed no token steps")
                expected_doc = {
                    "topToken": expected_tokens[-1],
                    "generatedTokens": expected_tokens,
                }
                response = {
                    "schema": "ndnsf-di-qwen-onnx-response-v1",
                    "topToken": generated_tokens[-1] if generated_tokens else -1,
                    "generatedTokens": generated_tokens,
                    "tokenCount": len(generated_tokens),
                }
            else:
                result = client.distributed_inference(
                    SERVICE,
                    request_payload,
                    dynamic_provisioning=False,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )
            distributed_ms = (time.perf_counter() - started) * 1000.0
            if not result.status:
                if metrics_file:
                    metrics_file.write(
                        f"{phase},{index},{distributed_ms:.3f},failed,"
                        f"{json.dumps(str(result.error))}\n"
                    )
                print(
                    "LLM_PIPELINE_USER_FAILED",
                    f"phase={phase}",
                    f"index={index}",
                    f"error={result.error}",
                    f"local_ms={local.elapsed_ms:.2f}",
                )
                return 2
            if not args.native_cpu_provider:
                response = decode_payload(result.payload)
            if args.runtime in (TINY_TRANSFORMERS_RUNTIME, QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
                matches = (
                    response.get("generatedTokens") == expected_doc.get("generatedTokens")
                    if args.native_cpu_provider else
                    response.get("topToken") == expected_doc.get("topToken")
                )
            else:
                matches = response.get("lineage") == expected_doc.get("lineage")
            if not matches:
                print("LLM_PIPELINE_USER_LINEAGE_MISMATCH")
                print("local", json.dumps(expected_doc, sort_keys=True))
                print("distributed", json.dumps(response, sort_keys=True))
                return 3
            if phase == "measured":
                measured_latencies.append(distributed_ms)
                measured_count += 1
            if metrics_file:
                metrics_file.write(f"{phase},{index},{distributed_ms:.3f},ok,\"\"\n")
                metrics_file.flush()
            print(
                "LLM_PIPELINE_USER_RESPONSE",
                f"phase={phase}",
                f"index={index}",
                f"local_ms={local.elapsed_ms:.2f}",
                f"distributed_ms={distributed_ms:.2f}",
                f"stages={args.stages}",
                f"runtime={args.runtime}",
                f"expectedTopToken={expected_doc.get('topToken', '')}",
                f"lineage={','.join(response.get('lineage', []))}",
                json.dumps(response, sort_keys=True),
            )
            index += 1
            if args.request_interval_ms > 0:
                time.sleep(args.request_interval_ms / 1000.0)
    finally:
        if metrics_file:
            metrics_file.close()
    if measured_latencies:
        print(
            "LLM_PIPELINE_USER_SUMMARY",
            f"count={len(measured_latencies)}",
            f"local_ms={local.elapsed_ms:.2f}",
            f"avg_ms={statistics.fmean(measured_latencies):.2f}",
            f"p50_ms={statistics.median(measured_latencies):.2f}",
            f"p95_ms={_percentile(measured_latencies, 0.95):.2f}",
            f"min_ms={min(measured_latencies):.2f}",
            f"max_ms={max(measured_latencies):.2f}",
            f"runtime={args.runtime}",
            f"metrics_csv={metrics_path or ''}",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
