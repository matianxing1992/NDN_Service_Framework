#!/usr/bin/env python3
"""Provider for the validation LLM pipeline distributed inference example."""

from __future__ import annotations

import hashlib
import time

from ndnsf import parse_large_data_reference_payload
from ndnsf_distributed_inference import APPProvider, ProviderRuntimeContext

from llm_pipeline_lib import (
    QWEN_ONNX_RUNTIME,
    QWEN_TRANSFORMERS_RUNTIME,
    SERVICE,
    TINY_TRANSFORMERS_RUNTIME,
    create_tiny_transformer_model,
    decode_qwen_pipeline_context,
    encode_final_response,
    encode_qwen_pipeline_context,
    encode_stage_payload,
    merge_qwen_pipeline_delta,
    parse_common_args,
    qwen_transformer_model_from_stage_package,
    qwen_transformer_stage_spec_from_execution,
    role_index,
    run_qwen_transformer_stage,
    run_qwen_onnx_stage,
    run_tiny_transformer_stage,
    tiny_transformer_model_from_execution,
    tiny_transformer_model_from_stage_package,
    tiny_transformer_stage_spec_from_execution,
)


_QWEN_CONTEXT_CACHE: dict[str, dict] = {}


def _selected_roles(raw_roles: str, provider: APPProvider) -> set[str]:
    if raw_roles.lower() == "all":
        return set(provider.roles_for_service(SERVICE))
    return {part.strip() for part in raw_roles.split(",") if part.strip()}


def _preload_tiny_stage_models(provider: APPProvider, roles: set[str],
                               fallback_layer_count: int) -> dict[str, object]:
    service_policy = provider.deployment.service_policy(SERVICE)
    cache: dict[str, object] = {}
    for artifact in service_policy.artifacts:
        if artifact.role not in roles:
            continue
        if artifact.kind != "llm-stage-weights":
            continue
        path = artifact.path
        if not path:
            continue
        cache[path] = tiny_transformer_model_from_stage_package(
            path,
            fallback_layer_count=fallback_layer_count,
        )
        print(
            "LLM_PIPELINE_TRANSFORMER_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
            f"path={path}",
            flush=True,
        )
    return cache


def _preload_qwen_stage_models(provider: APPProvider, roles: set[str]) -> dict[str, object]:
    service_policy = provider.deployment.service_policy(SERVICE)
    cache: dict[str, object] = {}
    for artifact in service_policy.artifacts:
        if artifact.role not in roles:
            continue
        if artifact.kind != "llm-stage-weights":
            continue
        if (artifact.metadata or {}).get("runtime") != QWEN_TRANSFORMERS_RUNTIME:
            continue
        path = artifact.path
        if not path:
            continue
        cache[path] = qwen_transformer_model_from_stage_package(path)
        print(
            "LLM_PIPELINE_QWEN_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
            f"path={path}",
            flush=True,
        )
    return cache


def _preload_qwen_onnx_sessions(provider: APPProvider, roles: set[str]) -> dict[str, object]:
    import onnxruntime as ort

    service_policy = provider.deployment.service_policy(SERVICE)
    cache: dict[str, object] = {}
    for artifact in service_policy.artifacts:
        if artifact.role not in roles:
            continue
        if artifact.kind != "onnx-model":
            continue
        if (artifact.metadata or {}).get("runtime") != QWEN_ONNX_RUNTIME:
            continue
        path = artifact.path
        if not path:
            continue
        cache[path] = ort.InferenceSession(
            path,
            providers=["CPUExecutionProvider"],
        )
        print(
            "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
            f"path={path}",
            flush=True,
        )
    return cache


def _qwen_onnx_metadata_by_path(provider: APPProvider) -> dict[str, dict]:
    service_policy = provider.deployment.service_policy(SERVICE)
    result: dict[str, dict] = {}
    for artifact in service_policy.artifacts:
        if artifact.path and (artifact.metadata or {}).get("runtime") == QWEN_ONNX_RUNTIME:
            result[artifact.path] = dict(artifact.metadata or {})
    return result


def _producer_for_single_input(ctx: ProviderRuntimeContext) -> str:
    edge = ctx.dependencies.input()
    if len(edge.producers) != 1:
        raise RuntimeError(
            f"LLM pipeline stage expects one producer, got {edge.producers}")
    return edge.producers[0]


