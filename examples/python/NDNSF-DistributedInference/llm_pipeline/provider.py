#!/usr/bin/env python3
"""Provider for the validation LLM pipeline distributed inference example."""

from __future__ import annotations

import base64
import builtins
import copy
import hashlib
import hmac
import json
import os
import re
import time
from pathlib import Path
from threading import Lock, RLock, Thread
from types import SimpleNamespace
from typing import Any, Mapping

from ndnsf import parse_large_data_reference_payload
from ndnsf_distributed_inference.app_sdk.provider import (
    APPProvider, ProviderEvidenceSigner,
)
from ndnsf_distributed_inference.artifact_deployment import (
    ProviderResidencyIdentity,
    ProviderResidencyLedger,
    RuntimePreparationEvidence,
)
from ndnsf_distributed_inference.core.contracts import (
    DIRequestEnvelopeV2,
    DISelectionAssignmentV2,
    GenerationRecoveryV1,
)
from ndnsf_distributed_inference.conversation import (
    ConversationContinuation,
    ConversationCheckpointInvalid,
    ConversationStateReferenceV1,
    ConversationStateUnavailable,
    ConversationTurnBindingV1,
    ProviderConversationStateManager,
    ProviderConversationStateReceiptV1,
)
from ndnsf_distributed_inference.core.deployment_control import (
    DISelectionParticipant,
    GpuMiBAdmissionLedger,
    SelectionPreparationCallbacks,
)
from ndnsf_distributed_inference.provider import (
    DIProviderOfferIssuer, DIProviderOfferIssuerV3, ProviderRuntimeContext,
)
from ndnsf_distributed_inference.adapters.qwen.generation import (
    accepted_prefix_digest,
)
from ndnsf_distributed_inference.sdk.placement import (
    DeviceResourceSnapshot, ResidencyProofV3, ResidencyTierV3,
    canonical_digest,
)
from ndnsf_distributed_inference.sdk.adapters import RunnerAdapterRegistry

from deployment_control import (
    EvidenceRunnerAdapter, ProviderDeploymentControl,
)

from llm_pipeline_lib import (
    QWEN_ONNX_RUNTIME,
    QWEN_TRANSFORMERS_RUNTIME,
    SERVICE,
    TINY_ONNX_RUNTIME,
    TINY_TRANSFORMERS_RUNTIME,
    create_tiny_transformer_model,
    decode_payload,
    decode_qwen_pipeline_context,
    encode_final_response,
    encode_qwen_pipeline_context,
    encode_stage_payload,
    merge_qwen_pipeline_delta,
    parse_common_args,
    probe_qwen_transformers_model_type,
    qwen_transformer_model_from_stage_package,
    qwen_transformer_stage_spec_from_execution,
    role_index,
    run_qwen_transformer_stage,
    run_qwen_onnx_stage,
    run_tiny_onnx_stage,
    run_tiny_transformer_stage,
    tiny_transformer_model_from_execution,
    tiny_transformer_model_from_stage_package,
    tiny_transformer_stage_spec_from_execution,
    warm_qwen_transformer_stage,
)


_PIPELINE_MARKER_LOCK = Lock()
_QWEN_CONTEXT_CACHE_LOCK = RLock()


def _configure_spec175_stream_fault(provider: APPProvider, case_id: str) -> None:
    """Install one registered real-SVS fault for the host MiniNDN gate."""
    case_id = str(case_id or "").upper()
    if case_id not in {"M02", "M03", "M04", "M05", "M09"}:
        return
    lock = Lock()
    held_wire: list[bytes] = []
    injected = {"value": False}

    def mark(action: str, cursor: int) -> None:
        _emit(
            "NDNSF_DI_SPEC175_FAULT_INJECTED",
            f"case={case_id}", f"action={action}", f"cursor={cursor}",
            flush=True,
        )

    def publication(_name: str, cursor: int, wire: bytes) -> bool:
        if case_id == "M02":
            if cursor == 3:
                with lock:
                    if not held_wire:
                        held_wire.append(bytes(wire))
                        mark("hold-publication", cursor)
                        return False
            if cursor == 4:
                with lock:
                    delayed = held_wire.pop() if held_wire else b""
                if delayed:
                    provider.publish_stream_packet_for_test(delayed)
                    mark("release-after-cursor-4", cursor)
            return True
        if case_id == "M03" and cursor == 4:
            with lock:
                if not injected["value"]:
                    injected["value"] = True
                    duplicate_wire = bytes(wire)
                    mark("duplicate-publication", cursor)
                    # The interceptor runs on the Provider Face event loop.
                    # Re-entering publish_stream_packet_for_test synchronously
                    # corrupts the SVS publication transaction and can hide
                    # even cursor 1 from the consumer.  Replay after the
                    # current commit returns, preserving duplicate Data
                    # semantics without Face/SVS reentrancy.
                    def republish_after_commit() -> None:
                        time.sleep(0.01)
                        provider.publish_stream_packet_for_test(duplicate_wire)

                    Thread(target=republish_after_commit, daemon=True).start()
            return True
        if case_id == "M09" and cursor == 3:
            with lock:
                if not injected["value"]:
                    injected["value"] = True
                    mark("provider-exit-after-publication", cursor)

                    def exit_after_publication() -> None:
                        time.sleep(0.1)
                        os._exit(86)

                    Thread(target=exit_after_publication, daemon=True).start()
            return True
        return True

    def retention(_name: str, cursor: int, _wire: bytes) -> bool:
        if case_id == "M04" and cursor == 5:
            with lock:
                if not injected["value"]:
                    injected["value"] = True
                    delayed = bytes(_wire)
                    mark("drop-once-before-retention", cursor)

                    def republish_after_one_interest_timeout() -> None:
                        time.sleep(0.35)
                        provider.publish_stream_packet_for_test(delayed)
                        mark("bounded-republish", cursor)

                    Thread(
                        target=republish_after_one_interest_timeout,
                        daemon=True,
                    ).start()
                    return False
        if case_id == "M05" and cursor == 5:
            with lock:
                if not injected["value"]:
                    injected["value"] = True
                    mark("suppress-retention", cursor)
                    return False
        return True

    provider.set_stream_publication_interceptor_for_test(publication)
    if case_id in {"M04", "M05"}:
        provider.set_stream_retention_interceptor_for_test(retention)


def _emit(*args, **kwargs) -> None:
    """Emit runtime output and an atomic machine-readable marker copy.

    Provider handlers run concurrently and share stdout with NDNSF Core's
    logger.  Even flushed ``print`` calls can therefore be interleaved between
    ``key=value`` fields, which makes post-run lifecycle analysis ambiguous.
    When the harness supplies ``NDNSF_PIPELINE_MARKER_LOG``, retain the same
    marker line in a provider-local append-only file under the rank scratch
    directory.  Runtime behavior is unchanged if the variable is absent.
    """
    separator = str(kwargs.get("sep", " "))
    end = str(kwargs.get("end", "\n"))
    line = separator.join(str(value) for value in args)
    with _PIPELINE_MARKER_LOCK:
        builtins.print(*args, **kwargs)
        marker_path = os.environ.get("NDNSF_PIPELINE_MARKER_LOG", "").strip()
        if not marker_path or not (
                line.startswith("LLM_PIPELINE_")
                or line.startswith("NDNSF_DI_")):
            return
        try:
            with open(marker_path, "a", encoding="utf-8") as stream:
                stream.write(line + end)
                stream.flush()
        except OSError:
            # Marker capture is diagnostic-only and must never change the
            # provider's request/response behavior.
            return


_QWEN_CONTEXT_CACHE: dict[str, dict] = {}
_QWEN_CPU_CONTROL_OP_ALLOWLIST = frozenset({
    "Add",
    "Concat",
    "ConstantOfShape",
    "Div",
    "Equal",
    "Expand",
    "Gather",
    "Identity",
    "Mul",
    "Range",
    "Reshape",
    "Shape",
    "Split",
    "Squeeze",
    "Unsqueeze",
    "Where",
})
_QWEN_CPU_CONTROL_DTYPES = frozenset({"bool", "int64"})
_QWEN_CPU_CONTROL_MAX_ELEMENTS = 8
_QWEN_REQUIRED_CUDA_CORE_GROUPS = {
    "matmul": frozenset({"FusedMatMul", "MatMul"}),
    "normalization": frozenset({
        "ReduceMean", "SimplifiedLayerNormalization",
    }),
    "softmax": frozenset({"Softmax"}),
}
_QWEN_REQUIRED_CUDA_CORE_OPS = frozenset().union(
    *_QWEN_REQUIRED_CUDA_CORE_GROUPS.values())


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
        _emit(
            "LLM_PIPELINE_TRANSFORMER_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
            f"path={path}",
            flush=True,
        )
    return cache


def _preload_qwen_stage_models(
    provider: APPProvider,
    roles: set[str], *,
    device: str = "cpu",
    require_cuda: bool = False,
    local_artifacts: dict[str, dict] | None = None,
) -> dict[str, object]:
    service_policy = provider.deployment.service_policy(SERVICE)
    cache: dict[str, object] = {}
    configured = dict(local_artifacts or {})
    entries = []
    for role, item in configured.items():
        if role in roles:
            entries.append((role, str(item.get("path", "")), dict(item)))
    for artifact in service_policy.artifacts:
        if artifact.role not in roles or artifact.role in configured:
            continue
        if artifact.kind != "llm-stage-weights":
            continue
        if (artifact.metadata or {}).get("runtime") != QWEN_TRANSFORMERS_RUNTIME:
            continue
        entries.append((artifact.role, artifact.path, {
            "kind": artifact.kind,
            "metadata": dict(artifact.metadata or {}),
        }))
    for role, path, item in entries:
        if not path:
            continue
        cache[path] = qwen_transformer_model_from_stage_package(
            path,
            device=device,
            require_cuda=require_cuda,
        )
        execution_device = str(
            getattr(cache[path], "ndnsf_execution_device", "unknown"))
        cpu_fallback = bool(
            getattr(cache[path], "ndnsf_cpu_fallback", True))
        _emit(
            "LLM_PIPELINE_QWEN_STAGE_ARTIFACT_READY",
            f"role={role}",
            f"path={path}",
            f"device={execution_device}",
            f"cpuFallback={str(cpu_fallback).lower()}",
            flush=True,
        )
    return cache


def _qwen_onnx_session(path: str, *, device: str,
                       require_cuda: bool,
                       profile_prefix: str | None = None):
    import onnxruntime as ort

    available = set(ort.get_available_providers())
    normalized = device.strip().lower()
    wants_cuda = normalized == "auto" or normalized.startswith("cuda")
    has_cuda = "CUDAExecutionProvider" in available
    if require_cuda and not wants_cuda:
        raise RuntimeError(
            "--require-cuda cannot be combined with a non-CUDA ONNX device")
    if wants_cuda and has_cuda:
        device_id = 0
        if ":" in normalized:
            device_id = int(normalized.split(":", 1)[1])
        options = ort.SessionOptions()
        if profile_prefix:
            options.enable_profiling = True
            options.profile_file_prefix = profile_prefix
        # The exported Qwen graph is already canonicalized.  Disable ORT's
        # CPU-side graph rewrite: on Tiger, rewriting this large external-data
        # graph can take minutes before CUDA sessions even load.  Qwen's
        # dynamic KV-cache graph contains small int64/bool shape-control
        # subgraphs which CUDA EP intentionally partitions to CPU.  CPU EP is
        # therefore present only as the shape/control execution partner; the
        # runtime/profile gate rejects CPU model-compute nodes separately.
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        # The fixed-context Qwen3.6 ONNX contract still has large intermediate
        # tensors.  Keep the memory-pattern planner disabled because the
        # request shape can vary, but allow ORT to reuse buffers whose
        # lifetimes do not overlap.  Disabling reuse made the third 27B stage
        # exhaust a 32-GiB RTX 5000 while allocating a 60-MiB transpose buffer.
        options.enable_mem_pattern = False
        options.enable_mem_reuse = True
        providers = [(
            "CUDAExecutionProvider",
            {"device_id": device_id},
        )]
        # CUDA EP does not implement every shape/control kernel.  Keep CPU EP
        # after CUDA so those bounded control tensors can be executed without
        # moving model compute off the GPU.  `_qwen_onnx_profile_summary`
        # classifies the actual node providers after warmup.
        providers.append("CPUExecutionProvider")
        return ort.InferenceSession(
            path,
            sess_options=options,
            providers=providers,
        )
    if require_cuda:
        raise RuntimeError(
            "Qwen ONNX CUDA is required but CUDAExecutionProvider is unavailable")
    return ort.InferenceSession(
        path,
        providers=["CPUExecutionProvider"],
    )


def _qwen_onnx_session_placement(session) -> tuple[str, bool]:
    providers = tuple(session.get_providers())
    if providers and providers[0] == "CUDAExecutionProvider":
        return "cuda:0", False
    return "cpu", True


class _QwenOnnxRuntimeHandle:
    """Assignment-bound ONNX Runtime object used by Selection preparation.

    The request-first Selection code historically stored a Transformers model
    here and inspected its execution-device attributes.  Keep that contract
    for the residency ledger, but make the deployed ONNX path explicit: the
    actual session remains the object used by the request handler, while this
    small handle records the verified CUDA/CPU placement and metadata.
    """

    def __init__(self, session, metadata: dict | None = None) -> None:
        self.session = session
        self.metadata = dict(metadata or {})
        self.ndnsf_execution_device, self.ndnsf_cpu_fallback = (
            _qwen_onnx_session_placement(session))


def _qwen_onnx_session_from_cache(
    session_cache: Mapping[str, object], model_key: str,
) -> object | None:
    """Return the raw ORT session from either eager or lazy cache entries."""
    value = session_cache.get(model_key)
    if isinstance(value, _QwenOnnxRuntimeHandle):
        return value.session
    return value


def _onnx_numpy_dtype(type_name: str, default):
    import numpy as np

    normalized = str(type_name or "").lower()
    if "bfloat16" in normalized:
        raise RuntimeError(
            "QWEN_ONNX_BFLOAT16_UNSUPPORTED: export FP16 or provide an "
            "explicit packed-bfloat16 binding")
    if "float16" in normalized:
        return np.float16
    if "float" in normalized:
        return np.float32
    if "int64" in normalized:
        return np.int64
    if "int32" in normalized:
        return np.int32
    if "bool" in normalized:
        return np.bool_
    return default


def _warm_qwen_onnx_runtime(handle: _QwenOnnxRuntimeHandle) -> bool:
    """Run one bounded real ORT invocation on the assigned execution provider."""
    import numpy as np

    session = handle.session
    model_type = str(handle.metadata.get("modelType", "qwen2"))
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"
    hidden_size = int(handle.metadata.get("hiddenSize", 0) or 0)
    if hidden_size <= 0:
        for item in session.get_inputs():
            if item.name == "hidden_states" and len(item.shape) >= 3:
                if isinstance(item.shape[2], int):
                    hidden_size = int(item.shape[2])
                    break
    hidden_size = hidden_size or 896
    feed = {}
    for item in session.get_inputs():
        name = str(item.name)
        if name in ("input_ids", "attention_mask", "position_ids"):
            dtype = np.int64
            shape = ((4, 1, 1) if model_type == "qwen3_5"
                     and name == "position_ids" else (1, 1))
            if name == "attention_mask":
                shape = (1, 1)
            feed[name] = np.zeros(shape, dtype=dtype)
            if name == "attention_mask":
                feed[name].fill(1)
        elif name == "hidden_states":
            feed[name] = np.zeros(
                (1, 1, hidden_size),
                dtype=_onnx_numpy_dtype(getattr(item, "type", ""), np.float32),
            )
        elif name.startswith(("past_key.", "past_value.")):
            shape = item.shape
            if len(shape) != 4:
                raise RuntimeError(
                    f"unsupported Qwen ONNX KV input shape for {name}: {shape}")
            dims = [1]
            for index, dimension in enumerate(shape[1:], start=1):
                if index == 2:
                    dims.append(0)
                elif isinstance(dimension, int) and dimension > 0:
                    dims.append(int(dimension))
                else:
                    dims.append(1)
            feed[name] = np.empty(
                tuple(dims),
                dtype=_onnx_numpy_dtype(
                    getattr(item, "type", ""), np.float32),
            )
        else:
            raise RuntimeError(f"unrecognized Qwen ONNX input: {name}")
    session.run(None, feed)
    return True


def _qwen_onnx_profile_summary(session) -> dict:
    profile_path = Path(session.end_profiling())
    events = json.loads(profile_path.read_text(encoding="utf-8"))
    if not isinstance(events, list):
        raise RuntimeError("Qwen ONNX profile must be one JSON event array")

    counts: dict[str, dict[str, int]] = {}
    cpu_node_names: dict[str, set[str]] = {}
    cpu_tensor_types: dict[str, set[str]] = {}
    cpu_non_control_tensor_nodes: set[str] = set()
    cpu_missing_type_shape_nodes: set[str] = set()

    def inspect_cpu_tensors(event_name: str, op_name: str,
                            args: dict) -> None:
        observed = []
        missing = False
        invalid = False
        for field in ("input_type_shape", "output_type_shape"):
            entries = args.get(field)
            if not isinstance(entries, list) or not entries:
                missing = True
                continue
            for entry in entries:
                if not isinstance(entry, dict) or len(entry) != 1:
                    invalid = True
                    continue
                dtype, shape = next(iter(entry.items()))
                observed.append(f"{dtype}:{shape}")
                if dtype not in _QWEN_CPU_CONTROL_DTYPES:
                    invalid = True
                    continue
                if not isinstance(shape, list) or any(
                        not isinstance(dim, int) or dim < 0 for dim in shape):
                    invalid = True
                    continue
                elements = 1
                for dim in shape:
                    elements *= dim
                if elements > _QWEN_CPU_CONTROL_MAX_ELEMENTS:
                    invalid = True
        cpu_tensor_types.setdefault(op_name, set()).update(observed)
        if missing:
            cpu_missing_type_shape_nodes.add(event_name)
        if invalid:
            cpu_non_control_tensor_nodes.add(event_name)

    for event in events:
        if not isinstance(event, dict) or event.get("cat") != "Node":
            continue
        args = event.get("args")
        if not isinstance(args, dict):
            continue
        provider = str(args.get("provider") or "")
        op_name = str(args.get("op_name") or "")
        if not provider:
            continue
        if not op_name:
            op_name = "<missing-op-name>"
        provider_counts = counts.setdefault(provider, {})
        provider_counts[op_name] = provider_counts.get(op_name, 0) + 1
        if provider == "CPUExecutionProvider":
            event_name = str(event.get("name") or "<missing-node-name>")
            cpu_node_names.setdefault(op_name, set()).add(event_name)
            inspect_cpu_tensors(event_name, op_name, args)

    cuda_ops = set(counts.get("CUDAExecutionProvider", {}))
    cpu_ops = set(counts.get("CPUExecutionProvider", {}))
    unknown_cpu_ops = sorted(cpu_ops - _QWEN_CPU_CONTROL_OP_ALLOWLIST)
    cpu_core_ops = sorted(cpu_ops & _QWEN_REQUIRED_CUDA_CORE_OPS)
    missing_cpu_node_names = any(
        "<missing-node-name>" in names
        for names in cpu_node_names.values()
    )
    missing_cuda_core_groups = sorted(
        group_name
        for group_name, aliases in _QWEN_REQUIRED_CUDA_CORE_GROUPS.items()
        if not (cuda_ops & aliases)
    )
    node_event_count = sum(
        sum(provider_counts.values())
        for provider_counts in counts.values()
    )
    violations = []
    if node_event_count == 0:
        violations.append("EMPTY_NODE_PROFILE")
    if unknown_cpu_ops:
        violations.append("UNKNOWN_CPU_OPS:" + ",".join(unknown_cpu_ops))
    if missing_cpu_node_names:
        violations.append("MISSING_CPU_NODE_NAME")
    if cpu_core_ops:
        violations.append("CPU_CORE_OPS:" + ",".join(cpu_core_ops))
    if cpu_missing_type_shape_nodes:
        violations.append(
            "MISSING_CPU_TYPE_SHAPE:" +
            ",".join(sorted(cpu_missing_type_shape_nodes)))
    if cpu_non_control_tensor_nodes:
        violations.append(
            "CPU_NON_CONTROL_TENSORS:" +
            ",".join(sorted(cpu_non_control_tensor_nodes)))
    if missing_cuda_core_groups:
        violations.append(
            "MISSING_CUDA_CORE_GROUPS:" +
            ",".join(missing_cuda_core_groups))

    return {
        "schemaVersion": "ndnsf-qwen-onnx-ep-profile-v2",
        "state": "PASS" if not violations else "FAIL",
        "profilePath": str(profile_path),
        "nodeEventCount": node_event_count,
        "providerOpCounts": counts,
        "cpuControlOps": sorted(
            cpu_ops & _QWEN_CPU_CONTROL_OP_ALLOWLIST),
        "cpuNodeNamesByOp": {
            op_name: sorted(names)
            for op_name, names in sorted(cpu_node_names.items())
        },
        "cpuTensorTypesByOp": {
            op_name: sorted(types)
            for op_name, types in sorted(cpu_tensor_types.items())
        },
        "cpuMissingTypeShapeNodes": sorted(cpu_missing_type_shape_nodes),
        "cpuNonControlTensorNodes": sorted(cpu_non_control_tensor_nodes),
        "unknownCpuOps": unknown_cpu_ops,
        "cpuCoreOps": cpu_core_ops,
        "missingCudaCoreGroups": missing_cuda_core_groups,
        "violations": violations,
    }


def _preload_qwen_onnx_sessions(provider: APPProvider, roles: set[str], *,
                                 device: str = "cpu",
                                 require_cuda: bool = False,
                                 local_artifacts: dict[str, dict] | None = None
                                 ) -> dict[str, object]:
    service_policy = provider.deployment.service_policy(SERVICE)
    cache: dict[str, object] = {}
    configured = dict(local_artifacts or {})
    entries: list[tuple[str, str, dict]] = []
    for role, item in configured.items():
        if role in roles:
            entries.append((role, str(item.get("path", "")), dict(item)))
    for artifact in service_policy.artifacts:
        if artifact.role not in roles or artifact.role in configured:
            continue
        if artifact.kind != "onnx-model":
            continue
        if (artifact.metadata or {}).get("runtime") != QWEN_ONNX_RUNTIME:
            continue
        entries.append((artifact.role, artifact.path, {
            "kind": artifact.kind,
            "metadata": dict(artifact.metadata or {}),
        }))
    profile_root = os.environ.get("NDNSF_QWEN_ORT_PROFILE_DIR", "").strip()
    if profile_root:
        Path(profile_root).mkdir(parents=True, exist_ok=True)
    for role, path, item in entries:
        if not path:
            continue
        profile_prefix = None
        if profile_root:
            safe_role = re.sub(r"[^A-Za-z0-9_.-]+", "_", role).strip("_")
            profile_prefix = str(
                Path(profile_root) / f"qwen-{safe_role or 'stage'}")
        cache[path] = _qwen_onnx_session(
            path,
            device=device,
            require_cuda=require_cuda,
            profile_prefix=profile_prefix,
        )
        execution_device, cpu_fallback = _qwen_onnx_session_placement(
            cache[path])
        _emit(
            "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY",
            f"role={role}",
            f"path={path}",
            f"device={execution_device}",
            f"cpuFallback={str(cpu_fallback).lower()}",
            flush=True,
        )
    return cache


def _qwen_onnx_sessions_for_startup(
    provider: APPProvider,
    roles: set[str],
    *,
    device: str,
    require_cuda: bool,
    local_artifacts: dict[str, dict] | None,
    lazy_qwen_load: bool,
) -> dict[str, object]:
    """Construct ONNX sessions only when eager loading was requested.

    ``--lazy-qwen-load`` is a real startup boundary: constructing a CUDA ORT
    session after APPProvider has created its NDN workers can be unsafe on
    some deployments. The selected-role preparation callback creates and
    warms the session later, after ACK/Selection, using the same cache object.
    """
    if lazy_qwen_load:
        return {}
    return _preload_qwen_onnx_sessions(
        provider,
        roles,
        device=device,
        require_cuda=require_cuda,
        local_artifacts=local_artifacts,
    )


def _qwen_onnx_can_prepare(
    args,
    *,
    runtime_cache: Mapping[str, object],
    local_artifacts: Mapping[str, Mapping[str, object]] | None,
) -> bool:
    """Return whether Selection can truthfully prepare an ONNX role."""
    if runtime_cache:
        return True
    if any(str(item.get("path", ""))
           for item in (local_artifacts or {}).values()):
        return True
    return bool(str(getattr(args, "selection_repo_registration", "") or ""))