def _planned_output_name(ctx: ProviderRuntimeContext) -> str:
    if not ctx.dependencies.outputs:
        return ""
    edge = ctx.dependencies.output()
    return ctx.planned_large_data_name(edge, ctx.role)


def _elapsed_ms(start: float) -> float:
    return (time.perf_counter() - start) * 1000.0


def _materialize_first_stage_request(ctx: ProviderRuntimeContext) -> tuple[bytes, float, bool]:
    reference = parse_large_data_reference_payload(ctx.request)
    if reference is None:
        return _resolve_qwen_context_request(ctx.request), 0.0, False
    fetch_start = time.perf_counter()
    payload = ctx.ndnsf.fetch_encrypted_large_data(reference.data_name, SERVICE)
    if payload is None:
        raise RuntimeError(f"failed to fetch Qwen context reference {reference.data_name}")
    if reference.digest.startswith("sha256:"):
        actual = hashlib.sha256(payload).hexdigest()
        expected = reference.digest[len("sha256:"):]
        if actual != expected:
            raise RuntimeError(
                f"Qwen context reference digest mismatch: expected {expected}, got {actual}")
    return _resolve_qwen_context_request(payload), _elapsed_ms(fetch_start), True


def _resolve_qwen_context_request(payload: bytes) -> bytes:
    try:
        doc = decode_qwen_pipeline_context(payload)
    except Exception:
        return payload
    mode = doc.get("contextMode", "full")
    session_id = str(doc.get("sessionId", ""))
    if mode == "append-delta":
        if not session_id:
            raise RuntimeError("append-delta Qwen context requires a sessionId")
        base = _QWEN_CONTEXT_CACHE.get(session_id)
        if base is None:
            raise RuntimeError(f"append-delta Qwen context has no cached base for {session_id}")
        doc = merge_qwen_pipeline_delta(base, doc)
    if doc.get("contextMode", "full") == "full" and session_id:
        _QWEN_CONTEXT_CACHE[session_id] = dict(doc)
    return encode_qwen_pipeline_context(
        doc["inputIds"],
        attention_mask=doc.get("attentionMask"),
        position_ids=doc.get("positionIds"),
        request_id=str(doc.get("requestId", "")),
        session_id=session_id,
        context_epoch=int(doc.get("contextEpoch", 0) or 0),
    )


def _print_qwen_stage_timing(**fields) -> None:
    normalized = {
        key: (f"{value:.2f}" if isinstance(value, float) else value)
        for key, value in fields.items()
    }
    print(
        "LLM_PIPELINE_QWEN_STAGE_TIMING",
        " ".join(f"{key}={value}" for key, value in normalized.items()),
        flush=True,
    )


def handle_stage(ctx: ProviderRuntimeContext, *, compute_delay_ms: float) -> None:
    handle_fake_stage(ctx, compute_delay_ms=compute_delay_ms)