def _finalize_qwen_onnx_profiles(
    sessions: dict[str, object], metadata_by_path: dict[str, dict],
) -> None:
    """Close and validate optional ORT profiles after a real warmup run."""
    profile_root = os.environ.get("NDNSF_QWEN_ORT_PROFILE_DIR", "").strip()
    if not profile_root:
        return
    root = Path(profile_root)
    root.mkdir(parents=True, exist_ok=True)
    for path, session in sessions.items():
        profile = _qwen_onnx_profile_summary(session)
        stage_index = metadata_by_path.get(path, {}).get("stageIndex", "unknown")
        profile_output = root / f"qwen-stage-{stage_index}-summary.json"
        profile_output.write_text(
            json.dumps(profile, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        _emit(
            "LLM_PIPELINE_QWEN_ONNX_PROFILE",
            f"stage={stage_index}",
            f"state={profile.get('state')}",
            f"profile={profile.get('profilePath', '')}",
            f"summary={profile_output}",
            flush=True,
        )
        if profile.get("state") != "PASS":
            raise RuntimeError(
                "QWEN_ONNX_EXECUTION_PROFILE_FAILED:"
                + ",".join(str(v) for v in profile.get("violations", ())))


def _qwen_onnx_metadata_by_path(
    provider: APPProvider,
    local_artifacts: dict[str, dict] | None = None,
) -> dict[str, dict]:
    service_policy = provider.deployment.service_policy(SERVICE)
    result: dict[str, dict] = {}
    for item in (local_artifacts or {}).values():
        path = str(item.get("path", ""))
        metadata = dict(item.get("metadata") or {})
        if path and metadata.get("runtime") == QWEN_ONNX_RUNTIME:
            result[path] = metadata
    for artifact in service_policy.artifacts:
        if artifact.path and (artifact.metadata or {}).get("runtime") == QWEN_ONNX_RUNTIME:
            result[artifact.path] = dict(artifact.metadata or {})
    return result


def _artifact_path_by_role(provider: APPProvider) -> dict[str, str]:
    return {
        artifact.role: artifact.path
        for artifact in provider.deployment.service_policy(SERVICE).artifacts
        if artifact.path
    }


def _parse_selection_local_artifacts(
    values: list[str], *, runtime: str = QWEN_TRANSFORMERS_RUNTIME,
) -> dict[str, dict]:
    """Parse explicit preloaded stage paths for the V3 exact-reuse gate.

    The deployment policy normally supplies artifact paths.  The real local
    gate intentionally starts from a content-addressed stage bundle instead,
    so the runner passes one immutable ``role=path`` binding per Provider.
    This is an opt-in local-artifact input; it is never inferred from a
    request or from a request-scoped directory.
    """
    result: dict[str, dict] = {}
    for raw in values:
        role, separator, path_value = str(raw).partition("=")
        role = role.strip()
        path_value = path_value.strip()
        if not separator or not role or not path_value:
            raise SystemExit(
                "--selection-local-artifact requires ROLE=PATH")
        path = Path(path_value).expanduser().resolve()
        if not path.is_file():
            raise SystemExit(
                f"--selection-local-artifact path does not exist: {path}")
        result[role] = {
            "path": str(path),
            "kind": (
                "onnx-model" if runtime == QWEN_ONNX_RUNTIME
                else "llm-stage-weights"
            ),
            "metadata": {"runtime": runtime},
            "filename": path.name,
        }
    return result


def _read_secret(path: str, *, expected_bytes: int = 32) -> bytes:
    value = Path(path).read_bytes()
    if len(value) != expected_bytes:
        raise RuntimeError(
            f"selection secret {path} must contain {expected_bytes} raw bytes")
    return value


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(16 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _qwen_residency_identity(
    residency_template: dict,
    *,
    artifact_digest: str,
    adapter_id: str,
    adapter_version: str,
    backend: str,
    device: str,
    provider_boot_epoch: str,
) -> ProviderResidencyIdentity:
    """Bind a reusable shard to the model graph and exact partition."""

    template_artifact = str(residency_template.get("artifact_digest", ""))
    if template_artifact and template_artifact != artifact_digest:
        raise RuntimeError("residency template artifact does not match assignment")
    for key, expected in (
            ("adapter_id", adapter_id),
            ("adapter_version", adapter_version)):
        configured = str(residency_template.get(key, ""))
        if configured and configured != expected:
            raise RuntimeError(
                f"residency template {key} does not match assignment")
    return ProviderResidencyIdentity(
        model_content_digest=str(
            residency_template.get("model_content_digest", "")),
        graph_digest=str(residency_template.get("graph_digest", "")),
        partition_digest=str(
            residency_template.get("partition_digest", "")),
        artifact_digest=artifact_digest,
        adapter_id=adapter_id,
        adapter_version=adapter_version,
        backend=backend,
        device=device,
        provider_boot_epoch=provider_boot_epoch,
    )


def _validate_selection_timing_window(
    *, offer_lease_ms: int, max_prepare_ms: int,
) -> tuple[int, int]:
    """Reject a request-first timing window that can only expire mid-plan.

    The Qwen V2 path publishes or resolves model artifacts after ACK_CLOSED and
    before the final Selection.  There is no offer-renewal round in that
    attempt, so an offer lease shorter than the admitted preparation window
    makes a valid cold request deterministically unselectable.
    """

    offer_lease_ms = int(offer_lease_ms)
    max_prepare_ms = int(max_prepare_ms)
    if offer_lease_ms <= 0 or max_prepare_ms <= 0:
        raise RuntimeError(
            "Selection timing windows must be positive")
    if offer_lease_ms < max_prepare_ms:
        raise RuntimeError(
            "request-first Selection offer lease must cover the maximum "
            "preparation window")
    return offer_lease_ms, max_prepare_ms


def _preflight_qwen_runtime(args) -> None:
    selection_dataflow_v2 = bool(getattr(args, "selection_dataflow_v2", False))
    selection_dataflow_v3 = bool(getattr(args, "selection_dataflow_v3", False))
    if (getattr(args, "require_onnx_runtime", False)
            and args.runtime != QWEN_ONNX_RUNTIME):
        raise RuntimeError(
            "deployed Qwen providers must use qwen-onnx; "
            "qwen-transformers is offline/legacy-only"
        )
    if (args.runtime != QWEN_TRANSFORMERS_RUNTIME
            or not (selection_dataflow_v2 or selection_dataflow_v3)):
        return
    model_type = str(args.selection_model_type or "")
    if not model_type:
        raise RuntimeError(
            "request-first Qwen Provider requires --selection-model-type")
    probe_qwen_transformers_model_type(model_type)


def _selection_v2_for_qwen(
    provider: APPProvider,
    args,
    *,
    model_cache: dict[str, object],
    model_cache_lock: Lock,
    local_artifacts: dict[str, dict] | None = None,
    runtime_metadata: dict[str, dict] | None = None,
) -> dict:
    runtime = str(getattr(args, "runtime", QWEN_TRANSFORMERS_RUNTIME))
    if runtime not in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
        raise RuntimeError(f"unsupported Qwen Selection runtime: {runtime}")
    onnx_runtime = runtime == QWEN_ONNX_RUNTIME
    artifact_suffix = ".qwen-onnx.onnx" if onnx_runtime else ".qwen-transformers.pt"
    runtime_metadata = dict(runtime_metadata or {})
    selection_dataflow_v2 = bool(getattr(args, "selection_dataflow_v2", False))
    selection_dataflow_v3 = bool(getattr(args, "selection_dataflow_v3", False))
    if not (selection_dataflow_v2 or selection_dataflow_v3):
        return {}
    required = (
        args.provider_identity,
        args.selection_wal_path,
        args.selection_storage_key_file,
        args.selection_signing_key_file,
        args.selection_residency_json,
        args.selection_model_cache_dir,
    )
    if (not all(required) or args.selection_gpu_capacity_mib <= 0
            or args.selection_offered_gpu_mib <= 0):
        raise RuntimeError("Selection Dataflow V2 configuration is incomplete")
    offer_lease_ms, max_prepare_ms = _validate_selection_timing_window(
        offer_lease_ms=args.selection_offer_lease_ms,
        max_prepare_ms=args.selection_max_prepare_ms,
    )
    assigned_device = (
        "cuda:0" if args.device == "auto" and args.require_cuda
        else "cpu" if args.device == "auto"
        else str(args.device)
    )
    if args.require_cuda:
        if (not assigned_device.startswith("cuda:")
                or not assigned_device[5:].isdigit()):
            raise RuntimeError(
                "CUDA Selection Dataflow V2 requires one explicit device")
    elif assigned_device != "cpu":
        raise RuntimeError(
            "CPU_LOGIC Selection Dataflow V2 requires --device cpu")
    storage_key = _read_secret(args.selection_storage_key_file)
    signing_key = _read_secret(args.selection_signing_key_file)
    residency_template = json.loads(
        Path(args.selection_residency_json).read_text(encoding="utf-8"))
    if not isinstance(residency_template, dict):
        raise RuntimeError("selection residency record must be one JSON object")
    try:
        core_boot_epoch = provider.provider_boot_epoch
    except AttributeError:
        # Compatibility with an already sealed runtime built before the
        # public APPProvider property was added. The underlying NDNSF Python
        # provider has exposed Core's epoch throughout Selection Dataflow V2.
        core_boot_epoch = (
            provider._network_provider._provider.provider.provider_boot_epoch
        )
    if not core_boot_epoch:
        raise RuntimeError("NDNSF Core provider boot epoch is unavailable")
    configured_boot_epoch = str(getattr(args, "provider_boot_epoch", "") or "")
    if configured_boot_epoch and configured_boot_epoch != core_boot_epoch:
        raise RuntimeError(
            "configured provider boot epoch does not match NDNSF Core epoch; "
            "Selection must use provider.provider_boot_epoch")
    # Validate the complete durable residency identity before advertising the
    # Provider as ready.  Otherwise a malformed deployment record first fails
    # inside the asynchronous ACK callback and looks like network packet loss.
    _qwen_residency_identity(
        residency_template,
        artifact_digest=str(residency_template.get("artifact_digest", "")),
        adapter_id=str(residency_template.get("adapter_id", "")),
        adapter_version=str(residency_template.get("adapter_version", "")),
        backend=str(residency_template.get("backend", "")),
        device=assigned_device,
        provider_boot_epoch=core_boot_epoch,
    )
    print(
        "LLM_PIPELINE_SELECTION_RESIDENCY_TEMPLATE_READY "
        f"role={residency_template.get('role', '')} ",
        flush=True,
    )
    artifact_paths = _artifact_path_by_role(provider)
    for role, item in dict(local_artifacts or {}).items():
        path = str(item.get("path", ""))
        if path:
            artifact_paths[str(role)] = path
    repo_registration_path = str(
        getattr(args, "selection_repo_registration", "") or "")
    # Registration is deliberately late-bound.  ACK/offer publication must
    # not depend on a model split having already been published: the deferred
    # planner publishes the selected split only after ACK_CLOSED.  Providers
    # therefore advertise their current residency first and reload the exact
    # content-bound registration when Selection preparation begins.
    repo_registration_by_role: dict[str, dict] = {}
    repo_receipts: tuple[dict, ...] = ()
    repo_registration_lock = Lock()

    def refresh_repo_registration() -> dict[str, dict]:
        nonlocal repo_registration_by_role, repo_receipts
        if not repo_registration_path:
            return dict(repo_registration_by_role)
        path = Path(repo_registration_path)
        if not path.is_file():
            return dict(repo_registration_by_role)
        registration = json.loads(path.read_text(encoding="utf-8"))
        if registration.get("schemaVersion") != (
                "ndnsf-di-qwen36-repo-registration-v1"):
            raise RuntimeError("unsupported Qwen DistributedRepo registration")
        artifacts = list(registration.get("artifacts", []))
        by_role = {
            str(item["role"]): dict(item)
            for item in artifacts
        }
        if not by_role:
            raise RuntimeError("Qwen DistributedRepo registration has no artifacts")
        receipts = tuple(
            dict(receipt)
            for item in artifacts
            for receipt in item.get("receipts", [])
        )
        if not getattr(args, "selection_model_cache_dir", ""):
            raise RuntimeError(
                "Qwen DistributedRepo fetch requires --selection-model-cache-dir")
        # Publish one immutable snapshot under a lock.  clear()+update() exposed
        # a transient empty map to concurrent Selection retries.
        with repo_registration_lock:
            repo_registration_by_role = by_role
            repo_receipts = receipts
            return dict(repo_registration_by_role)

    def await_repo_registration(context, role: str) -> dict | None:
        if not repo_registration_path:
            return None
        interval_s = 0.05
        last_progress_ms = 0
        while True:
            try:
                item = refresh_repo_registration().get(role)
            except (FileNotFoundError, json.JSONDecodeError):
                item = None
            if item is not None:
                return item
            now_ms = int(time.time() * 1000)
            remaining_ms = int(context.deadline_ms) - now_ms
            if remaining_ms <= 0:
                raise TimeoutError(
                    f"Qwen DistributedRepo registration deadline expired for {role}")
            if now_ms - last_progress_ms >= 1000:
                _emit(
                    "LLM_PIPELINE_QWEN_REPO_REGISTRATION_WAIT",
                    f"requestId={context.request_id}",
                    f"role={role}",
                    f"remainingMs={remaining_ms}",
                    flush=True,
                )
                last_progress_ms = now_ms
            time.sleep(min(interval_s, remaining_ms / 1000.0))
            interval_s = min(interval_s * 2.0, 1.0)

    # Preserve compatibility with the pre-split path while allowing the file
    # to be absent during the initial ACK phase.
    refresh_repo_registration()
    repo_holder: dict[str, object] = {}
    verified_disk_paths: dict[str, tuple[str, tuple[int, int, int, int]]] = {}
    prepared_runtime_by_request_role_digest: dict[
        tuple[str, str, str], dict,
    ] = {}
    residency_identity_by_request_role: dict[
        tuple[str, str], ProviderResidencyIdentity,
    ] = {}
    residency_owner_by_request_role: dict[tuple[str, str], str] = {}
    residency_ledger = ProviderResidencyLedger(
        args.selection_model_cache_dir,
        provider_boot_epoch=core_boot_epoch,
    )

    # Seed the same content-addressed ledger used by deferred preparation when
    # the operator intentionally starts with a locally loaded stage.  The
    # source stage is never copied into a request directory; a hard link is
    # sufficient to create the durable cache identity.
    process_epoch = "process-" + str(os.getpid())
    topology_digest = canonical_digest({
        "provider": args.provider_identity,
        "devices": (assigned_device,),
    })
    artifact_paths = _artifact_path_by_role(provider)
    for role, item in dict(local_artifacts or {}).items():
        path = str(item.get("path", ""))
        if path:
            artifact_paths[str(role)] = path
    for role, source_name in artifact_paths.items():
        source = Path(source_name)
        model = model_cache.get(str(source))
        if model is None or not source.is_file():
            continue
        artifact_digest = str(residency_template.get("artifact_digest", ""))
        if not artifact_digest.startswith("sha256:"):
            artifact_digest = "sha256:" + artifact_digest
        if _sha256_file(source) != artifact_digest:
            continue
        identity = _qwen_residency_identity(
            residency_template,
            artifact_digest=artifact_digest,
            adapter_id=str(residency_template.get("adapter_id", "")),
            adapter_version=str(residency_template.get("adapter_version", "")),
            backend=str(residency_template.get("backend", "")),
            device=assigned_device,
            provider_boot_epoch=core_boot_epoch,
        )
        destination = residency_ledger.content_path(
            identity, artifact_digest[7:] + artifact_suffix)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            try:
                os.link(source, destination)
            except FileExistsError:
                pass
            except OSError:
                continue
        try:
            residency_ledger.admit_disk(
                identity, destination, size=source.stat().st_size)
            if assigned_device.startswith("cuda:"):
                residency_ledger.promote_gpu(
                    identity, model, bytes_loaded=source.stat().st_size,
                    load_completed=True, warmup_completed=True,
                    cpu_fallback_count=0)
            else:
                residency_ledger.promote_ram(
                    identity, model, bytes_loaded=source.stat().st_size)
        except (OSError, RuntimeError, ValueError):
            continue

    def remember_verified_file(path: Path, digest: str) -> None:
        stat = path.stat()
        verified_disk_paths[str(path)] = (
            digest,
            (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns),
        )

    def is_verified_file(path: Path, digest: str) -> bool:
        record = verified_disk_paths.get(str(path))
        if record is None or record[0] != digest or not path.is_file():
            return False
        stat = path.stat()
        return record[1] == (
            stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
    admission_ledger = GpuMiBAdmissionLedger(
        provider=args.provider_identity,
        boot_epoch=core_boot_epoch,
        capacity_mib=args.selection_gpu_capacity_mib,
    )
    issuer = DIProviderOfferIssuer(
        provider=args.provider_identity,
        service=SERVICE,
        boot_epoch=core_boot_epoch,
        ledger=admission_ledger,
        offered_gpu_memory_mb=args.selection_offered_gpu_mib,
        signer_key_id="sha256:" + hashlib.sha256(signing_key).hexdigest(),
        sign_offer_digest=lambda digest: hmac.new(
            signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
        devices=(assigned_device,),
        offer_lease_ms=offer_lease_ms,
        max_pending_state_ttl_ms=max_prepare_ms,
    )
    issue_offer = issuer.issue

    def issue_offer_with_ttl_diagnostics(*issue_args, **issue_kwargs):
        decision = issue_offer(*issue_args, **issue_kwargs)
        _emit(
            "LLM_PIPELINE_SELECTION_ACK_TTL",
            f"pendingStateTtlMs={decision.pending_state_ttl_ms}",
            f"providerLimitMs={issuer.max_pending_state_ttl_ms}",
            flush=True,
        )
        return decision

    issuer.issue = issue_offer_with_ttl_diagnostics

    def prepare_role(context) -> None:
        role = context.role.role
        assigned_digest = str(context.role.artifact_digest)
        residency_identity = _qwen_residency_identity(
            residency_template,
            artifact_digest=assigned_digest,
            adapter_id=str(context.role.adapter_id),
            adapter_version=str(context.role.adapter_version),
            backend=str(context.role.backend),
            device=str(context.role.device),
            provider_boot_epoch=core_boot_epoch,
        )
        registration_item = await_repo_registration(context, role)
        assignment_key = (
            str(registration_item["objectName"])
            if registration_item is not None else ""
        )
        fetch_ms = 0.0
        disk_cache_hit = False
        fetch_result = None
        with model_cache_lock:
            Path(args.selection_model_cache_dir).mkdir(
                parents=True, exist_ok=True)
            model_path = artifact_paths.get(role, "")
            if registration_item is not None:
                digest = str(registration_item["fileSha256"])
                if not digest.startswith("sha256:"):
                    digest = "sha256:" + digest
                if digest != assigned_digest:
                    raise RuntimeError(
                        "selected Qwen artifact does not match Repo registration")
                digest_hex = digest[7:] if digest.startswith("sha256:") else digest
                model_path = str(residency_ledger.content_path(
                    residency_identity,
                    f"{digest_hex}{artifact_suffix}",
                ))
                destination = Path(model_path)
                resident = residency_ledger.lookup(residency_identity)
                disk_cache_hit = (
                    resident is not None and resident.path == destination)
                if not disk_cache_hit and destination.is_file():
                    disk_cache_hit = (
                        destination.stat().st_size
                        == int(registration_item["fileBytes"])
                        and _sha256_file(destination) == digest_hex
                    )
                    if disk_cache_hit:
                        remember_verified_file(destination, digest)
                        residency_ledger.admit_disk(
                            residency_identity,
                            destination,
                            size=int(registration_item["fileBytes"]),
                        )
                if not disk_cache_hit:
                    if destination.exists():
                        raise RuntimeError(
                            "Qwen repo cache path exists with invalid content")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    if "artifact_api" not in repo_holder:
                        from py_repoclient import (
                            ArtifactRepositoryApi,
                            CollaborationArtifactApiBackend,
                            artifact_reference_from_dict,
                        )
                        backend = CollaborationArtifactApiBackend.from_config(
                            args.config,
                            generated_policy_dir=(
                                str(args.generated_policy_dir) + "-repo"),
                            state_root=args.repo_client_state_root,
                            user=args.provider_identity,
                            committed_receipts=repo_receipts,
                        )
                        repo_holder["artifact_api"] = ArtifactRepositoryApi(
                            backend,
                            publisher_identity=args.provider_identity,
                            default_timeout_ms=max(
                                int(args.selection_max_prepare_ms), 600000),
                        )
                        repo_holder["reference_from_dict"] = (
                            artifact_reference_from_dict
                        )
                    fetch_start = time.perf_counter()
                    reference = repo_holder["reference_from_dict"](
                        dict(registration_item["artifactReference"]))
                    def emit_repo_fetch_progress(progress) -> None:
                        # The Artifact API dispatches progress asynchronously;
                        # this callback only writes bounded, structured
                        # evidence and never controls the fetch lifecycle.
                        _emit(
                            "LLM_PIPELINE_QWEN_REPO_FETCH_PROGRESS",
                            f"requestId={context.request_id}",
                            f"role={role}",
                            f"logicalName={reference.logical_name}",
                            f"sequence={progress.sequence}",
                            f"receivedBytes={progress.received_bytes}",
                            f"verifiedBytes={progress.verified_bytes}",
                            f"totalBytes={progress.total_bytes}",
                            f"lastSegment={progress.last_segment}",
                            f"deliveredSegments={progress.delivered_segments}",
                            f"totalSegments={progress.total_segments}",
                            f"retransmittedBytes={progress.retransmitted_bytes}",
                            f"elapsedMs={progress.elapsed_ms:.2f}",
                            flush=True,
                        )
                    try:
                        fetch_result = repo_holder["artifact_api"].fetch_file(
                            reference,
                            destination,
                            on_progress=emit_repo_fetch_progress,
                            timeout_ms=max(
                                int(args.selection_max_prepare_ms), 600000),
                        )
                    except Exception as exc:  # noqa: BLE001
                        # Keep the public ArtifactApiError intentionally
                        # bounded, but expose the local exception class and
                        # message in the provider evidence.  Without this,
                        # route/fetch failures collapse into the opaque
                        # ``INTERNAL_ERROR: artifact backend failed`` and
                        # cannot be distinguished from a Repo integrity fault.
                        _emit(
                            "LLM_PIPELINE_QWEN_REPO_FETCH_FAILED",
                            f"requestId={context.request_id}",
                            f"role={role}",
                            f"logicalName={reference.logical_name}",
                            f"errorType={type(exc).__name__}",
                            f"error={str(exc)}",
                            flush=True,
                        )
                        raise
                    fetch_ms = _elapsed_ms(fetch_start)
                    if (destination.stat().st_size
                            != int(registration_item["fileBytes"])
                            or _sha256_file(destination) != digest_hex):
                        raise RuntimeError(
                            "Qwen Repo fetch completed with invalid content")
                    remember_verified_file(destination, digest)
                    residency_ledger.admit_disk(
                        residency_identity,
                        destination,
                        size=int(registration_item["fileBytes"]),
                        unique_bytes=int(registration_item["fileBytes"]),
                        wire_bytes=(
                            int(getattr(fetch_result, "transferred_bytes", 0))
                            + int(getattr(
                                fetch_result, "retransmitted_bytes", 0))
                        ),
                    )
                _emit(
                    "LLM_PIPELINE_QWEN_REPO_FETCH",
                    f"requestId={context.request_id}",
                    f"role={role}",
                    f"objectName={registration_item['objectName']}",
                    f"cacheHit={str(disk_cache_hit).lower()}",
                    f"fetch_ms={fetch_ms:.2f}",
                    f"bytes={registration_item['fileBytes']}",
                    f"lastSegment={getattr(fetch_result, 'last_segment', -1)}",
                    f"deliveredSegments={getattr(fetch_result, 'delivered_segments', 0)}",
                    f"totalSegments={getattr(fetch_result, 'total_segments', 0)}",
                    f"retransmittedBytes={getattr(fetch_result, 'retransmitted_bytes', 0)}",
                    flush=True,
                )
                _emit(
                    "LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE",
                    f"requestId={context.request_id}",
                    f"role={role}",
                    f"objectName={registration_item['objectName']}",
                    f"bytes={registration_item['fileBytes']}",
                    f"lastSegment={getattr(fetch_result, 'last_segment', -1)}",
                    f"deliveredSegments={getattr(fetch_result, 'delivered_segments', 0)}",
                    f"totalSegments={getattr(fetch_result, 'total_segments', 0)}",
                    f"retransmittedBytes={getattr(fetch_result, 'retransmitted_bytes', 0)}",
                    f"fetchMs={fetch_ms:.2f}",
                    flush=True,
                )
            if not model_path:
                raise RuntimeError(
                    f"selected Qwen role has no resolved artifact: {role}")
            if registration_item is None:
                source_path = Path(model_path)
                if not source_path.is_file():
                    raise RuntimeError(
                        f"selected Qwen artifact is unavailable: {model_path}")
                destination = residency_ledger.content_path(
                    residency_identity,
                    f"{assigned_digest[7:]}{artifact_suffix}",
                )
                resident = residency_ledger.lookup(residency_identity)
                disk_cache_hit = resident is not None
                if resident is None:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    if not destination.exists():
                        try:
                            os.link(source_path, destination)
                        except OSError as exc:
                            raise RuntimeError(
                                "pre-split Qwen artifact cannot enter the "
                                "content-addressed cache without copying; "
                                "publish it through DistributedRepo"
                            ) from exc
                    residency_ledger.admit_disk(
                        residency_identity,
                        destination,
                        size=source_path.stat().st_size,
                    )
                model_path = str(destination)
            model_file = Path(model_path)
            if not model_file.is_file():
                raise RuntimeError(
                    f"selected Qwen artifact is unavailable: {model_path}")
            if not is_verified_file(model_file, assigned_digest):
                observed_digest = "sha256:" + _sha256_file(model_file)
                if observed_digest != assigned_digest:
                    raise RuntimeError(
                        "selected Qwen artifact hash does not match assignment")
                remember_verified_file(model_file, assigned_digest)
            resident = residency_ledger.lookup(residency_identity)
            expected_residency_tier = (
                "GPU" if assigned_device.startswith("cuda:") else "RAM")
            cache_hit = (
                resident is not None
                and resident.tier == expected_residency_tier)
            load_start = time.perf_counter()
            if cache_hit:
                model = resident.resource
            else:
                if onnx_runtime:
                    session = _qwen_onnx_session(
                        model_path,
                        device=context.role.device,
                        require_cuda=args.require_cuda,
                    )
                    model = _QwenOnnxRuntimeHandle(
                        session,
                        runtime_metadata.get(model_path, {}),
                    )
                    if _warm_qwen_onnx_runtime(model) is not True:
                        raise RuntimeError(
                            "Qwen ONNX runtime warmup did not complete")
                    execution_device, cpu_fallback = (
                        _qwen_onnx_session_placement(session))
                    _emit(
                        "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY",
                        f"role={role}",
                        f"path={model_path}",
                        f"device={execution_device}",
                        f"cpuFallback={str(cpu_fallback).lower()}",
                        flush=True,
                    )
                else:
                    model = qwen_transformer_model_from_stage_package(
                        model_path,
                        device=context.role.device,
                        require_cuda=args.require_cuda,
                    )
            model_cache[model_path] = model
            if assignment_key:
                model_cache[assignment_key] = model
            # Selection preparation resolves the immutable Repo cache path,
            # while the later collaboration execution may expose a distinct
            # per-assignment temporary artifact path.  Keep the role as a
            # stable in-process alias so execution reuses the CUDA-resident
            # model instead of loading a second copy and exhausting the GPU.
            model_cache[role] = model
            prepared_runtime_by_request_role_digest[
                (context.request_id, role, assigned_digest)
            ] = {
                "model": model,
                "path": model_path,
                "assignment": context.role,
                "residency_identity": residency_identity,
                "runtime_cache_hit": cache_hit,
                "expected_residency_tier": expected_residency_tier,
                "file_identity": (
                    model_file.stat().st_dev,
                    model_file.stat().st_ino,
                    model_file.stat().st_size,
                    model_file.stat().st_mtime_ns,
                ),
            }
        _emit(
            "LLM_PIPELINE_QWEN_SELECTION_PREPARE",
            f"requestId={context.request_id}",
            f"role={role}",
            f"cacheHit={str(cache_hit).lower()}",
            f"diskCacheHit={str(disk_cache_hit).lower()}",
            f"fetch_ms={fetch_ms:.2f}",
            f"load_ms={_elapsed_ms(load_start):.2f}",
            f"device={getattr(model, 'ndnsf_execution_device', 'unknown')}",
            f"cpuFallback={str(bool(getattr(model, 'ndnsf_cpu_fallback', True))).lower()}",
            flush=True,
        )
        _emit(
            "LLM_PIPELINE_QWEN_RESIDENCY_SNAPSHOT",
            f"requestId={context.request_id}",
            f"role={role}",
            f"snapshot={json.dumps(residency_ledger.snapshot(), sort_keys=True, separators=(',', ':'))}",
            flush=True,
        )

    def prepare_role_with_diagnostics(context) -> None:
        try:
            prepare_role(context)
        except Exception as exc:  # noqa: BLE001
            _emit(
                "LLM_PIPELINE_QWEN_SELECTION_PREPARE_FAILED",
                f"requestId={context.request_id}",
                f"role={context.role.role}",
                f"errorType={type(exc).__name__}",
                f"error={exc}",
                flush=True,
            )
            raise

    def release_residency_owner(context, reason: str) -> None:
        key = (context.request_id, context.role.role)
        with model_cache_lock:
            identity = residency_identity_by_request_role.pop(key, None)
            owner = residency_owner_by_request_role.pop(key, "")
            prepared_runtime_by_request_role_digest.pop(
                (context.request_id, context.role.role,
                 context.role.artifact_digest),
                None,
            )
            if identity is not None and owner:
                residency_ledger.release(identity, owner=owner)
        _emit(
            "LLM_PIPELINE_QWEN_RESIDENCY_RELEASE",
            f"requestId={context.request_id}",
            f"role={context.role.role}",
            f"reason={reason}",
            f"snapshot={json.dumps(residency_ledger.snapshot(), sort_keys=True, separators=(',', ':'))}",
            flush=True,
        )

    participant = DISelectionParticipant(
        provider=args.provider_identity,
        boot_epoch=core_boot_epoch,
        ledger=admission_ledger,
        offer_lookup=issuer.lookup,
        callbacks=SelectionPreparationCallbacks(
            prepare_role=prepare_role_with_diagnostics,
            start_role=lambda role: _emit(
                "LLM_PIPELINE_QWEN_SELECTION_ROLE_START",
                f"role={role}", flush=True),
            release_role=lambda role, reason: _emit(
                "LLM_PIPELINE_QWEN_SELECTION_ROLE_RELEASE",
                f"role={role}", f"reason={reason}", flush=True),
            release_assignment=release_residency_owner,
        ),
        clock_ms=lambda: int(time.time() * 1000),
    )
    sealed_prepare = participant.prepare

    def prepare_with_field_diagnostics(context, payload):
        """Preserve sealed Core semantics while naming only mismatched fields."""
        try:
            return sealed_prepare(context, payload)
        except ValueError as exc:
            error_text = str(exc)
            if not (
                error_text.startswith(
                    "DI Selection/Core context binding mismatch")
                or error_text == "DI Selection is not exactly ACK-offer bound"
            ):
                raise
            # Keep this diagnostic-only path independent of package import
            # caching in a sealed runtime.  The normal participant path has
            # already validated the payload; this import is only used to
            # decode it when producing field-level diagnostics.
            from ndnsf_distributed_inference.core.contracts import (
                DISelectionAssignmentV2 as _DISelectionAssignmentV2,
            )
            assignment = _DISelectionAssignmentV2.from_bytes(bytes(payload))
            offer = issuer.lookup(assignment.offer_digest)
            mismatches = []
            if context.get("request_id") != assignment.request_id:
                mismatches.append("request_id")
            if int(context.get("attempt", 0)) != assignment.attempt:
                mismatches.append("attempt")
            if offer is None:
                mismatches.append("offer_digest")
            elif context.get("service_name") != offer.service:
                mismatches.append("service_name")
            if context.get("provider_identity") != participant.provider:
                mismatches.append("provider_identity")
            if context.get("provider_boot_epoch") != participant.boot_epoch:
                mismatches.append("provider_boot_epoch")
            if (context.get("selection_payload_digest")
                    != participant._digest_bytes(payload)):
                mismatches.append("selection_payload_digest")
            if assignment.provider != participant.provider:
                mismatches.append("assignment_provider")
            if assignment.provider_boot_epoch != participant.boot_epoch:
                mismatches.append("assignment_provider_boot_epoch")
            now_ms = int(time.time() * 1000)
            if assignment.deadline_ms <= now_ms:
                mismatches.append("assignment_deadline")
            core_expiry_ms = int(context.get("expires_at_unix_ms", 0))
            if core_expiry_ms < assignment.deadline_ms:
                mismatches.append(
                    "core_expiry"
                    f"(delta_ms={core_expiry_ms - assignment.deadline_ms})")
            if error_text == "DI Selection is not exactly ACK-offer bound":
                now_ms = int(time.time() * 1000)
                if not participant._offer_verifier(offer):
                    mismatches.append("offer_signature")
                if offer.digest() != assignment.offer_digest:
                    mismatches.append("offer_digest")
                if offer.request_id != assignment.request_id:
                    mismatches.append("offer_request_id")
                if offer.attempt != assignment.attempt:
                    mismatches.append("offer_attempt")
                if offer.provider != assignment.provider:
                    mismatches.append("offer_provider")
                if offer.boot_epoch != assignment.provider_boot_epoch:
                    mismatches.append("offer_boot_epoch")
                if offer.resource_sequence != assignment.resource_sequence:
                    mismatches.append("offer_resource_sequence")
                if offer.expires_at_ms <= now_ms:
                    mismatches.append(
                        "offer_expiry"
                        f"(delta_ms={offer.expires_at_ms - now_ms})")
                if offer.accepted_deadline_ms < assignment.deadline_ms:
                    mismatches.append(
                        "offer_accepted_deadline"
                        "(delta_ms="
                        f"{offer.accepted_deadline_ms - assignment.deadline_ms})")
                rejected_roles = tuple(
                    role.role for role in assignment.roles
                    if role.role not in offer.accepted_roles
                )
                if rejected_roles:
                    mismatches.append(
                        "offer_roles(" + ",".join(rejected_roles) + ")")
                rejected_backends = tuple(
                    role.backend for role in assignment.roles
                    if role.backend not in offer.backends
                )
                if rejected_backends:
                    mismatches.append(
                        "offer_backends(" + ",".join(rejected_backends) + ")")
                rejected_devices = tuple(
                    role.device for role in assignment.roles
                    if role.device not in offer.devices
                )
                if rejected_devices:
                    mismatches.append(
                        "offer_devices(" + ",".join(rejected_devices) + ")")
            suffix = ",".join(mismatches) if mismatches else "unclassified"
            _emit(
                "LLM_PIPELINE_QWEN_SELECTION_VALIDATION_FAILED",
                f"requestId={assignment.request_id}",
                f"roleCount={len(assignment.roles)}",
                f"error={error_text}",
                f"fields={suffix}",
                flush=True,
            )
            raise ValueError(
                (
                    "DI Selection/Core context binding mismatch: "
                    if error_text.startswith(
                        "DI Selection/Core context binding mismatch")
                    else "DI Selection/ACK-offer binding mismatch: "
                ) + suffix
            ) from exc

    participant.prepare = prepare_with_field_diagnostics

    def runtime_preparer(execution, progress):
        metadata = dict(execution.spec.metadata or {})
        role = str(execution.spec.role)
        request_id = str(metadata.get("selectionRequestId", ""))
        artifact_digest = str(metadata.get("selectionArtifactDigest", ""))
        key = (request_id, role, artifact_digest)
        # V2 prepares the role in DISelectionParticipant's WAL callback.  V3
        # deliberately has no reservation/participant, so the Core Selection
        # callback reaches this adapter with an authenticated projection but
        # without a participant-created preparation record.  Materialize the
        # same assignment-bound preparation record here, immediately before
        # runtime activation.  The context is deliberately reconstructed only
        # from Selection-bound metadata; no provider-local role or artifact is
        # selected from an unsigned request field.
        with model_cache_lock:
            prepared = prepared_runtime_by_request_role_digest.get(key)
        if prepared is None:
            adapter_id = str(metadata.get("selectionAdapterId", ""))
            adapter_version = str(
                metadata.get("selectionAdapterVersion", ""))
            device = str(metadata.get("selectionDevice", ""))
            if not (request_id and role and artifact_digest and adapter_id
                    and adapter_version and device):
                raise RuntimeError(
                    "Qwen V3 runtime preparation metadata is incomplete")
            role_context = SimpleNamespace(
                role=role,
                artifact_digest=artifact_digest,
                adapter_id=adapter_id,
                adapter_version=adapter_version,
                backend=str(execution.spec.backend),
                device=device,
            )
            now_ms = int(time.time() * 1000)
            preparation_context = SimpleNamespace(
                transaction_id=(
                    f"v3:{request_id}:{int(metadata.get('selectionAttempt', 1))}:"
                    f"{args.provider_identity}:{role}"),
                invocation_id=request_id,
                request_id=request_id,
                attempt=max(1, int(metadata.get("selectionAttempt", 1))),
                plan_digest=str(metadata.get("planDigest", "")),
                provider=args.provider_identity,
                provider_boot_epoch=core_boot_epoch,
                deadline_ms=now_ms + max(1, int(args.selection_max_prepare_ms)),
                generation=1,
                role=role_context,
            )
            _emit(
                "LLM_PIPELINE_QWEN_V3_ASSIGNMENT_PREPARE_START",
                f"requestId={request_id}",
                f"role={role}",
                f"artifactDigest={artifact_digest}",
                flush=True,
            )
            prepare_role_with_diagnostics(preparation_context)
            with model_cache_lock:
                prepared = prepared_runtime_by_request_role_digest.get(key)
            if prepared is None:
                raise RuntimeError(
                    "Qwen V3 assignment preparation produced no runtime")
        with model_cache_lock:
            prepared = prepared_runtime_by_request_role_digest.get(key)
            if prepared is None:
                raise RuntimeError(
                    "Qwen runtime preparation lacks an assignment-bound model")
            assignment = prepared["assignment"]
            model = prepared["model"]
            residency_identity = prepared["residency_identity"]
            path = Path(prepared["path"])
            if not path.is_file():
                raise RuntimeError(
                    "Qwen runtime preparation lost verified artifact identity")
            stat = path.stat()
            if ((stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
                    != tuple(prepared["file_identity"])):
                raise RuntimeError(
                    "Qwen runtime artifact changed after hash verification")
            progress("LOADING", 0.75)
            observed_device = str(
                getattr(model, "ndnsf_execution_device", ""))
            cpu_fallback = _unexpected_cpu_fallback_count(
                model, assigned_device)
            if observed_device != assignment.device or cpu_fallback:
                raise RuntimeError(
                    "Qwen loaded runtime does not match assigned device")
            progress("WARMING", 0.90)
            if prepared["runtime_cache_hit"]:
                hit = residency_ledger.lookup(residency_identity)
                if (hit is None
                        or hit.tier != prepared["expected_residency_tier"]
                        or hit.resource is not model):
                    raise RuntimeError(
                        "Qwen runtime residency changed before execution")
            else:
                warmed = (
                    _warm_qwen_onnx_runtime(model)
                    if onnx_runtime
                    else warm_qwen_transformer_stage(model, assigned_device)
                )
                if warmed is not True:
                    raise RuntimeError("Qwen stage warmup did not complete")
                if assigned_device.startswith("cuda:"):
                    residency_ledger.promote_gpu(
                        residency_identity,
                        model,
                        bytes_loaded=path.stat().st_size,
                        load_completed=True,
                        warmup_completed=True,
                        cpu_fallback_count=cpu_fallback,
                    )
                else:
                    residency_ledger.promote_ram(
                        residency_identity,
                        model,
                        bytes_loaded=path.stat().st_size,
                    )
            owner = (
                f"{request_id}:{int(metadata.get('selectionAttempt', 0))}:"
                f"{role}"
            )
            residency_ledger.acquire(residency_identity, owner=owner)
            residency_identity_by_request_role[(request_id, role)] = (
                residency_identity)
            residency_owner_by_request_role[(request_id, role)] = owner
            evidence = RuntimePreparationEvidence(
                adapter_id=assignment.adapter_id,
                adapter_version=assignment.adapter_version,
                backend=assignment.backend,
                device=observed_device,
                artifact_digests=(artifact_digest,),
                load_completed=True,
                warmup_completed=True,
                cpu_fallback_count=cpu_fallback,
                prepared_at_ms=int(time.time() * 1000),
                device_class=(
                    "CUDA" if assigned_device.startswith("cuda:")
                    else "CPU_LOGIC"),
            )
            _emit(
                "LLM_PIPELINE_QWEN_RUNTIME_READY",
                f"requestId={request_id}",
                f"role={role}",
                f"device={observed_device}",
                f"artifactDigest={artifact_digest}",
                "loadCompleted=true",
                "warmupCompleted=true",
                f"cpuFallbackCount={cpu_fallback}",
                f"deviceClass={evidence.device_class}",
                flush=True,
            )
            return evidence

    def cached_shards():
        refresh_repo_registration()
        role = str(residency_template.get("role", ""))
        artifact_digest = str(
            residency_template.get("artifact_digest", ""))
        registration_item = repo_registration_by_role.get(role)
        if registration_item is not None:
            artifact_digest = str(registration_item["fileSha256"])
            if not artifact_digest.startswith("sha256:"):
                artifact_digest = "sha256:" + artifact_digest
        identity = _qwen_residency_identity(
            residency_template,
            artifact_digest=artifact_digest,
            adapter_id=str(residency_template.get("adapter_id", "")),
            adapter_version=str(
                residency_template.get("adapter_version", "")),
            backend=str(residency_template.get("backend", "")),
            device=assigned_device,
            provider_boot_epoch=core_boot_epoch,
        )
        resident = residency_ledger.lookup(identity)
        if resident is None and registration_item is not None:
            digest_hex = artifact_digest[7:]
            model_path = residency_ledger.content_path(
                identity, f"{digest_hex}{artifact_suffix}")
            if model_path.is_file():
                residency_ledger.admit_disk(
                    identity,
                    model_path,
                    size=int(registration_item["fileBytes"]),
                )
                resident = residency_ledger.lookup(identity)
        if resident is None:
            return ()
        now_ms = int(time.time() * 1000)
        value = dict(residency_template)
        value.update({
            "boot_epoch": core_boot_epoch,
            "cache_epoch": 1,
            "captured_at_ms": now_ms,
            "expires_at_ms": now_ms + args.selection_residency_ttl_ms,
            "pin_until_ms": now_ms + args.selection_residency_ttl_ms,
            "device": assigned_device,
            "tier": {
                "GPU": "RELOAD_SAFE_GPU",
                "RAM": "HOST_RAM",
                "DISK": "DISK",
            }[resident.tier],
        })
        return (value,)

    def reusable_state_v3():
        now_ms = int(time.time() * 1000)
        proofs = []
        # ``artifact_paths`` is the effective provider-local binding.  It
        # includes explicit ``ROLE=PATH`` bindings supplied by the deployment
        # runner; using only the policy-derived map here silently dropped those
        # artifacts from the V3 residency proof and caused every CPU/provider
        # offer to become REJECT even though the stage was loaded and usable.
        for role, source_name in artifact_paths.items():
            artifact_digest = str(residency_template.get("artifact_digest", ""))
            if not artifact_digest.startswith("sha256:"):
                artifact_digest = "sha256:" + artifact_digest
            identity = _qwen_residency_identity(
                residency_template,
                artifact_digest=artifact_digest,
                adapter_id=str(residency_template.get("adapter_id", "")),
                adapter_version=str(residency_template.get("adapter_version", "")),
                backend=str(residency_template.get("backend", "")),
                device=assigned_device,
                provider_boot_epoch=core_boot_epoch,
            )
            resident = residency_ledger.lookup(identity)
            if resident is None:
                continue
            tier = {
                "GPU": ResidencyTierV3.GPU,
                "RAM": ResidencyTierV3.RAM,
                "DISK": ResidencyTierV3.DISK,
            }.get(resident.tier)
            if tier is None:
                continue
            proof = ResidencyProofV3(
                artifact_digest=artifact_digest, role=role, rank=0,
                tier=tier, device_set=(assigned_device,),
                boot_epoch=core_boot_epoch, process_epoch=process_epoch,
                topology_digest=topology_digest,
                captured_at_ms=now_ms,
                expires_at_ms=now_ms + int(args.selection_residency_ttl_ms),
            )
            proofs.append(proof)
        return tuple(proofs)

    issuer_v3 = DIProviderOfferIssuerV3(
        provider=args.provider_identity, service=SERVICE,
        boot_epoch=core_boot_epoch, devices=(assigned_device,),
        signer_key_id="sha256:" + hashlib.sha256(signing_key).hexdigest(),
        sign_offer_digest=lambda digest: hmac.new(
            signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
    )

    return {
        "selection_offer_issuer": issuer if selection_dataflow_v2 else None,
        "selection_offer_issuer_v3": (
            issuer_v3 if selection_dataflow_v3 else None),
        "selection_participant": participant if selection_dataflow_v2 else None,
        "selection_wal_path": args.selection_wal_path if selection_dataflow_v2 else None,
        "selection_storage_key": storage_key if selection_dataflow_v2 else None,
        "selection_storage_key_epoch": core_boot_epoch if selection_dataflow_v2 else "",
        "selection_max_prepare_ms": args.selection_max_prepare_ms,
        "selection_cached_shards": cached_shards if selection_dataflow_v2 else None,
        "selection_reusable_state": reusable_state_v3 if selection_dataflow_v3 else None,
        # V3 is the normal request-first path.  Its ACK is observational and
        # does not hold resources, so a selected role must still be able to
        # run the same assignment-bound preparation callback when the offer
        # cannot prove exact residency.  Leaving this V2-only silently turns
        # a valid V3 ACCEPT_WITH_PREPARATION offer into a handler that can
        # never load its selected artifact.
        "runtime_preparer": (
            runtime_preparer
                if (selection_dataflow_v2 or selection_dataflow_v3)
            else None
        ),
    }


def _selection_v3_for_tiny_onnx(provider: APPProvider, args) -> dict:
    """Bind the deterministic Spec175 fixture to authenticated V3 offers.

    The tiny fixture is already present in each Provider's immutable policy
    artifact.  It therefore needs no V2 reservation WAL or deferred Repo
    materialization; it still must publish a signed V3 offer so the normal
    automatic planner can create assignment-bound Selection projections.
    """
    if not bool(getattr(args, "selection_dataflow_v3", False)):
        return {}
    if not str(getattr(args, "provider_identity", "")):
        raise RuntimeError("tiny ONNX V3 offers require provider identity")
    key_path = str(getattr(args, "selection_signing_key_file", ""))
    if not key_path:
        raise RuntimeError("tiny ONNX V3 offers require signing key file")
    signing_key = _read_secret(key_path)
    if len(signing_key) != 32:
        raise RuntimeError("tiny ONNX V3 offer key must be 32 bytes")
    try:
        boot_epoch = provider.provider_boot_epoch
    except AttributeError:
        boot_epoch = provider._network_provider._provider.provider.provider_boot_epoch
    if not boot_epoch:
        raise RuntimeError("NDNSF Core provider boot epoch is unavailable")
    # Core exposes the process epoch as a numeric value, while the automatic
    # DATA_V1 key offer binds it to the Provider identity namespace.
    boot_epoch = f"{args.provider_identity}:{boot_epoch}"
    device = "cuda:0" if args.device == "auto" and args.require_cuda else (
        "cpu" if args.device == "auto" else str(args.device))
    # V3 represents a CPU-only Provider with an empty device set.  The
    # topology contract reserves concrete device identities for CUDA devices;
    # advertising the literal ``cpu`` as a device makes the asynchronous ACK
    # handler reject an otherwise valid CPU offer before ACK_CLOSED.
    topology_devices = () if device == "cpu" else (device,)
    issuer = DIProviderOfferIssuerV3(
        provider=args.provider_identity,
        service=SERVICE,
        boot_epoch=boot_epoch,
        devices=topology_devices,
        signer_key_id="sha256:" + hashlib.sha256(signing_key).hexdigest(),
        sign_offer_digest=lambda digest: hmac.new(
            signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
    )
    return {"selection_offer_issuer_v3": issuer}


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


def _unexpected_cpu_fallback_count(model, assigned_device: str) -> int:
    """Count CPU execution only when the committed assignment required CUDA."""
    return int(
        str(assigned_device).startswith("cuda:")
        and bool(getattr(model, "ndnsf_cpu_fallback", True))
    )


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
        envelope = DIRequestEnvelopeV2.from_bytes(payload)
    except (TypeError, ValueError):
        pass
    else:
        payload = base64.b64decode(
            envelope.input_payload_b64.encode("ascii"), validate=True)
    try:
        doc = decode_qwen_pipeline_context(payload)
    except Exception:
        return payload
    mode = doc.get("contextMode", "full")
    session_id = str(doc.get("sessionId", ""))
    if mode == "append-delta":
        if not session_id:
            raise RuntimeError("append-delta Qwen context requires a sessionId")
        with _QWEN_CONTEXT_CACHE_LOCK:
            base = _QWEN_CONTEXT_CACHE.get(session_id)
        if base is None:
            raise RuntimeError(f"append-delta Qwen context has no cached base for {session_id}")
        doc = merge_qwen_pipeline_delta(base, doc)
    if doc.get("contextMode", "full") == "full" and session_id:
        # Keep the existing compatibility cache behavior for ordinary FULL
        # requests.  Conversation turns additionally replace this entry only
        # after their authenticated COMMIT (see _conversation_finalize).
        with _QWEN_CONTEXT_CACHE_LOCK:
            _QWEN_CONTEXT_CACHE[session_id] = dict(doc)
    return encode_qwen_pipeline_context(
        doc["inputIds"],
        attention_mask=doc.get("attentionMask"),
        position_ids=doc.get("positionIds"),
        request_id=str(doc.get("requestId", "")),
        session_id=session_id,
        context_epoch=int(doc.get("contextEpoch", 0) or 0),
        generation=doc.get("generation") or {},
    )


_GENERATION_CONTROL_SCOPE = "generation-control-v1"


def _qwen_response_generation_mode(streamed: bool) -> str:
    """Return the wire mode for the final-role response.

    The final-role implementation is shared by unary and streamed calls.  A
    streamed callback path must remain visibly streamed in the terminal
    response; otherwise downstream evidence can mistake a token stream for a
    unary generation even though the transport delivered events.
    """
    return "TOKEN_STREAMING" if bool(streamed) else "FULL"


def _qwen_generation_spec(payload: bytes) -> dict | None:
    """Read the signed DI task/options view for one unary or streamed generation."""

    try:
        envelope = DIRequestEnvelopeV2.from_bytes(bytes(payload))
        options_raw = base64.b64decode(
            envelope.options_payload_b64.encode("ascii"), validate=True)
        options = json.loads(options_raw.decode("utf-8")) if options_raw else {}
        task = dict(envelope.task)
    except (TypeError, ValueError, json.JSONDecodeError, UnicodeDecodeError):
        # Preplanned compatibility calls do not carry a DI envelope.  The
        # application context still carries the explicit FULL contract, so
        # accept it without weakening the normal envelope path.
        try:
            context = decode_qwen_pipeline_context(bytes(payload))
            generation = dict(context.get("generation") or {})
        except (TypeError, ValueError, json.JSONDecodeError, UnicodeDecodeError):
            return None
        mode = str(generation.get("outputMode", "")).upper()
        if mode != "FULL":
            return None
        raw_eos = generation.get("eosTokenIds", ())
        if isinstance(raw_eos, int):
            raw_eos = (raw_eos,)
        eos = tuple(int(item) for item in raw_eos)
        max_new_tokens = int(generation.get("maxNewTokens", 0) or 0)
        if max_new_tokens < 1 or max_new_tokens > 64 or not eos:
            raise ValueError("FULL Qwen context generation metadata is invalid")
        return {
            "max_new_tokens": max_new_tokens,
            "eos_token_ids": eos,
            "request_id": str(context.get("requestId", "")),
            "session_id": str(context.get("sessionId", "")),
        }
    mode = str(task.get("generation_mode", options.get("outputMode", ""))).upper()
    if mode not in {"FULL", "TOKEN_STREAMING"}:
        return None
    use_cache = options.get("useCache") is True
    if mode == "TOKEN_STREAMING" and not use_cache:
        raise ValueError(
            "TOKEN_STREAMING Qwen generation requires useCache=true")
    try:
        max_new_tokens = int(options.get("maxNewTokens", 0))
    except (TypeError, ValueError):
        raise ValueError("FULL Qwen generation requires integer maxNewTokens")
    if max_new_tokens < 1 or max_new_tokens > 64:
        raise ValueError("FULL Qwen generation maxNewTokens must be between 1 and 64")
    raw_eos = options.get("eosTokenIds", ())
    if isinstance(raw_eos, int):
        raw_eos = (raw_eos,)
    eos = tuple(int(item) for item in raw_eos)
    if not eos or any(item < 0 for item in eos):
        raise ValueError("FULL Qwen generation requires non-negative eosTokenIds")
    recovery = task.get("generation_recovery")
    if recovery is not None:
        recovery = GenerationRecoveryV1.from_dict(recovery)
        if (envelope.attempt != 2
                or recovery.recovery_request_id != envelope.request_id):
            raise ValueError("Qwen generation recovery binding mismatch")
        committed_prefix = recovery.committed_token_ids
    else:
        committed_prefix = ()
    conversation = task.get("conversation")
    conversation_contract = None
    if conversation is not None:
        try:
            conversation_contract = ConversationContinuation.from_dict(
                conversation)
            input_payload = base64.b64decode(
                envelope.input_payload_b64.encode("ascii"), validate=True)
            input_digest = "sha256:" + hashlib.sha256(input_payload).hexdigest()
            if (conversation_contract.turn_input_digest != input_digest or
                    conversation_contract.request_contract_digest !=
                    conversation_contract.request_contract(
                        input_digest=input_digest)):
                raise ConversationCheckpointInvalid(
                    "conversation request contract digest mismatch")
        except (ConversationCheckpointInvalid, ValueError, TypeError) as exc:
            raise ValueError("Qwen conversation continuation is invalid") from exc
    return {
        "generation_mode": mode,
        "use_cache": use_cache,
        "max_new_tokens": max_new_tokens,
        "eos_token_ids": eos,
        "request_id": str(envelope.request_id),
        "session_id": str(envelope.invocation_id),
        "generation_id": str(
            options.get("generationId", options.get("generation_id", ""))
            or task.get("generationId", task.get("generation_id", ""))),
        "attempt_epoch": int(envelope.attempt),
        "committed_prefix_token_ids": tuple(committed_prefix),
        "conversation": (conversation_contract.to_dict()
                          if conversation_contract is not None else None),
    }


def _require_python_qwen_generation_mode(spec: dict) -> None:
    """Keep the legacy Python role loop out of the native streaming contract."""

    if spec.get("generation_mode") == "TOKEN_STREAMING":
        raise RuntimeError(
            "TOKEN_STREAMING requires the native stateful ONNX Provider runtime")


def _generation_topic(
    ctx: ProviderRuntimeContext,
    edge,
    epoch: int,
) -> str:
    session = hashlib.sha256(ctx.request_id.encode("utf-8")).hexdigest()[:24]
    # Keep control records below the same application topic namespace as the
    # planned activation edges.  This is important for NDNSF's scoped
    # CollaborationContext subscriptions and avoids a second unregistered
    # topic tree for the internal loop.
    return edge.topic(f"full-token/{session}/{int(epoch)}")


def _generation_step_control(kind: str, epoch: int, **fields) -> bytes:
    return json.dumps({
        "schema": "ndnsf-di-qwen-generation-control-v1",
        "kind": str(kind),
        "epoch": int(epoch),
        **fields,
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _generation_control_doc(payload: bytes) -> dict | None:
    try:
        doc = json.loads(bytes(payload).decode("utf-8"))
    except (TypeError, ValueError, UnicodeDecodeError):
        return None
    if doc.get("schema") != "ndnsf-di-qwen-generation-control-v1":
        return None
    return doc


def _generation_dependency_timeout(ctx: ProviderRuntimeContext) -> int:
    return max(1, int(ctx.dependency_timeout_ms(fallback_ms=30000)))


def _generation_publish_output(
    ctx: ProviderRuntimeContext,
    edge_scope: str,
    epoch: int,
    payload: bytes,
) -> None:
    ctx.publish_output_large_reference(
        payload,
        key_scope=edge_scope,
        data_topic_suffix=f"full/{epoch}/data",
        ref_topic_suffix=f"full/{epoch}/ref",
        object_type="application/x-ndnsf-di-qwen-generation-step",
        object_id=f"{ctx.request_id}-{epoch}",
        max_segment_size=7000,
        freshness_ms=60000,
    )


def _generation_wait_input(
    ctx: ProviderRuntimeContext,
    edge_scope: str,
    epoch: int,
) -> bytes:
    future = ctx.prefetch_input_large(
        key_scope=edge_scope,
        topic_suffix=f"full/{epoch}/ref",
        ref_timeout_ms=_generation_dependency_timeout(ctx),
        fetch_timeout_ms=_generation_dependency_timeout(ctx),
    )
    value = ctx.wait_prefetched_input_large(
        future, timeout_ms=_generation_dependency_timeout(ctx))
    return bytes(value)


def _handle_qwen_transformer_full_generation(
    ctx: ProviderRuntimeContext,
    *,
    model,
    stages: int,
    stage_index: int,
    compute_delay_ms: float,
    spec: dict,
    stage_runner=None,
) -> dict[str, tuple[int, ...]]:
    """Run one full autoregressive invocation across the selected stages."""

    if stage_runner is None:
        stage_runner = lambda payload, delay: run_qwen_transformer_stage(
            payload,
            role=ctx.role,
            stages=stages,
            model=model,
            compute_delay_ms=delay,
        )
    streamed = getattr(ctx, "stream_writer", None) is not None
    if not ctx.dependencies.outputs and stage_index != stages - 1:
        raise RuntimeError("FULL Qwen stage graph lacks its output dependency")
    if not ctx.dependencies.inputs and stage_index != 0:
        raise RuntimeError("FULL Qwen stage graph lacks its input dependency")
    if stage_index == 0:
        payload, _, _ = _materialize_first_stage_request(ctx)
        context_doc = decode_qwen_pipeline_context(payload)
        input_ids = context_doc.get("inputIds")
        if not isinstance(input_ids, list) or not input_ids:
            raise ValueError("FULL Qwen request inputIds must be non-empty")
        sequence = list(input_ids[0] if isinstance(input_ids[0], list) else input_ids)
        generated: list[int] = []
        committed_prefix = tuple(
            int(value) for value in spec.get(
                "committed_prefix_token_ids", ()))
        token_completion_monotonic_ms: list[float] = []
        output_edge = ctx.dependencies.output()
        _emit("LLM_PIPELINE_QWEN_FULL_STAGE_START", f"requestId={ctx.request_id}", f"role={ctx.role}", flush=True)
        for epoch in range(int(spec["max_new_tokens"])):
            hidden = stage_runner(payload, compute_delay_ms)
            _generation_publish_output(ctx, output_edge.key_scope, epoch, hidden)
            _emit(
                "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED",
                f"requestId={ctx.request_id}", f"role={ctx.role}",
                f"epoch={epoch}",
                f"monotonicMs={time.perf_counter() * 1000.0:.3f}",
                flush=True,
            )
            token_data = ctx.ndnsf.wait_one(
                output_edge.key_scope,
                _generation_topic(ctx, output_edge, epoch),
                _generation_dependency_timeout(ctx),
            )
            if token_data is None:
                raise TimeoutError(f"FULL Qwen token result timed out at epoch {epoch}")
            token_doc = _generation_control_doc(token_data.payload)
            if not token_doc or token_doc.get("kind") != "TOKEN":
                raise RuntimeError("FULL Qwen stage0 received an invalid token control record")
            if int(token_doc.get("epoch", -1)) != epoch:
                raise RuntimeError("FULL Qwen token epoch mismatch")
            token = int(token_doc.get("token", -1))
            if token < 0:
                raise RuntimeError("FULL Qwen token result is negative")
            generated.append(token)
            replaying_prefix = epoch < len(committed_prefix)
            if replaying_prefix and token != committed_prefix[epoch]:
                raise RuntimeError(
                    "Qwen recovery recomputed prefix does not match")
            token_completion_monotonic_ms.append(
                time.perf_counter() * 1000.0)
            _emit("LLM_PIPELINE_QWEN_FULL_TOKEN_RECEIVED", f"requestId={ctx.request_id}", f"role={ctx.role}", f"epoch={epoch}", f"token={token}", flush=True)
            sequence.append(token)
            if token in spec["eos_token_ids"]:
                break
            payload = encode_qwen_pipeline_context(
                [sequence],
                request_id=ctx.request_id,
                session_id=spec["session_id"],
                context_epoch=epoch + 1,
            )
        stop_epoch = len(generated)
        stop_reason = (
            "EOS" if generated and generated[-1] in spec["eos_token_ids"]
            else "TOKEN_LIMIT")
        _generation_publish_output(
            ctx,
            output_edge.key_scope,
            stop_epoch,
            _generation_step_control(
                "STOP", stop_epoch,
                inputTokenIds=list(input_ids[0]
                                   if isinstance(input_ids[0], list)
                                   else input_ids),
                generatedTokenIds=generated,
                stopReason=stop_reason,
                tokenCompletionMonotonicMs=token_completion_monotonic_ms,
            ),
        )
        _emit(
            "LLM_PIPELINE_QWEN_FULL_STOP_SENT",
            f"requestId={ctx.request_id}",
            f"role={ctx.role}",
            f"epoch={stop_epoch}",
            flush=True,
        )
        _emit(
            "LLM_PIPELINE_QWEN_FULL_STOP_PUBLISHED",
            f"requestId={ctx.request_id}",
            f"tokenCount={len(generated)}",
            f"stopReason={stop_reason}",
            flush=True,
        )
        return {
            "input_token_ids": tuple(
                int(value) for value in (
                    input_ids[0] if isinstance(input_ids[0], list)
                    else input_ids)),
            "generated_token_ids": tuple(generated),
        }

    input_edge = ctx.dependencies.input()
    if stage_index < stages - 1:
        output_edge = ctx.dependencies.output()
        _emit("LLM_PIPELINE_QWEN_FULL_STAGE_START", f"requestId={ctx.request_id}", f"role={ctx.role}", flush=True)
        for epoch in range(int(spec["max_new_tokens"]) + 1):
            incoming = _generation_wait_input(ctx, input_edge.key_scope, epoch)
            _emit(
                "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED",
                f"requestId={ctx.request_id}", f"role={ctx.role}",
                f"epoch={epoch}",
                f"monotonicMs={time.perf_counter() * 1000.0:.3f}",
                flush=True,
            )
            control = _generation_control_doc(incoming)
            if control:
                if control.get("kind") != "STOP":
                    raise RuntimeError("FULL Qwen middle stage received unknown control")
                # STOP is a signed transcript control record, not a local
                # stage marker.  Preserve every field published by Stage0
                # (input identity, generated-token transcript, stop reason,
                # and completion timestamps) while forwarding it to the next
                # role.  Rebuilding only {kind, epoch} loses the terminal
                # evidence required by the final role and makes every
                # multi-stage run fail at transcript validation.
                _generation_publish_output(
                    ctx, output_edge.key_scope, epoch,
                    incoming,
                )
                _emit(
                    "LLM_PIPELINE_QWEN_FULL_STOP_FORWARDED",
                    f"requestId={ctx.request_id}",
                    f"role={ctx.role}",
                    f"epoch={epoch}",
                    flush=True,
                )
                return {
                    "input_token_ids": tuple(
                        int(value) for value in control.get(
                            "inputTokenIds", ())),
                    "generated_token_ids": tuple(
                        int(value) for value in control.get(
                            "generatedTokenIds", ())),
                }
            output = stage_runner(incoming, compute_delay_ms)
            _generation_publish_output(ctx, output_edge.key_scope, epoch, output)
            _emit(
                "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED",
                f"requestId={ctx.request_id}", f"role={ctx.role}",
                f"epoch={epoch}",
                f"monotonicMs={time.perf_counter() * 1000.0:.3f}",
                flush=True,
            )
            token_data = ctx.ndnsf.wait_one(
                output_edge.key_scope,
                _generation_topic(ctx, output_edge, epoch),
                _generation_dependency_timeout(ctx),
            )
            if token_data is None:
                raise TimeoutError(f"FULL Qwen middle-stage token timed out at epoch {epoch}")
            token_doc = _generation_control_doc(token_data.payload)
            if not token_doc or token_doc.get("kind") != "TOKEN":
                raise RuntimeError("FULL Qwen middle stage received an invalid token record")
            ctx.ndnsf.publish(
                input_edge.key_scope,
                _generation_topic(ctx, input_edge, epoch),
                token_data.payload,
            )
            _emit("LLM_PIPELINE_QWEN_FULL_TOKEN_FORWARDED", f"requestId={ctx.request_id}", f"role={ctx.role}", f"epoch={epoch}", flush=True)
        raise RuntimeError("FULL Qwen middle stage exceeded generation limit")

    _emit("LLM_PIPELINE_QWEN_FULL_STAGE_START", f"requestId={ctx.request_id}", f"role={ctx.role}", flush=True)
    generated: list[int] = []
    committed_prefix = tuple(
        int(value) for value in spec.get(
            "committed_prefix_token_ids", ()))
    for epoch in range(int(spec["max_new_tokens"]) + 1):
        incoming = _generation_wait_input(ctx, input_edge.key_scope, epoch)
        _emit(
            "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED",
            f"requestId={ctx.request_id}", f"role={ctx.role}",
            f"epoch={epoch}",
            f"monotonicMs={time.perf_counter() * 1000.0:.3f}",
            flush=True,
        )
        control = _generation_control_doc(incoming)
        if control:
            if control.get("kind") != "STOP":
                raise RuntimeError("FULL Qwen final stage received unknown control")
            if int(control.get("epoch", -1)) != len(generated):
                raise RuntimeError("FULL Qwen STOP epoch/transcript mismatch")
            declared_tokens = tuple(
                int(value) for value in control.get(
                    "generatedTokenIds", ()))
            if declared_tokens != tuple(generated):
                raise RuntimeError("FULL Qwen STOP token transcript mismatch")
            input_token_ids = tuple(
                int(value) for value in control.get("inputTokenIds", ()))
            if not input_token_ids:
                raise RuntimeError("FULL Qwen STOP omitted input token identity")
            stop_reason = str(control.get("stopReason", ""))
            if stop_reason not in {"EOS", "TOKEN_LIMIT"}:
                raise RuntimeError("FULL Qwen STOP reason is invalid")
            completion_times = tuple(
                float(value) for value in control.get(
                    "tokenCompletionMonotonicMs", ()))
            if len(completion_times) != len(generated):
                raise RuntimeError("FULL Qwen STOP timing transcript mismatch")
            response = {
                "schema": "ndnsf-di-qwen-generation-response-v1",
                "requestId": ctx.request_id,
                "inputTokenIds": list(input_token_ids),
                "generatedTokenIds": generated,
                # The generic streamed handle validates this canonical key.
                "tokenIds": generated,
                "tokenEvidence": [
                    {
                        "requestId": ctx.request_id,
                        "tokenIndex": index,
                        "contextLength": len(input_token_ids) + index,
                        "tokenId": token,
                    }
                    for index, token in enumerate(generated)
                ],
                "topToken": generated[-1] if generated else -1,
                "stopReason": stop_reason,
                "stageCount": stages,
                # ``streamed`` is the authoritative delivery mode for this
                # response.  The same final-role loop supports a compatibility
                # unary call, but must not label a streamed response as FULL.
                "generationMode": _qwen_response_generation_mode(streamed),
                "wireRequestCount": 1,
                "tokenRequestCount": 0,
                "tokenCompletionClock": "CLOCK_MONOTONIC",
                "tokenCompletionMonotonicMs": list(completion_times),
            }
            response_payload = json.dumps(
                response, sort_keys=True,
                separators=(",", ":")).encode("utf-8")
            if streamed:
                ctx.finish_stream(
                    response_payload,
                    finish_reason=("eos" if stop_reason == "EOS"
                                   else "max_tokens"),
                )
            else:
                ctx.publish_final_response(response_payload)
            _emit(
                "LLM_PIPELINE_QWEN_FULL_STOP_CONSUMED",
                f"requestId={ctx.request_id}",
                f"role={ctx.role}",
                f"epoch={len(generated)}",
                flush=True,
            )
            _emit(
                "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL",
                f"requestId={ctx.request_id}",
                f"tokenCount={len(generated)}",
                f"stopReason={stop_reason}",
                flush=True,
            )
            return {
                "input_token_ids": tuple(input_token_ids),
                "generated_token_ids": tuple(generated),
            }
        output = stage_runner(incoming, compute_delay_ms)
        result = decode_payload(output)
        token = int(result.get("topToken", -1))
        if token < 0:
            raise RuntimeError("FULL Qwen final stage produced no topToken")
        generated.append(token)
        replaying_prefix = epoch < len(committed_prefix)
        if replaying_prefix and token != committed_prefix[epoch]:
            raise RuntimeError(
                "Qwen recovery recomputed prefix does not match")
        if streamed and not replaying_prefix:
            # Only the terminal role receives the event-key grant and owns
            # End/Response. It therefore publishes the externally visible
            # token at the same point where sampling commits it.
            ctx.publish_event(json.dumps({
                "type": "GenerationTokenEventV1",
                "requestId": ctx.request_id,
                "tokenEpoch": len(generated),
                "tokenId": token,
                "textDelta": "",
                "acceptedPrefixDigest": accepted_prefix_digest(generated),
            }, sort_keys=True, separators=(",", ":")).encode("utf-8"))
        ctx.ndnsf.publish(
            input_edge.key_scope,
            _generation_topic(ctx, input_edge, epoch),
            _generation_step_control("TOKEN", epoch, token=token),
        )
        _emit(
            "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED",
            f"requestId={ctx.request_id}", f"role={ctx.role}",
            f"epoch={epoch}", f"token={token}",
            f"monotonicMs={time.perf_counter() * 1000.0:.3f}",
            flush=True,
        )


def _print_qwen_stage_timing(**fields) -> None:
    normalized = {
        key: (f"{value:.2f}" if isinstance(value, float) else value)
        for key, value in fields.items()
    }
    line = "LLM_PIPELINE_QWEN_STAGE_TIMING " + " ".join(
        f"{key}={value}" for key, value in normalized.items()) + "\n"
    # The native runtime and Python provider share stdout.  One write syscall
    # prevents native log records from being inserted between the marker and
    # its fields, which would corrupt request-lineage evidence.
    with _PIPELINE_MARKER_LOCK:
        os.write(1, line.encode("utf-8"))
        marker_path = os.environ.get("NDNSF_PIPELINE_MARKER_LOG", "").strip()
        if marker_path:
            try:
                with open(marker_path, "a", encoding="utf-8") as stream:
                    stream.write(line)
                    stream.flush()
            except OSError:
                pass


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
        _emit(
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
    _emit(
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
        _emit(
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
    _emit(
        "LLM_PIPELINE_TRANSFORMER_STAGE_OUTPUT",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


def handle_qwen_transformer_stage(ctx: ProviderRuntimeContext, *,
                                  stages: int,
                                  model_cache,
                                  compute_delay_ms: float,
                                  device: str = "cpu",
                                  require_cuda: bool = False,
                                  model_cache_lock=None) -> None:
    total_start = time.perf_counter()
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    model = model_cache.get(model_key) or model_cache.get(str(ctx.role))
    cache_hit = model is not None
    model_load_ms = 0.0
    if model is None:
        if not model_key:
            raise RuntimeError("Qwen stage execution requires a model artifact path")
        lock = model_cache_lock or Lock()
        with lock:
            model = model_cache.get(model_key) or model_cache.get(str(ctx.role))
            cache_hit = model is not None
            if model is None:
                load_start = time.perf_counter()
                model = qwen_transformer_model_from_stage_package(
                    model_key,
                    device=device,
                    require_cuda=require_cuda,
                )
                model_load_ms = _elapsed_ms(load_start)
                model_cache[model_key] = model
                _emit(
                    "LLM_PIPELINE_QWEN_MODEL_RESIDENCY",
                    f"role={ctx.role}",
                    f"path={model_key}",
                    "cacheHit=false",
                    f"load_ms={model_load_ms:.2f}",
                    f"device={getattr(model, 'ndnsf_execution_device', 'unknown')}",
                    f"cpuFallback={str(bool(getattr(model, 'ndnsf_cpu_fallback', True))).lower()}",
                    flush=True,
                )
    if cache_hit:
        _emit(
            "LLM_PIPELINE_QWEN_MODEL_RESIDENCY",
            f"role={ctx.role}",
            f"path={model_key}",
            "cacheHit=true",
            "load_ms=0.00",
            f"device={getattr(model, 'ndnsf_execution_device', 'unknown')}",
            f"cpuFallback={str(bool(getattr(model, 'ndnsf_cpu_fallback', True))).lower()}",
            flush=True,
        )
    if require_cuda and (
            bool(getattr(model, "ndnsf_cpu_fallback", True))
            or not str(getattr(model, "ndnsf_execution_device", "")).startswith("cuda")):
        raise RuntimeError("Qwen stage cache is not CUDA-resident")
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

    full_generation = _qwen_generation_spec(ctx.request)
    if full_generation is not None:
        _require_python_qwen_generation_mode(full_generation)
        _handle_qwen_transformer_full_generation(
            ctx,
            model=model,
            stages=stages,
            stage_index=stage_index,
            compute_delay_ms=compute_delay_ms,
            spec=full_generation,
        )
        return
    is_first = not ctx.dependencies.inputs
    is_final = not ctx.dependencies.outputs

    input_future = None
    prefetch_submit_ms = 0.0
    if not is_first:
        prefetch_submit_start = time.perf_counter()
        dependency_timeout_ms = ctx.dependency_timeout_ms()
        producer_role = _producer_for_single_input(ctx)
        _emit(
            "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_WAIT",
            f"requestId={ctx.request_id}",
            f"role={ctx.role}",
            f"producerRole={producer_role}",
            flush=True,
        )
        input_future = ctx.prefetch_input_large(
            producer_role=producer_role,
            ref_timeout_ms=dependency_timeout_ms,
            fetch_timeout_ms=dependency_timeout_ms,
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
            timeout_ms=ctx.dependency_timeout_ms(),
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

    _emit(
        "LLM_PIPELINE_QWEN_STAGE_EXECUTION_READY",
        f"requestId={ctx.request_id}",
        f"role={ctx.role}",
        f"stage={stage_index}",
        f"modelCacheHit={str(cache_hit).lower()}",
        f"inputBytes={len(input_payload)}",
        f"dependencyWaitMs={input_wait_ms:.2f}",
        f"dependencyFetchMs={fetch_ms:.2f}",
        f"dependencyRefWaitMs={ref_wait_ms:.2f}",
        flush=True,
    )
    if not is_first:
        _emit(
            "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_READY",
            f"requestId={ctx.request_id}",
            f"role={ctx.role}",
            f"producerRole={_producer_for_single_input(ctx)}",
            f"waitMs={input_wait_ms:.2f}",
            f"fetchMs={fetch_ms:.2f}",
            flush=True,
        )

    runner_timing: dict[str, float | int | str] = {}
    output = run_qwen_transformer_stage(
        input_payload,
        role=ctx.role,
        stages=stages,
        model=model,
        compute_delay_ms=compute_delay_ms,
        timing=runner_timing,
    )
    input_sha256 = hashlib.sha256(input_payload).hexdigest()
    output_sha256 = hashlib.sha256(output).hexdigest()
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
            model_cache_hit=int(cache_hit),
            model_load_ms=model_load_ms,
            device=runner_timing.get("device", "unknown"),
            cpuFallback=runner_timing.get("cpu_fallback", 1),
            input_sha256=input_sha256,
            output_sha256=output_sha256,
            dataName="-",
            artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
            runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
            publish_ms=publish_ms,
            total_ms=_elapsed_ms(total_start),
        )
        _emit(
            "LLM_PIPELINE_QWEN_STAGE_FINAL",
            f"requestId={ctx.request_id}",
            f"role={ctx.role}",
            f"bytes={len(output)}",
            f"sha256={output_sha256}",
            flush=True,
        )
        _emit(
            "LLM_PIPELINE_QWEN_FINAL_RESPONSE_PUBLISHED",
            f"requestId={ctx.request_id}",
            f"role={ctx.role}",
            f"bytes={len(output)}",
            f"sha256={output_sha256}",
            flush=True,
        )
        return

    data_name = _planned_output_name(ctx)
    publish_start = time.perf_counter()
    published_name = ctx.publish_output_large_reference(
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
        model_cache_hit=int(cache_hit),
        model_load_ms=model_load_ms,
        device=runner_timing.get("device", "unknown"),
        cpuFallback=runner_timing.get("cpu_fallback", 1),
        input_sha256=input_sha256,
        output_sha256=output_sha256,
        dataName=published_name,
        artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
        runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
        publish_ms=publish_ms,
        total_ms=_elapsed_ms(total_start),
    )
    _emit(
        "LLM_PIPELINE_QWEN_STAGE_OUTPUT",
        f"requestId={ctx.request_id}",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"sha256={hashlib.sha256(output).hexdigest()}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


def handle_qwen_onnx_stage(ctx: ProviderRuntimeContext, *,
                           stages: int,
                           session_cache,
                           metadata_cache,
                           compute_delay_ms: float,
                           device: str = "cpu",
                           require_cuda: bool = False) -> None:
    total_start = time.perf_counter()
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    session = _qwen_onnx_session_from_cache(session_cache, model_key)
    if session is None:
        if not model_key:
            raise RuntimeError("Qwen ONNX stage execution requires an ONNX artifact path")
        session = _qwen_onnx_session(
            model_key, device=device, require_cuda=require_cuda)
        session_cache[model_key] = session
    execution_device, cpu_fallback = _qwen_onnx_session_placement(session)
    if require_cuda and cpu_fallback:
        raise RuntimeError("Qwen ONNX stage cache is not CUDA-resident")
    metadata = metadata_cache.get(model_key, {})
    stage_index = int(metadata.get("stageIndex", role_index(ctx.role)))
    full_generation = _qwen_generation_spec(ctx.request)
    if full_generation is not None:
        _require_python_qwen_generation_mode(full_generation)
        _handle_qwen_transformer_full_generation(
            ctx,
            model=session,
            stages=int(metadata.get("stageCount", stages)),
            stage_index=stage_index,
            compute_delay_ms=compute_delay_ms,
            spec=full_generation,
            stage_runner=lambda payload, delay: run_qwen_onnx_stage(
                payload,
                role=ctx.role,
                stages=int(metadata.get("stageCount", stages)),
                session=session,
                metadata=metadata,
                compute_delay_ms=delay,
                timing={},
            ),
        )
        return
    is_first = not ctx.dependencies.inputs
    is_final = not ctx.dependencies.outputs

    input_future = None
    prefetch_submit_ms = 0.0
    if not is_first:
        prefetch_submit_start = time.perf_counter()
        dependency_timeout_ms = ctx.dependency_timeout_ms()
        input_future = ctx.prefetch_input_large(
            producer_role=_producer_for_single_input(ctx),
            ref_timeout_ms=dependency_timeout_ms,
            fetch_timeout_ms=dependency_timeout_ms,
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
            timeout_ms=ctx.dependency_timeout_ms(),
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
    runner_timing["device"] = execution_device
    runner_timing["cpu_fallback"] = int(cpu_fallback)
    output = run_qwen_onnx_stage(
        input_payload,
        role=ctx.role,
        stages=stages,
        session=session,
        metadata=metadata,
        compute_delay_ms=compute_delay_ms,
        timing=runner_timing,
    )
    input_sha256 = hashlib.sha256(input_payload).hexdigest()
    output_sha256 = hashlib.sha256(output).hexdigest()
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
            device=runner_timing.get("device", "unknown"),
            cpuFallback=runner_timing.get("cpu_fallback", 1),
            input_sha256=input_sha256,
            output_sha256=output_sha256,
            dataName="-",
            artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
            runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
            publish_ms=publish_ms,
            total_ms=_elapsed_ms(total_start),
        )
        _emit("LLM_PIPELINE_QWEN_ONNX_STAGE_FINAL", f"role={ctx.role}", f"bytes={len(output)}", flush=True)
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
        device=runner_timing.get("device", "unknown"),
        cpuFallback=runner_timing.get("cpu_fallback", 1),
        input_sha256=input_sha256,
        output_sha256=output_sha256,
        dataName=data_name or "-",
        artificial_delay_ms=float(runner_timing.get("artificial_delay_ms", 0.0)),
        runner_total_ms=float(runner_timing.get("total_ms", 0.0)),
        publish_ms=publish_ms,
        total_ms=_elapsed_ms(total_start),
    )
    _emit(
        "LLM_PIPELINE_QWEN_ONNX_STAGE_OUTPUT",
        f"role={ctx.role}",
        f"bytes={len(output)}",
        f"plannedName={bool(data_name)}",
        flush=True,
    )


_CONVERSATION_STATE_SCOPE = "ndnsf-di-conversation-state-v1"
_CONVERSATION_RECEIPT_TOPIC = "/ndnsf-di/conversation/receipt"
_CONVERSATION_READY_TOPIC = "/ndnsf-di/conversation/ready"
_CONVERSATION_CONTROL_TOPIC = "/ndnsf-di/conversation/control"
_CONVERSATION_COMMIT_TOPIC = "/ndnsf-di/conversation/commit"


def _conversation_digest(value: object) -> str:
    return canonical_digest(value)


def _copy_onnx_state(value: object) -> object:
    """Copy adapter-owned state without exposing a mutable parent entry."""
    return copy.deepcopy(value)


def _qwen_context_document(payload: bytes) -> dict[str, Any] | None:
    """Decode the application context without accepting a malformed envelope.

    Conversation state is keyed by the application ``sessionId`` carried in
    the Qwen context, not by the per-request invocation ID generated by the
    planner.  Keeping this extraction in one helper prevents a later turn
    from accidentally creating a second cache namespace.
    """
    try:
        envelope = DIRequestEnvelopeV2.from_bytes(bytes(payload))
        payload = base64.b64decode(
            envelope.input_payload_b64.encode("ascii"), validate=True)
    except (TypeError, ValueError, UnicodeDecodeError):
        pass
    try:
        return decode_qwen_pipeline_context(bytes(payload))
    except (TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
        return None


def _qwen_context_cache_after_commit(
        payload: bytes, *, prefix: tuple[int, ...], context_epoch: int) -> None:
    """Publish the committed full prefix to the Provider-local context index.

    The index contains only the small canonical token/context metadata used to
    expand a later APPEND_DELTA request.  KV/recurrent tensors remain in the
    conversation state manager; no state tensor is copied into this cache or
    onto NDN.  Updating it after COMMIT is essential: a failed/rolled-back
    turn must not advance the append-delta epoch.
    """
    document = _qwen_context_document(payload)
    if not document:
        return
    session_id = str(document.get("sessionId", "") or "")
    if not session_id or not prefix or int(context_epoch) < 1:
        return
    raw_positions = document.get("positionIds")
    model_type = "qwen3_5" if (
        isinstance(raw_positions, list) and len(raw_positions) == 4
    ) else "qwen2"
    cached = decode_qwen_pipeline_context(
        encode_qwen_pipeline_context(
            [list(prefix)],
            attention_mask=[[1] * len(prefix)],
            request_id=str(document.get("requestId", "")),
            session_id=session_id,
            context_epoch=int(context_epoch),
            generation=document.get("generation") or {},
            model_type=model_type,
        )
    )
    with _QWEN_CONTEXT_CACHE_LOCK:
        _QWEN_CONTEXT_CACHE[session_id] = cached


def _conversation_state_bytes(value: object) -> int:
    total = 0
    if isinstance(value, Mapping):
        for item in value.values():
            total += _conversation_state_bytes(item)
        return total
    nbytes = getattr(value, "nbytes", None)
    if nbytes is not None:
        try:
            return max(0, int(nbytes))
        except (TypeError, ValueError):
            pass
    if isinstance(value, (bytes, bytearray, memoryview)):
        return len(value)
    return 0


def _conversation_projection_metadata(ctx: ProviderRuntimeContext) -> tuple[
        ConversationTurnBindingV1 | None, ConversationStateReferenceV1 | None,
        Mapping[str, object]]:
    metadata = dict(getattr(getattr(ctx.execution, "spec", None),
                            "metadata", {}) or {})
    raw_binding = metadata.get("conversationTurnBinding")
    raw_reference = metadata.get("conversationStateReference")
    binding = (ConversationTurnBindingV1.from_dict(raw_binding)
               if isinstance(raw_binding, Mapping) else None)
    reference = (ConversationStateReferenceV1.from_dict(raw_reference)
                 if isinstance(raw_reference, Mapping) else None)
    if binding is None:
        if reference is not None:
            raise ConversationCheckpointInvalid(
                "conversation state reference has no turn binding")
        return None, None, metadata
    if binding.service_name != SERVICE:
        raise ConversationCheckpointInvalid(
            "conversation turn binding service mismatch")
    if reference is not None and (
            reference.conversation_id != binding.conversation_id
            or reference.context_epoch != binding.parent_context_epoch
            or reference.plan_role_map_digest != binding.plan_role_map_digest
            or reference.checkpoint_digest != binding.parent_checkpoint_digest
            or reference.role_name != ctx.role):
        raise ConversationCheckpointInvalid(
            "conversation state reference binding mismatch")
    return binding, reference, metadata


def _conversation_continuation(spec: Mapping[str, object],
                               binding: ConversationTurnBindingV1
                               ) -> ConversationContinuation:
    raw = spec.get("conversation")
    if not isinstance(raw, Mapping):
        raise ConversationCheckpointInvalid(
            "conversation turn binding has no continuation")
    continuation = ConversationContinuation.from_dict(raw)
    if (continuation.conversation_id != binding.conversation_id
            or (continuation.mode.value == "append-delta"
                and continuation.expected_parent_context_epoch
                != binding.parent_context_epoch)):
        raise ConversationCheckpointInvalid(
            "conversation continuation/binding mismatch")
    if continuation.request_contract_digest != binding.request_contract_digest:
        raise ConversationCheckpointInvalid(
            "conversation request contract digest mismatch")
    return continuation


def _conversation_model_digest(ctx: ProviderRuntimeContext,
                               metadata: Mapping[str, object]) -> str:
    try:
        envelope = DIRequestEnvelopeV2.from_bytes(bytes(ctx.request))
    except (TypeError, ValueError):
        return _conversation_digest((SERVICE, "tiny-model"))
    # DIRequestEnvelopeV2 already stores the canonical ``sha256:<64-hex>``
    # identity.  Do not prepend a second scheme marker; receipts are parsed
    # by the strict conversation contract validator.
    return str(envelope.model_identity_hash)


def _conversation_state_component_digests(
        state: Mapping[str, object], *, role: str) -> tuple[str, ...]:
    names = tuple(sorted(str(name) for name in state))
    if not names:
        names = ("state",)
    return tuple(_conversation_digest({
        "role": role, "component": name,
        "shape": list(getattr(state.get(name), "shape", ())),
        "dtype": str(getattr(state.get(name), "dtype", "")),
    }) for name in names)


def _conversation_receipt(
        ctx: ProviderRuntimeContext,
        *,
        binding: ConversationTurnBindingV1,
        spec: Mapping[str, object],
        metadata: Mapping[str, object],
        transcript: Mapping[str, object],
        prefix: tuple[int, ...],
        manager: ProviderConversationStateManager,
        state: Mapping[str, object],
        state_reference: ConversationStateReferenceV1 | None,
        ) -> ProviderConversationStateReceiptV1:
    provider = str(ctx.ndnsf.local_provider)
    requester = str(getattr(ctx.ndnsf, "requester_name", "") or
                    "/example/llm-pipeline/user")
    origin_generation = str(metadata.get("generationId", "") or
                            spec.get("generation_id", ""))
    if (len(origin_generation) != 32 or
            any(char not in "0123456789abcdef" for char in origin_generation)):
        origin_generation = hashlib.sha256(
            f"{ctx.request_id}:{binding.conversation_id}".encode()).hexdigest()[:32]
    state_schema = _conversation_digest({
        "schema": "ndnsf-di-tiny-onnx-state-v1",
        "stage": ctx.role,
        "components": tuple(sorted(str(name) for name in state)),
    })
    role_split = _conversation_digest({
        "role": ctx.role,
        "layerRange": metadata.get("selectionLayerRange", ""),
        "rank": metadata.get("selectionRank", 0),
    })
    layout = _conversation_digest({
        "role": ctx.role,
        "backend": metadata.get("selectionBackend", metadata.get("selectionDevice", "cpu")),
        "runtime": TINY_ONNX_RUNTIME,
    })
    return ProviderConversationStateReceiptV1(
        conversation_id=binding.conversation_id,
        parent_context_epoch=binding.parent_context_epoch,
        successor_context_epoch=binding.successor_context_epoch,
        origin_request_id=ctx.request_id,
        origin_generation_id=origin_generation,
        service_name=binding.service_name,
        requester_identity=requester,
        security_domain_digest=_conversation_digest(
            metadata.get("securityDomain", "requester-default")),
        model_digest=_conversation_model_digest(ctx, metadata),
        graph_semantic_digest=_conversation_digest({
            "plan": metadata.get("selectionPlanDigest", ""),
            "core": metadata.get("planCoreDigest", ""),
            "role": ctx.role,
        }),
        adapter_digest=_conversation_digest({
            "id": metadata.get("selectionAdapterId", "tiny-onnx"),
            "version": metadata.get("selectionAdapterVersion", "v1"),
        }),
        role_name=ctx.role,
        role_split_digest=role_split,
        layout_digest=layout,
        plan_role_map_digest=binding.plan_role_map_digest,
        provider_identity=provider,
        provider_boot_id=manager.provider_boot_id,
        cache_epoch=manager.cache_epoch,
        prefix_digest=_conversation_digest({
            "schema": "ndnsf-di-prefix-v1", "tokenIds": list(prefix)}),
        prefix_token_count=len(prefix),
        position_digest=_conversation_digest({
            "role": ctx.role, "prefixLength": len(prefix),
            "contextEpoch": binding.successor_context_epoch,
        }),
        state_schema_digest=state_schema,
        state_component_digests=_conversation_state_component_digests(
            state, role=ctx.role),
        expires_at_ms=min(binding.retention_deadline_ms,
                          int(time.time() * 1000) + manager.retention_ms),
    )


def _conversation_control_matches(
        value: object, *, action: str, ctx: ProviderRuntimeContext,
        binding: ConversationTurnBindingV1,
        receipt: ProviderConversationStateReceiptV1,
    ) -> bool:
    if not isinstance(value, Mapping):
        return False
    # Treat a malformed control as an unrelated Data packet.  A provider must
    # never terminate the request merely because an unauthenticated or corrupt
    # control payload contains a non-integer field.
    try:
        parent_epoch = int(value.get("parentContextEpoch", -1))
        successor_epoch = int(value.get("successorContextEpoch", -1))
        expires_at_ms = int(value.get("expiresAtMs", 0))
    except (TypeError, ValueError, OverflowError):
        return False
    if expires_at_ms <= int(time.time() * 1000):
        return False
    if (value.get("schema") !=
            "ndnsf-di-conversation-promotion-control-v1" or
            value.get("action") != action or
            str(value.get("conversationId", "")) != binding.conversation_id or
            parent_epoch != binding.parent_context_epoch or
            successor_epoch != binding.successor_context_epoch or
            str(value.get("serviceName", "")) != binding.service_name or
            str(value.get("planRoleMapDigest", "")) !=
            binding.plan_role_map_digest or
            str(value.get("roleName", "")) != ctx.role or
            str(value.get("receiptDigest", "")) != receipt.receipt_digest):
        return False
    # COMMIT is only valid when it binds the exact finalized User checkpoint;
    # ROLLBACK intentionally does not need that field.
    if action == "COMMIT":
        checkpoint_digest = str(value.get("checkpointDigest", ""))
        if not checkpoint_digest:
            return False
    return True


def _conversation_commit_or_rollback(
        ctx: ProviderRuntimeContext,
        *,
        manager: ProviderConversationStateManager,
        receipt: ProviderConversationStateReceiptV1,
        binding: ConversationTurnBindingV1,
        generation_id: str,
        attempt_epoch: int,
        plan_digest: str,
        ) -> None:
    """Wait for the requester COMMIT and publish one Provider commit receipt."""
    ctx.ndnsf.allow_data(_CONVERSATION_STATE_SCOPE,
                         _CONVERSATION_CONTROL_TOPIC)
    deadline = time.monotonic() + max(
        0.001, ctx.remaining_deadline_ms(fallback_ms=30_000,
                                          safety_margin_ms=0) / 1000.0)
    while time.monotonic() < deadline:
        remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
        data = ctx.ndnsf.wait_one(
            _CONVERSATION_STATE_SCOPE, _CONVERSATION_CONTROL_TOPIC,
            min(remaining_ms, 1000))
        if data is None:
            continue
        try:
            control = json.loads(bytes(data.payload).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if _conversation_control_matches(
                control, action="ROLLBACK", ctx=ctx, binding=binding,
                receipt=receipt):
            manager.rollback_staged_promotion(receipt)
            raise RuntimeError("conversation promotion was rolled back")
        if not _conversation_control_matches(
                control, action="COMMIT", ctx=ctx, binding=binding,
                receipt=receipt):
            continue
        checkpoint_digest = str(control.get("checkpointDigest", ""))
        try:
            manager.commit_staged_promotion(
                receipt, request_id=ctx.request_id, role=ctx.role,
                checkpoint_digest=checkpoint_digest)
        except (ConversationStateUnavailable, ValueError) as exc:
            raise RuntimeError("conversation promotion commit was not staged") from exc
        commit = {
            "schema": "ndnsf-di-conversation-commit-v1",
            "requestId": ctx.request_id,
            "attemptEpoch": int(attempt_epoch),
            "generationId": str(generation_id),
            "planDigest": str(plan_digest),
            "conversationId": binding.conversation_id,
            "parentContextEpoch": binding.parent_context_epoch,
            "successorContextEpoch": binding.successor_context_epoch,
            "serviceName": binding.service_name,
            "planRoleMapDigest": binding.plan_role_map_digest,
            "roleName": ctx.role,
            "receiptDigest": receipt.receipt_digest,
            "checkpointDigest": checkpoint_digest,
            "providerIdentity": str(ctx.ndnsf.local_provider),
            "providerBootId": manager.provider_boot_id,
            "cacheEpoch": manager.cache_epoch,
            "committed": True,
        }
        ctx.ndnsf.publish(
            _CONVERSATION_STATE_SCOPE,
            f"{_CONVERSATION_COMMIT_TOPIC}/{ctx.role.strip('/')}",
            json.dumps(commit, sort_keys=True, separators=(",", ":")).encode(),
        )
        return
    manager.rollback_staged_promotion(receipt)
    raise TimeoutError("conversation promotion commit control timed out")


def _conversation_prepare(
        ctx: ProviderRuntimeContext,
        *,
        spec: Mapping[str, object],
        metadata: Mapping[str, object],
        state_cache: dict[tuple[str, int], dict[str, object]],
        stage_index: int,
        manager: ProviderConversationStateManager,
        receipt_store: dict[str, ProviderConversationStateReceiptV1],
        cancelled: Callable[[], bool] | None = None,
        ) -> tuple[ConversationTurnBindingV1 | None,
                   ConversationStateReferenceV1 | None,
                   ConversationContinuation | None,
                   object | None]:
    binding, reference, _ = _conversation_projection_metadata(ctx)
    if binding is None:
        return None, None, None, None
    continuation = _conversation_continuation(spec, binding)
    if continuation.mode.value != "append-delta":
        if reference is not None:
            raise ConversationCheckpointInvalid(
                "FULL_CONTEXT conversation has a parent state reference")
        return binding, None, continuation, None
    if reference is None:
        raise ConversationCheckpointInvalid(
            "APPEND_DELTA conversation has no state reference")
    parent_receipt = receipt_store.get(reference.role_receipt_digest)
    if parent_receipt is None:
        raise ConversationStateUnavailable(
            "conversation parent receipt is not retained by this Provider")
    if parent_receipt.successor_context_epoch != reference.context_epoch:
        raise ConversationCheckpointInvalid(
            "conversation parent receipt epoch mismatch")
    cancellation_check = cancelled or ctx.stream_cancelled
    try:
        acquired = manager.acquire_for_request(
            continuation, request_id=ctx.request_id, role=ctx.role,
            receipt=parent_receipt,
            deadline_ms=ctx.deadline_ms or None,
            cancelled=cancellation_check,
        )
    except ConversationStateUnavailable:
        if cancellation_check():
            _emit(
                "LLM_PIPELINE_SPEC175_M14_PREFETCH_CANCELLED",
                f"requestId={ctx.request_id}",
                f"role={ctx.role}",
                f"conversationId={binding.conversation_id}",
                f"contextEpoch={reference.context_epoch}",
                flush=True,
            )
        raise
    if acquired.transfer_bytes > 0:
        _emit(
            "LLM_PIPELINE_CONVERSATION_PREFETCHED",
            f"requestId={ctx.request_id}",
            f"role={ctx.role}",
            f"conversationId={binding.conversation_id}",
            f"contextEpoch={reference.context_epoch}",
            f"transferBytes={acquired.transfer_bytes}",
            f"transferLatencyMs={acquired.transfer_latency_ms}",
            flush=True,
        )
    state_cache[(ctx.request_id, stage_index)] = _copy_onnx_state(
        acquired.opaque_state)
    ready = {
        "schema": "ndnsf-di-conversation-ready-v1",
        "requestId": ctx.request_id,
        "attemptEpoch": int(spec.get("attempt_epoch", 1)),
        "generationId": str(metadata.get("generationId", "") or
                             spec.get("generation_id", "")),
        "planDigest": str(metadata.get("selectionPlanDigest", "")),
        "conversationId": binding.conversation_id,
        "contextEpoch": reference.context_epoch,
        "serviceName": binding.service_name,
        "planRoleMapDigest": binding.plan_role_map_digest,
        "checkpointDigest": reference.checkpoint_digest,
        "roleName": ctx.role,
        "roleReceiptDigest": reference.role_receipt_digest,
        "providerIdentity": str(ctx.ndnsf.local_provider),
        "providerBootId": manager.provider_boot_id,
        "cacheEpoch": manager.cache_epoch,
        "ready": True,
        "reason": "state-hit",
        "residency": "GPU_RESIDENT",
    }
    ctx.ndnsf.allow_data(_CONVERSATION_STATE_SCOPE, _CONVERSATION_READY_TOPIC)
    ctx.ndnsf.publish(
        _CONVERSATION_STATE_SCOPE,
        f"{_CONVERSATION_READY_TOPIC}/{ctx.role.strip('/')}",
        json.dumps(ready, sort_keys=True, separators=(",", ":")).encode(),
    )
    return binding, reference, continuation, parent_receipt


def _conversation_finalize(
        ctx: ProviderRuntimeContext,
        *,
        binding: ConversationTurnBindingV1,
        continuation: ConversationContinuation,
        metadata: Mapping[str, object],
        spec: Mapping[str, object],
        transcript: Mapping[str, object],
        state_cache: dict[tuple[str, int], dict[str, object]],
        stage_index: int,
        manager: ProviderConversationStateManager,
        receipt_store: dict[str, ProviderConversationStateReceiptV1],
        prefix_store: dict[str, tuple[int, ...]],
        ) -> ProviderConversationStateReceiptV1:
    input_ids = tuple(int(item) for item in transcript.get(
        "input_token_ids", ()))
    generated = tuple(int(item) for item in transcript.get(
        "generated_token_ids", ()))
    if not input_ids or not generated:
        raise RuntimeError("conversation receipt lacks generation transcript")
    state = state_cache.get((ctx.request_id, stage_index), {})
    # Provider-local parent state stores the canonical prefix.  APPEND_DELTA
    # requests add only their authenticated input suffix and newly generated
    # tokens; the opaque checkpoint itself never substitutes for this state.
    prefix = tuple(input_ids) + generated
    if continuation.mode.value == "append-delta":
        reference = _conversation_projection_metadata(ctx)[1]
        parent_receipt = (receipt_store.get(reference.role_receipt_digest)
                          if reference else None)
        parent_prefix = (prefix_store.get(reference.role_receipt_digest)
                         if reference else None)
        if parent_receipt is None or parent_prefix is None:
            raise ConversationStateUnavailable(
                "conversation parent prefix is not retained by this Provider")
        # Stage 0 expands APPEND_DELTA into a full context before execution.
        # The transcript therefore already contains the committed parent
        # prefix followed by the authenticated suffix.  Do not prepend the
        # parent a second time; doing so makes the receipt digest disagree with
        # the User's canonical token sequence and can only be detected after
        # all Providers have executed.
        parent_count = len(parent_prefix)
        if (len(input_ids) < parent_count
                or tuple(input_ids[:parent_count]) != tuple(parent_prefix)):
            raise ConversationCheckpointInvalid(
                "conversation transcript does not contain committed parent prefix")
        prefix = tuple(input_ids) + generated
    receipt = _conversation_receipt(
        ctx, binding=binding, spec=spec, metadata=metadata,
        transcript=transcript, prefix=prefix, manager=manager,
        state=state, state_reference=None)
    # Keep the token sequence only inside the Provider process.  It is not put
    # in the signed receipt or any NDN payload, but is needed to form the next
    # turn's exact canonical prefix.
    prefix_store[receipt.receipt_digest] = prefix
    manager.put_request_local(
        ctx.request_id, ctx.role, _copy_onnx_state(state),
        logical_bytes=max(1, _conversation_state_bytes(state)))
    manager.stage_request_local_promotion(
        request_id=ctx.request_id, role=ctx.role, receipt=receipt,
        logical_bytes=max(1, _conversation_state_bytes(state)))
    receipt_store[receipt.receipt_digest] = receipt
    ctx.ndnsf.allow_data(_CONVERSATION_STATE_SCOPE, _CONVERSATION_RECEIPT_TOPIC)
    ctx.ndnsf.publish(
        _CONVERSATION_STATE_SCOPE,
        f"{_CONVERSATION_RECEIPT_TOPIC}/{ctx.role.strip('/')}",
        json.dumps(receipt.to_dict(), sort_keys=True, separators=(",", ":")).encode(),
    )
    generation_id = str(metadata.get("generationId", "") or ctx.request_id)
    _conversation_commit_or_rollback(
        ctx, manager=manager, receipt=receipt, binding=binding,
        generation_id=generation_id, attempt_epoch=int(
            metadata.get("selectionAttempt", 1) or 1),
        plan_digest=str(metadata.get("selectionPlanDigest", "")),
    )
    # Advance the small context-expansion index only after the Provider's
    # staged tensor state was atomically promoted by the authenticated COMMIT.
    # The opaque state itself remains owned by ProviderConversationStateManager.
    _qwen_context_cache_after_commit(
        ctx.request, prefix=prefix,
        context_epoch=binding.successor_context_epoch,
    )
    return receipt


def handle_tiny_onnx_stage(ctx: ProviderRuntimeContext, *,
                           stages: int,
                           session_cache: dict[str, object],
                           state_cache: dict[tuple[str, int], dict[str, object]],
                           compute_delay_ms: float,
                           conversation_manager: ProviderConversationStateManager | None = None,
                           conversation_receipts: dict[str, ProviderConversationStateReceiptV1] | None = None,
                           conversation_prefixes: dict[str, tuple[int, ...]] | None = None,
                           spec175_host_tier_after_commit: bool = False,
                           spec175_m13_restart_after_commit: bool = False,
                           spec175_m14_cancel_prefetch: bool = False,
                           spec175_m14_prefetch_cancel_after_ms: int = 50) -> None:
    """Execute the Spec175 tiny fixture through real ORT and stream tokens."""
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    if not model_key:
        raise RuntimeError("tiny ONNX stage requires an immutable model artifact")
    session = session_cache.get(model_key)
    if session is None:
        import onnxruntime as ort
        from ndnsf_distributed_inference.adapters.qwen.stateful_onnx import (
            PersistentStatefulOnnxSession,
            StatefulOnnxIOContractV1,
        )
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        raw_session = ort.InferenceSession(
            model_key, sess_options=options, providers=["CPUExecutionProvider"])
        contract = StatefulOnnxIOContractV1.from_session(raw_session)
        session = PersistentStatefulOnnxSession(raw_session, contract)
        session_cache[model_key] = session
    stage_spec = dict(getattr(ctx.execution.spec, "metadata", {}) or {})
    stage_index = int(stage_spec.get("stageIndex", role_index(ctx.role)))
    full_generation = _qwen_generation_spec(ctx.request)
    if full_generation is not None:
        binding = None
        continuation = None
        parent_receipt = None
        if conversation_manager is not None:
            prefetch_started = time.monotonic()

            def cancellation_check() -> bool:
                if ctx.stream_cancelled():
                    return True
                # M14 uses an explicit Provider-side fault control.  Core's
                # user cancellation is local to the user lifecycle and does
                # not, by itself, signal a remote Provider; this control makes
                # the in-flight HOST->GPU cancellation test deterministic
                # without pretending that a remote cancellation protocol
                # exists.
                return (
                    spec175_m14_cancel_prefetch
                    and ctx.request_id == "/spec175-m14-cancelled"
                    and ((time.monotonic() - prefetch_started) * 1000.0
                         >= spec175_m14_prefetch_cancel_after_ms)
                )

            binding, _reference, continuation, parent_receipt = _conversation_prepare(
                ctx,
                spec=full_generation,
                metadata=stage_spec,
                state_cache=state_cache,
                stage_index=stage_index,
                manager=conversation_manager,
                receipt_store=conversation_receipts
                if conversation_receipts is not None else {},
                cancelled=cancellation_check,
            )
            _emit(
                "LLM_PIPELINE_CONVERSATION_PREPARED",
                f"requestId={ctx.request_id}",
                f"role={ctx.role}",
                f"bound={str(binding is not None).lower()}",
                f"mode={continuation.mode.value if continuation is not None else 'none'}",
                flush=True,
            )
        elif stage_spec.get("conversationTurnBinding") is not None:
            raise RuntimeError("conversation Provider state manager is unavailable")
        try:
            transcript = _handle_qwen_transformer_full_generation(
                ctx,
                model=session,
                stages=int(stage_spec.get("stageCount", stages)),
                stage_index=stage_index,
                compute_delay_ms=compute_delay_ms,
                spec=full_generation,
                stage_runner=lambda payload, delay: run_tiny_onnx_stage(
                    payload,
                    role=ctx.role,
                    stages=int(stage_spec.get("stageCount", stages)),
                    session=session,
                    state_cache=state_cache,
                    request_id=ctx.request_id,
                    compute_delay_ms=delay,
                ),
            )
            _emit(
                "LLM_PIPELINE_CONVERSATION_TRANSCRIPT_READY",
                f"requestId={ctx.request_id}",
                f"role={ctx.role}",
                f"bound={str(binding is not None).lower()}",
                flush=True,
            )
            if binding is not None:
                if conversation_receipts is None or conversation_prefixes is None:
                    raise RuntimeError("conversation receipt store is unavailable")
                _emit(
                    "LLM_PIPELINE_CONVERSATION_FINALIZE_BEGIN",
                    f"requestId={ctx.request_id}",
                    f"role={ctx.role}",
                    flush=True,
                )
                finalized_receipt = _conversation_finalize(
                    ctx, binding=binding, continuation=continuation,
                    metadata=stage_spec, spec=full_generation,
                    transcript=transcript, state_cache=state_cache,
                    stage_index=stage_index, manager=conversation_manager,
                    receipt_store=conversation_receipts,
                    prefix_store=conversation_prefixes,
                )
                if spec175_host_tier_after_commit:
                    hosted = conversation_manager.pause_to_host(finalized_receipt)
                    _emit(
                        "LLM_PIPELINE_CONVERSATION_HOST_PAUSED",
                        f"requestId={ctx.request_id}",
                        f"role={ctx.role}",
                        f"conversationId={finalized_receipt.conversation_id}",
                        f"contextEpoch={finalized_receipt.successor_context_epoch}",
                        f"logicalBytes={hosted.logical_bytes}",
                        flush=True,
                    )
                if spec175_m13_restart_after_commit:
                    restarted_boot = (
                        f"{conversation_manager.provider_boot_id}:restarted")
                    cleared = conversation_manager.invalidate_provider_boot(
                        restarted_boot)
                    _emit(
                        "LLM_PIPELINE_SPEC175_M13_PROVIDER_RESTARTED",
                        f"requestId={ctx.request_id}",
                        f"role={ctx.role}",
                        f"clearedEntries={cleared}",
                        f"providerBootId={restarted_boot}",
                        flush=True,
                    )
                _emit(
                    "LLM_PIPELINE_CONVERSATION_FINALIZED",
                    f"requestId={ctx.request_id}",
                    f"role={ctx.role}",
                    flush=True,
                )
        finally:
            if (parent_receipt is not None and continuation is not None):
                try:
                    conversation_manager.release_for_request(
                        continuation, request_id=ctx.request_id,
                        receipt=parent_receipt)
                except (ConversationStateUnavailable, ConversationCheckpointInvalid):
                    pass
            state_cache.pop((str(ctx.request_id), stage_index), None)
        return
    raise RuntimeError("tiny-onnx provider requires TOKEN_STREAMING or FULL generation metadata")


def main() -> int:
    parser = parse_common_args("Run validation LLM pipeline provider")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--handler-workers", type=int, default=2)
    parser.add_argument("--compute-delay-ms", type=float, default=1.0)
    parser.add_argument(
        "--runtime",
        choices=("fake", TINY_TRANSFORMERS_RUNTIME, TINY_ONNX_RUNTIME,
                 QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME),
        default="fake",
    )
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--transformer-layers", type=int, default=4)
    parser.add_argument(
        "--device",
        default="cpu",
        help="Qwen stage device for the selected backend (cpu, cuda:0, or auto).",
    )
    parser.add_argument(
        "--require-cuda",
        action="store_true",
        help="Fail before readiness unless the Qwen stage is placed on CUDA.",
    )
    parser.add_argument(
        "--require-onnx-runtime",
        action="store_true",
        help="Fail closed unless the deployed Qwen path uses ONNX Runtime.",
    )
    parser.add_argument(
        "--lazy-qwen-load",
        action="store_true",
        help="Load the selected Qwen stage inside the first request and retain it.",
    )
    parser.add_argument("--provider-identity", default="")
    parser.add_argument("--bootstrap-token", default="")
    parser.add_argument("--bootstrap-token-file", default="")
    parser.add_argument("--deployment-control-service", default="")
    parser.add_argument("--deployment-role", default="")
    parser.add_argument("--deployment-revision", default="")
    parser.add_argument("--deployment-artifact-digest", action="append", default=[])
    parser.add_argument("--provider-boot-epoch", default="")
    parser.add_argument("--provider-evidence-private-key", default="")
    parser.add_argument("--selection-dataflow-v2", action="store_true")
    parser.add_argument(
        "--selection-dataflow-v3", action="store_true",
        help="Use observational V3 offers and Selection projections without ACK reservation.",
    )
    parser.add_argument("--selection-gpu-capacity-mib", type=int, default=0)
    parser.add_argument("--selection-offered-gpu-mib", type=int, default=0)
    parser.add_argument("--selection-offer-lease-ms", type=int, default=600000)
    parser.add_argument("--selection-max-prepare-ms", type=int, default=600000)
    parser.add_argument("--selection-model-type", default="")
    parser.add_argument("--selection-residency-ttl-ms", type=int, default=600000)
    parser.add_argument("--selection-wal-path", default="")
    parser.add_argument("--selection-storage-key-file", default="")
    parser.add_argument("--selection-signing-key-file", default="")
    parser.add_argument("--selection-residency-json", default="")
    parser.add_argument("--selection-repo-registration", default="")
    parser.add_argument("--selection-model-cache-dir", default="")
    parser.add_argument(
        "--selection-local-artifact", action="append", default=[],
        help="Explicit immutable preloaded V3 stage binding ROLE=PATH.")
    parser.add_argument("--repo-client-state-root", default="")
    parser.add_argument(
        "--spec175-fault-case",
        choices=("M02", "M03", "M04", "M05", "M09", "M11", "M12", "M13", "M14"),
        default="",
        help="Registered host-MiniNDN stream fault; integration use only.",
    )
    parser.add_argument(
        "--spec175-host-tier-after-commit",
        action="store_true",
        help="M12 control: move each committed conversation state to HOST_RESIDENT.",
    )
    parser.add_argument(
        "--spec175-m13-restart-after-commit",
        action="store_true",
        help=(
            "M13 control: invalidate Provider conversation state after the "
            "first committed turn; test-only."),
    )
    parser.add_argument(
        "--spec175-prefetch-delay-ms",
        type=int,
        default=0,
        help=(
            "Test-only delay inside Provider HOST-to-GPU prefetch; zero in "
            "production."),
    )
    parser.add_argument(
        "--spec175-m14-cancel-prefetch",
        action="store_true",
        help=(
            "M14 test-only Provider fault: cancel the designated child while "
            "its HOST-to-GPU prefetch is in flight."),
    )
    parser.add_argument(
        "--spec175-m14-prefetch-cancel-after-ms",
        type=int,
        default=50,
        help="M14 test-only Provider cancellation delay (default: 50 ms).",
    )
    args = parser.parse_args()
    if args.bootstrap_token and args.bootstrap_token_file:
        raise SystemExit(
            "use either --bootstrap-token or --bootstrap-token-file")
    bootstrap_token = args.bootstrap_token
    if args.bootstrap_token_file:
        bootstrap_token = Path(args.bootstrap_token_file).read_text(
            encoding="utf-8").strip()
        if not bootstrap_token:
            raise SystemExit("provider bootstrap token file is empty")
    if args.selection_repo_registration and not args.repo_client_state_root:
        raise SystemExit(
            "Qwen DistributedRepo fetch requires --repo-client-state-root")
    if args.selection_dataflow_v2 and args.selection_dataflow_v3:
        raise SystemExit("select exactly one Selection Dataflow profile")
    if args.dry_run:
        _emit("LLM_PIPELINE_PROVIDER_DRY_RUN", args.provider_id, args.roles)
        return 0

    _preflight_qwen_runtime(args)
    selection_local_artifacts = _parse_selection_local_artifacts(
        args.selection_local_artifact,
        runtime=args.runtime,
    )

    signer = None
    adapters = None
    if args.deployment_control_service:
        required = (
            args.provider_identity, args.deployment_role,
            args.deployment_revision, args.provider_boot_epoch,
            args.provider_evidence_private_key,
        )
        if not all(required) or not args.deployment_artifact_digest:
            raise SystemExit("deployment control requires identity/role/revision/artifact/boot/key")
        signer = ProviderEvidenceSigner.from_private_pem(
            Path(args.provider_evidence_private_key).read_bytes())
        adapters = RunnerAdapterRegistry()
        adapters.register(EvidenceRunnerAdapter())
    provider = APPProvider.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        provider_id=args.provider_id,
        group=args.group,
        handler_workers=args.handler_workers,
        provider_identity=args.provider_identity,
        bootstrap_token=bootstrap_token,
        adapter_registry=adapters,
        signer=signer,
        signer_key_id=signer.key_id if signer is not None else "",
    )
    _configure_spec175_stream_fault(provider, args.spec175_fault_case)
    if args.deployment_control_service:
        control = ProviderDeploymentControl(
            provider,
            role=args.deployment_role,
            revision=args.deployment_revision,
            artifact_digests=tuple(args.deployment_artifact_digest),
            boot_epoch=args.provider_boot_epoch,
        )
        provider.serve_service(
            service=args.deployment_control_service,
            roles=args.deployment_role,
            handler=control.handle,
            backends=("custom",),
            has_model=False,
            can_provision=False,
            ready_without_model=True,
        )
    qwen_models: dict[str, object] = {}
    qwen_runtime_cache: dict[str, object] = {}
    conversation_manager: ProviderConversationStateManager | None = None
    conversation_receipts: dict[str, ProviderConversationStateReceiptV1] = {}
    conversation_prefixes: dict[str, tuple[int, ...]] = {}
    if args.runtime == TINY_TRANSFORMERS_RUNTIME:
        selection_v2 = {}
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
        if args.selection_dataflow_v2 or args.selection_dataflow_v3:
            # ``transformers`` is the adapter capability; the concrete
            # execution backend records whether this Provider is CUDA-backed
            # or CPU-only.  Advertising both avoids rejecting a role whose
            # adapter requirement is generic while preserving the topology
            # and device policy as the source of GPU truth.
            backends = ["transformers"]
            backends.append("cuda" if args.require_cuda else "transformers-cpu")
        else:
            backends = ["transformers"]
    elif args.runtime == TINY_ONNX_RUNTIME:
        selected_roles = _selected_roles(args.roles, provider)
        sessions: dict[str, object] = {}
        state_cache: dict[tuple[str, int], dict[str, object]] = {}
        try:
            core_boot_epoch = str(provider.provider_boot_epoch)
        except AttributeError:
            core_boot_epoch = str(
                provider._network_provider._provider.provider.provider_boot_epoch)
        provider_identity = str(args.provider_identity or provider.provider)
        if not provider_identity or not core_boot_epoch:
            raise RuntimeError(
                "conversation state requires Provider identity and boot epoch")
        conversation_manager = ProviderConversationStateManager(
            provider_identity=provider_identity,
            provider_boot_id=f"{provider_identity}:{core_boot_epoch}",
            gpu_byte_quota=8 * 1024 * 1024 * 1024,
            host_byte_quota=8 * 1024 * 1024 * 1024,
            prefetch_delay_ms=args.spec175_prefetch_delay_ms,
        )
        handler = lambda ctx: handle_tiny_onnx_stage(
            ctx,
            stages=args.stages,
            session_cache=sessions,
            state_cache=state_cache,
            compute_delay_ms=args.compute_delay_ms,
            conversation_manager=conversation_manager,
            conversation_receipts=conversation_receipts,
            conversation_prefixes=conversation_prefixes,
            spec175_host_tier_after_commit=args.spec175_host_tier_after_commit,
            spec175_m13_restart_after_commit=args.spec175_m13_restart_after_commit,
            spec175_m14_cancel_prefetch=args.spec175_m14_cancel_prefetch,
            spec175_m14_prefetch_cancel_after_ms=(
                args.spec175_m14_prefetch_cancel_after_ms),
        )
        # The adapter requires the generic ``onnxruntime`` capability; the
        # signed V3 offer advertises the concrete CPU variant so the planner
        # can bind a CPU device without pretending it is CUDA-backed.
        backends = ["onnxruntime-cpu"] if args.device == "cpu" else ["onnxruntime"]
        selection_v2 = _selection_v3_for_tiny_onnx(provider, args)
    elif args.runtime == QWEN_TRANSFORMERS_RUNTIME:
        qwen_models = (
            {}
            if args.lazy_qwen_load else
            _preload_qwen_stage_models(
                provider,
                _selected_roles(args.roles, provider),
                device=args.device,
                require_cuda=args.require_cuda,
                local_artifacts=selection_local_artifacts,
            )
        )
        qwen_model_cache_lock = Lock()
        handler = lambda ctx: handle_qwen_transformer_stage(
            ctx,
            stages=args.stages,
            model_cache=qwen_models,
            compute_delay_ms=args.compute_delay_ms,
            device=args.device,
            require_cuda=args.require_cuda,
            model_cache_lock=qwen_model_cache_lock,
        )
        if args.selection_dataflow_v2 or args.selection_dataflow_v3:
            backends = ["transformers"]
            backends.append("cuda" if args.require_cuda else "transformers-cpu")
        else:
            backends = ["transformers"]
        selection_v2 = _selection_v2_for_qwen(
            provider,
            args,
            model_cache=qwen_models,
            model_cache_lock=qwen_model_cache_lock,
            local_artifacts=selection_local_artifacts,
        )
    elif args.runtime == QWEN_ONNX_RUNTIME:
        selected_roles = _selected_roles(args.roles, provider)
        qwen_sessions = _qwen_onnx_sessions_for_startup(
            provider,
            selected_roles,
            device=args.device,
            require_cuda=args.require_cuda,
            local_artifacts=selection_local_artifacts,
            lazy_qwen_load=args.lazy_qwen_load,
        )
        qwen_metadata = _qwen_onnx_metadata_by_path(
            provider, selection_local_artifacts)
        qwen_runtime_cache = {
            path: _QwenOnnxRuntimeHandle(
                session, qwen_metadata.get(path, {}))
            for path, session in qwen_sessions.items()
        }
        for runtime in qwen_runtime_cache.values():
            if _warm_qwen_onnx_runtime(runtime) is not True:
                raise RuntimeError("Qwen ONNX runtime warmup did not complete")
        _finalize_qwen_onnx_profiles(qwen_sessions, qwen_metadata)
        handler = lambda ctx: handle_qwen_onnx_stage(
            ctx,
            stages=args.stages,
            session_cache=qwen_runtime_cache,
            metadata_cache=qwen_metadata,
            compute_delay_ms=args.compute_delay_ms,
            device=args.device,
            require_cuda=args.require_cuda,
        )
        backends = ["onnxruntime"]
        selection_v2 = _selection_v2_for_qwen(
            provider,
            args,
            model_cache=qwen_runtime_cache,
            model_cache_lock=Lock(),
            local_artifacts=selection_local_artifacts,
            runtime_metadata=qwen_metadata,
        )
    else:
        selection_v2 = {}
        handler = lambda ctx: handle_stage(ctx, compute_delay_ms=args.compute_delay_ms)
        backends = ["custom"]
    # V3 ACKs are request-first.  An explicit immutable local stage binding,
    # a late-bound Repo registration, or a preloaded runtime gives this
    # Provider a truthful preparation path even when no exact residency proof
    # is available yet.  Keep the old V2 ``can_provision=False`` behavior
    # disjoint and explicit.
    v3_can_prepare = bool(
        args.selection_dataflow_v3
        and (selection_local_artifacts or args.selection_repo_registration
             or (args.runtime == TINY_ONNX_RUNTIME
                 and bool(sessions))
             or (args.runtime == QWEN_ONNX_RUNTIME
                 and _qwen_onnx_can_prepare(
                     args,
                     runtime_cache=qwen_runtime_cache,
                     local_artifacts=selection_local_artifacts))
             or (args.runtime != QWEN_ONNX_RUNTIME
                 and bool(qwen_models))))
    provider.serve_service(
        service=SERVICE,
        roles=args.roles,
        handler=handler,
        backends=backends,
        has_model=True,
        can_provision=v3_can_prepare,
        local_artifacts=selection_local_artifacts,
        **selection_v2,
    )
    _emit(
        "LLM_PIPELINE_PROVIDER_READY",
        f"provider_id={args.provider_id or '(root)'}",
        f"roles={args.roles}",
        f"runtime={args.runtime}",
        flush=True,
    )
    return provider.run()


if __name__ == "__main__":
    raise SystemExit(main())