def handle_fake_stage(ctx: ProviderRuntimeContext, *, compute_delay_ms: float) -> None:
    stage_index = role_index(ctx.role)
    is_first = not ctx.dependencies.inputs
    is_final = not ctx.dependencies.outputs

    input_future = None
    if not is_first:
        input_future = ctx.prefetch_input_large(
            producer_role=_producer_for_single_input(ctx),
            ref_timeout_ms=15000,
            fetch_timeout_ms=15000,
        )

    if is_first:
        input_payload = ctx.request
    else:
        input_payload = ctx.wait_prefetched_input_large(input_future, timeout_ms=20000)

    if is_final:
        response = encode_final_response(
            role=ctx.role,
            stage_index=stage_index,
            input_payload=input_payload,
            compute_delay_ms=compute_delay_ms,
        )
        ctx.ndnsf.publish_final_response(response)
        print(
            "LLM_PIPELINE_STAGE_FINAL",
            f"role={ctx.role}",
            f"bytes={len(response)}",
            flush=True,
        )
        return

    output = encode_stage_payload(
        role=ctx.role,
        stage_index=stage_index,
        input_payload=input_payload,
        compute_delay_ms=compute_delay_ms,
    )
    data_name = _planned_output_name(ctx)
    ctx.publish_output_large_reference(
        output,
        object_type="application/x-ndnsf-di-llm-hidden-state+json",
        object_id=f"stage-{stage_index}-hidden-state",
        data_name=data_name,
        max_segment_size=7000,
        freshness_ms=60000,
    )
    print(
        "LLM_PIPELINE_STAGE_OUTPUT",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


def handle_tiny_transformer_stage(ctx: ProviderRuntimeContext, *,
                                  stages: int,
                                  layer_count: int,
                                  model_cache,
                                  compute_delay_ms: float) -> None:
    stage_spec = tiny_transformer_stage_spec_from_execution(
        ctx.execution,
        fallback_role=ctx.role,
        fallback_stages=stages,
        fallback_layer_count=layer_count,
    )
    stages = int(stage_spec["stageCount"])
    layer_count = int(stage_spec["layerCount"])
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or f"seeded:{layer_count}")
    model = model_cache.get(model_key)
    if model is None:
        model = (
            tiny_transformer_model_from_execution(
                ctx.execution,
                fallback_layer_count=layer_count,
            ) or
            create_tiny_transformer_model(layer_count)
        )
        model_cache[model_key] = model
    is_first = not ctx.dependencies.inputs
    is_final = not ctx.dependencies.outputs

    input_future = None
    if not is_first:
        input_future = ctx.prefetch_input_large(
            producer_role=_producer_for_single_input(ctx),
            ref_timeout_ms=15000,
            fetch_timeout_ms=15000,
        )

    if is_first:
        input_payload = ctx.request
    else:
        input_payload = ctx.wait_prefetched_input_large(input_future, timeout_ms=20000)

    output = run_tiny_transformer_stage(
        input_payload,
        role=ctx.role,
        stages=stages,
        layer_count=layer_count,
        compute_delay_ms=compute_delay_ms,
        model=model,
    )
    if is_final:
        ctx.ndnsf.publish_final_response(output)
        print(
            "LLM_PIPELINE_TRANSFORMER_STAGE_FINAL",
            f"role={ctx.role}",
            f"bytes={len(output)}",
            flush=True,
        )
        return

    data_name = _planned_output_name(ctx)
    ctx.publish_output_large_reference(
        output,
        object_type="application/x-ndnsf-di-llm-transformer-hidden",
        object_id=f"{ctx.role.strip('/').replace('/', '-')}-hidden-state",
        data_name=data_name,
        max_segment_size=7000,
        freshness_ms=60000,
    )
    print(
        "LLM_PIPELINE_TRANSFORMER_STAGE_OUTPUT",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


def handle_qwen_transformer_stage(ctx: ProviderRuntimeContext, *,
                                  stages: int,
                                  model_cache,
                                  compute_delay_ms: float) -> None:
    total_start = time.perf_counter()
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    model = model_cache.get(model_key)
    if model is None:
        if not model_key:
            raise RuntimeError("Qwen stage execution requires a model artifact path")
        model = qwen_transformer_model_from_stage_package(model_key)
        model_cache[model_key] = model
    if hasattr(model, "ndnsf_stage_index"):
        stage_index = int(getattr(model, "ndnsf_stage_index"))
        stages = int(getattr(model, "ndnsf_stage_count"))
    else:
        stage_spec = qwen_transformer_stage_spec_from_execution(
            ctx.execution,
            fallback_role=ctx.role,
            fallback_stages=stages,
        )
        stage_index = int(stage_spec["stageIndex"])
        stages = int(stage_spec["stageCount"])
    is_first = not ctx.dependencies.inputs
    is_final = not ctx.dependencies.outputs

    input_future = None
    prefetch_submit_ms = 0.0
    if not is_first:
        prefetch_submit_start = time.perf_counter()
        input_future = ctx.prefetch_input_large(
            producer_role=_producer_for_single_input(ctx),
            ref_timeout_ms=30000,
            fetch_timeout_ms=30000,
        )
        prefetch_submit_ms = _elapsed_ms(prefetch_submit_start)

    if is_first:
        input_payload, input_reference_fetch_ms, used_input_reference = (
            _materialize_first_stage_request(ctx)
        )
        ref_wait_ms = 0.0
        fetch_ms = 0.0
        input_wait_ms = 0.0
        used_planned_name = False
        expected_segments = 0
        expected_bytes = 0
    else:
        input_wait_start = time.perf_counter()
        prefetch_result = ctx.wait_prefetched_input_large_result(
            input_future,
            timeout_ms=60000,
        )
        input_wait_ms = _elapsed_ms(input_wait_start)
        input_payload = prefetch_result.payload
        ref_wait_ms = prefetch_result.ref_wait_ms
        fetch_ms = prefetch_result.fetch_ms
        used_planned_name = prefetch_result.used_planned_name
        expected_segments = prefetch_result.expected_segments
        expected_bytes = prefetch_result.expected_bytes
        input_reference_fetch_ms = 0.0
        used_input_reference = False

    runner_timing: dict[str, float | int | str] = {}
    output = run_qwen_transformer_stage(
        input_payload,
        role=ctx.role,
        stages=stages,
        model=model,
        compute_delay_ms=compute_delay_ms,
        timing=runner_timing,
    )
    runner_compute_ms = (
        float(runner_timing.get("embed_ms", 0.0)) +
        float(runner_timing.get("mask_ms", 0.0)) +
        float(runner_timing.get("layers_ms", 0.0)) +
        float(runner_timing.get("final_head_ms", 0.0))
    )
    if is_final:
        publish_start = time.perf_counter()
        ctx.ndnsf.publish_final_response(output)
        publish_ms = _elapsed_ms(publish_start)
        _print_qwen_stage_timing(
            role=ctx.role,
            stage=stage_index,
            requestId=runner_timing.get("request_id", ""),
            isFinal=1,
            input_bytes=len(input_payload),
            output_bytes=len(output),
            prefetch_submit_ms=prefetch_submit_ms,
            input_wait_ms=input_wait_ms,
            input_reference_fetch_ms=input_reference_fetch_ms,
            used_input_reference=int(bool(used_input_reference)),
            ref_wait_ms=ref_wait_ms,
            fetch_ms=fetch_ms,
            used_planned_name=int(bool(used_planned_name)),
            expected_segments=expected_segments,
            expected_bytes=expected_bytes,
            decode_ms=float(runner_timing.get("decode_ms", 0.0)),
            serialize_ms=float(runner_timing.get("encode_ms", 0.0)),
            compute_ms=runner_compute_ms,
            artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
            runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
            publish_ms=publish_ms,
            total_ms=_elapsed_ms(total_start),
        )
        print(
            "LLM_PIPELINE_QWEN_STAGE_FINAL",
            f"role={ctx.role}",
            f"bytes={len(output)}",
            flush=True,
        )
        return

    data_name = _planned_output_name(ctx)
    publish_start = time.perf_counter()
    ctx.publish_output_large_reference(
        output,
        object_type="application/x-ndnsf-di-qwen-transformer-hidden",
        object_id=f"{ctx.role.strip('/').replace('/', '-')}-hidden-state",
        data_name=data_name,
        max_segment_size=7000,
        freshness_ms=60000,
    )
    publish_ms = _elapsed_ms(publish_start)
    _print_qwen_stage_timing(
        role=ctx.role,
        stage=stage_index,
        requestId=runner_timing.get("request_id", ""),
        isFinal=0,
        input_bytes=len(input_payload),
        output_bytes=len(output),
        prefetch_submit_ms=prefetch_submit_ms,
        input_wait_ms=input_wait_ms,
        input_reference_fetch_ms=input_reference_fetch_ms,
        used_input_reference=int(bool(used_input_reference)),
        ref_wait_ms=ref_wait_ms,
        fetch_ms=fetch_ms,
        used_planned_name=int(bool(used_planned_name)),
        expected_segments=expected_segments,
        expected_bytes=expected_bytes,
        decode_ms=float(runner_timing.get("decode_ms", 0.0)),
        serialize_ms=float(runner_timing.get("encode_ms", 0.0)),
        compute_ms=runner_compute_ms,
        artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
        runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
        publish_ms=publish_ms,
        total_ms=_elapsed_ms(total_start),
    )
    print(
        "LLM_PIPELINE_QWEN_STAGE_OUTPUT",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


def handle_qwen_onnx_stage(ctx: ProviderRuntimeContext, *,
                           stages: int,
                           session_cache,
                           metadata_cache,
                           compute_delay_ms: float) -> None:
    total_start = time.perf_counter()
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    session = session_cache.get(model_key)
    if session is None:
        if not model_key:
            raise RuntimeError("Qwen ONNX stage execution requires an ONNX artifact path")
        import onnxruntime as ort

        session = ort.InferenceSession(model_key, providers=["CPUExecutionProvider"])
        session_cache[model_key] = session
    metadata = metadata_cache.get(model_key, {})
    stage_index = int(metadata.get("stageIndex", role_index(ctx.role)))
    is_first = not ctx.dependencies.inputs
    is_final = not ctx.dependencies.outputs

    input_future = None
    prefetch_submit_ms = 0.0
    if not is_first:
        prefetch_submit_start = time.perf_counter()
        input_future = ctx.prefetch_input_large(
            producer_role=_producer_for_single_input(ctx),
            ref_timeout_ms=30000,
            fetch_timeout_ms=30000,
        )
        prefetch_submit_ms = _elapsed_ms(prefetch_submit_start)

    if is_first:
        input_payload, input_reference_fetch_ms, used_input_reference = (
            _materialize_first_stage_request(ctx)
        )
        ref_wait_ms = 0.0
        fetch_ms = 0.0
        input_wait_ms = 0.0
        used_planned_name = False
        expected_segments = 0
        expected_bytes = 0
    else:
        input_wait_start = time.perf_counter()
        prefetch_result = ctx.wait_prefetched_input_large_result(
            input_future,
            timeout_ms=60000,
        )
        input_wait_ms = _elapsed_ms(input_wait_start)
        input_payload = prefetch_result.payload
        ref_wait_ms = prefetch_result.ref_wait_ms
        fetch_ms = prefetch_result.fetch_ms
        used_planned_name = prefetch_result.used_planned_name
        expected_segments = prefetch_result.expected_segments
        expected_bytes = prefetch_result.expected_bytes
        input_reference_fetch_ms = 0.0
        used_input_reference = False

    runner_timing: dict[str, float | int | str] = {}
    output = run_qwen_onnx_stage(
        input_payload,
        role=ctx.role,
        stages=stages,
        session=session,
        metadata=metadata,
        compute_delay_ms=compute_delay_ms,
        timing=runner_timing,
    )
    runner_compute_ms = float(runner_timing.get("layers_ms", 0.0))
    if is_final:
        publish_start = time.perf_counter()
        ctx.ndnsf.publish_final_response(output)
        publish_ms = _elapsed_ms(publish_start)
        _print_qwen_stage_timing(
            role=ctx.role,
            stage=stage_index,
            requestId=runner_timing.get("request_id", ""),
            isFinal=1,
            input_bytes=len(input_payload),
            output_bytes=len(output),
            prefetch_submit_ms=prefetch_submit_ms,
            input_wait_ms=input_wait_ms,
            input_reference_fetch_ms=input_reference_fetch_ms,
            used_input_reference=int(bool(used_input_reference)),
            ref_wait_ms=ref_wait_ms,
            fetch_ms=fetch_ms,
            used_planned_name=int(bool(used_planned_name)),
            expected_segments=expected_segments,
            expected_bytes=expected_bytes,
            decode_ms=float(runner_timing.get("decode_ms", 0.0)),
            serialize_ms=float(runner_timing.get("encode_ms", 0.0)),
            compute_ms=runner_compute_ms,
            artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
            runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
            publish_ms=publish_ms,
            total_ms=_elapsed_ms(total_start),
        )
        print("LLM_PIPELINE_QWEN_ONNX_STAGE_FINAL", f"role={ctx.role}", f"bytes={len(output)}", flush=True)
        return

    if not output.startswith(b"NDITB001"):
        raise RuntimeError(
            "Qwen ONNX pilot intermediate output must use the typed tensor bundle")
    data_name = _planned_output_name(ctx)
    publish_start = time.perf_counter()
    ctx.publish_output_large_reference(
        output,
        object_type="application/x-ndnsf-di-tensor-bundle",
        object_id=f"{ctx.role.strip('/').replace('/', '-')}-hidden-state",
        data_name=data_name,
        max_segment_size=7000,
        freshness_ms=60000,
    )
    publish_ms = _elapsed_ms(publish_start)
    _print_qwen_stage_timing(
        role=ctx.role,
        stage=stage_index,
        requestId=runner_timing.get("request_id", ""),
        isFinal=0,
        input_bytes=len(input_payload),
        output_bytes=len(output),
        prefetch_submit_ms=prefetch_submit_ms,
        input_wait_ms=input_wait_ms,
        input_reference_fetch_ms=input_reference_fetch_ms,
        used_input_reference=int(bool(used_input_reference)),
        ref_wait_ms=ref_wait_ms,
        fetch_ms=fetch_ms,
        used_planned_name=int(bool(used_planned_name)),
        expected_segments=expected_segments,
        expected_bytes=expected_bytes,
        decode_ms=float(runner_timing.get("decode_ms", 0.0)),
        serialize_ms=float(runner_timing.get("encode_ms", 0.0)),
        compute_ms=runner_compute_ms,
        artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
        runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
        publish_ms=publish_ms,
        total_ms=_elapsed_ms(total_start),
    )
    print(
        "LLM_PIPELINE_QWEN_ONNX_STAGE_OUTPUT",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


def main() -> int:
    parser = parse_common_args("Run validation LLM pipeline provider")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--handler-workers", type=int, default=2)
    parser.add_argument("--compute-delay-ms", type=float, default=1.0)
    parser.add_argument(
        "--runtime",
        choices=("fake", TINY_TRANSFORMERS_RUNTIME, QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME),
        default="fake",
    )
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--transformer-layers", type=int, default=4)
    args = parser.parse_args()
    if args.dry_run:
        print("LLM_PIPELINE_PROVIDER_DRY_RUN", args.provider_id, args.roles)
        return 0

    provider = APPProvider.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        provider_id=args.provider_id,
        group=args.group,
        handler_workers=args.handler_workers,
    )
    if args.runtime == TINY_TRANSFORMERS_RUNTIME:
        tiny_models = _preload_tiny_stage_models(
            provider,
            _selected_roles(args.roles, provider),
            args.transformer_layers,
        )
        handler = lambda ctx: handle_tiny_transformer_stage(
            ctx,
            stages=args.stages,
            layer_count=args.transformer_layers,
            model_cache=tiny_models,
            compute_delay_ms=args.compute_delay_ms,
        )
        backends = ["transformers"]
    elif args.runtime == QWEN_TRANSFORMERS_RUNTIME:
        qwen_models = _preload_qwen_stage_models(
            provider,
            _selected_roles(args.roles, provider),
        )
        handler = lambda ctx: handle_qwen_transformer_stage(
            ctx,
            stages=args.stages,
            model_cache=qwen_models,
            compute_delay_ms=args.compute_delay_ms,
        )
        backends = ["transformers"]
    elif args.runtime == QWEN_ONNX_RUNTIME:
        selected_roles = _selected_roles(args.roles, provider)
        qwen_sessions = _preload_qwen_onnx_sessions(provider, selected_roles)
        qwen_metadata = _qwen_onnx_metadata_by_path(provider)
        handler = lambda ctx: handle_qwen_onnx_stage(
            ctx,
            stages=args.stages,
            session_cache=qwen_sessions,
            metadata_cache=qwen_metadata,
            compute_delay_ms=args.compute_delay_ms,
        )
        backends = ["onnxruntime"]
    else:
        handler = lambda ctx: handle_stage(ctx, compute_delay_ms=args.compute_delay_ms)
        backends = ["custom"]
    provider.serve_service(
        service=SERVICE,
        roles=args.roles,
        handler=handler,
        backends=backends,
        has_model=True,
        can_provision=False,
    )
    print(
        "LLM_PIPELINE_PROVIDER_READY",
        f"provider_id={args.provider_id or '(root)'}",
        f"roles={args.roles}",
        f"runtime={args.runtime}",
        flush=True,
    )
    return provider.run()


if __name__ == "__main__":
    raise SystemExit(main())
