#!/usr/bin/env python3
"""Provider for the validation LLM pipeline distributed inference example."""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import time
from pathlib import Path
from threading import Lock

from ndnsf import parse_large_data_reference_payload
from ndnsf_distributed_inference.app_sdk.provider import (
    APPProvider, ProviderEvidenceSigner,
)
from ndnsf_distributed_inference.core.contracts import (
    DIRequestEnvelopeV2,
    DISelectionAssignmentV2,
)
from ndnsf_distributed_inference.core.deployment_control import (
    DISelectionParticipant,
    GpuMiBAdmissionLedger,
    SelectionPreparationCallbacks,
)
from ndnsf_distributed_inference.provider import (
    DIProviderOfferIssuer, ProviderRuntimeContext,
)
from ndnsf_distributed_inference.sdk.adapters import RunnerAdapterRegistry

from deployment_control import (
    EvidenceRunnerAdapter, ProviderDeploymentControl,
)

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
        print(
            "LLM_PIPELINE_TRANSFORMER_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
            f"path={path}",
            flush=True,
        )
    return cache


def _preload_qwen_stage_models(provider: APPProvider, roles: set[str], *,
                               device: str = "cpu",
                               require_cuda: bool = False) -> dict[str, object]:
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
        cache[path] = qwen_transformer_model_from_stage_package(
            path,
            device=device,
            require_cuda=require_cuda,
        )
        execution_device = str(
            getattr(cache[path], "ndnsf_execution_device", "unknown"))
        cpu_fallback = bool(
            getattr(cache[path], "ndnsf_cpu_fallback", True))
        print(
            "LLM_PIPELINE_QWEN_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
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
        return ort.InferenceSession(
            path,
            sess_options=options,
            providers=[(
                "CUDAExecutionProvider",
                {"device_id": device_id},
            ), "CPUExecutionProvider"],
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
                                 require_cuda: bool = False) -> dict[str, object]:
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
        cache[path] = _qwen_onnx_session(
            path, device=device, require_cuda=require_cuda)
        execution_device, cpu_fallback = _qwen_onnx_session_placement(
            cache[path])
        print(
            "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY",
            f"role={artifact.role}",
            f"path={path}",
            f"device={execution_device}",
            f"cpuFallback={str(cpu_fallback).lower()}",
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


def _artifact_path_by_role(provider: APPProvider) -> dict[str, str]:
    return {
        artifact.role: artifact.path
        for artifact in provider.deployment.service_policy(SERVICE).artifacts
        if artifact.path
    }


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


def _selection_v2_for_qwen(
    provider: APPProvider,
    args,
    *,
    model_cache: dict[str, object],
    model_cache_lock: Lock,
) -> dict:
    if not args.selection_dataflow_v2:
        return {}
    required = (
        args.provider_identity,
        args.selection_wal_path,
        args.selection_storage_key_file,
        args.selection_signing_key_file,
        args.selection_residency_json,
    )
    if (not all(required) or args.selection_gpu_capacity_mib <= 0
            or args.selection_offered_gpu_mib <= 0):
        raise RuntimeError("Selection Dataflow V2 configuration is incomplete")
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
    artifact_paths = _artifact_path_by_role(provider)
    repo_registration_path = str(
        getattr(args, "selection_repo_registration", "") or "")
    repo_registration_by_role = {}
    if repo_registration_path:
        registration = json.loads(
            Path(repo_registration_path).read_text(encoding="utf-8"))
        if registration.get("schemaVersion") != (
                "ndnsf-di-qwen36-repo-registration-v1"):
            raise RuntimeError("unsupported Qwen DistributedRepo registration")
        repo_registration_by_role = {
            str(item["role"]): item
            for item in registration.get("artifacts", [])
        }
        repo_receipts = tuple(
            dict(receipt)
            for item in registration.get("artifacts", [])
            for receipt in item.get("receipts", [])
        )
        if not getattr(args, "selection_model_cache_dir", ""):
            raise RuntimeError(
                "Qwen DistributedRepo fetch requires --selection-model-cache-dir")
    else:
        repo_receipts = ()
    repo_holder: dict[str, object] = {}
    verified_disk_paths: set[str] = set()
    ledger = GpuMiBAdmissionLedger(
        provider=args.provider_identity,
        boot_epoch=core_boot_epoch,
        capacity_mib=args.selection_gpu_capacity_mib,
    )
    issuer = DIProviderOfferIssuer(
        provider=args.provider_identity,
        service=SERVICE,
        boot_epoch=core_boot_epoch,
        ledger=ledger,
        offered_gpu_memory_mb=args.selection_offered_gpu_mib,
        signer_key_id="sha256:" + hashlib.sha256(signing_key).hexdigest(),
        sign_offer_digest=lambda digest: hmac.new(
            signing_key, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
        offer_lease_ms=args.selection_offer_lease_ms,
        max_pending_state_ttl_ms=args.selection_max_prepare_ms,
    )
    issue_offer = issuer.issue

    def issue_offer_with_ttl_diagnostics(*issue_args, **issue_kwargs):
        decision = issue_offer(*issue_args, **issue_kwargs)
        print(
            "LLM_PIPELINE_SELECTION_ACK_TTL",
            f"pendingStateTtlMs={decision.pending_state_ttl_ms}",
            f"providerLimitMs={issuer.max_pending_state_ttl_ms}",
            flush=True,
        )
        return decision

    issuer.issue = issue_offer_with_ttl_diagnostics

    def prepare_role(context) -> None:
        role = context.role.role
        registration_item = repo_registration_by_role.get(role)
        assignment_key = (
            str(registration_item["objectName"])
            if registration_item is not None else ""
        )
        fetch_ms = 0.0
        disk_cache_hit = False
        with model_cache_lock:
            Path(args.selection_model_cache_dir).mkdir(
                parents=True, exist_ok=True)
            model_path = artifact_paths.get(role, "")
            if registration_item is not None:
                digest = str(registration_item["fileSha256"])
                digest_hex = digest[7:] if digest.startswith("sha256:") else digest
                model_path = str(
                    Path(args.selection_model_cache_dir)
                    / f"{digest_hex}.qwen-transformers.pt")
                destination = Path(model_path)
                disk_cache_hit = model_path in verified_disk_paths
                if not disk_cache_hit and destination.is_file():
                    disk_cache_hit = (
                        destination.stat().st_size
                        == int(registration_item["fileBytes"])
                        and _sha256_file(destination) == digest_hex
                    )
                    if disk_cache_hit:
                        verified_disk_paths.add(model_path)
                if not disk_cache_hit:
                    if destination.exists():
                        raise RuntimeError(
                            "Qwen repo cache path exists with invalid content")
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
                    repo_holder["artifact_api"].fetch_file(
                        reference,
                        destination,
                        timeout_ms=max(
                            int(args.selection_max_prepare_ms), 600000),
                    )
                    fetch_ms = _elapsed_ms(fetch_start)
                    verified_disk_paths.add(model_path)
                print(
                    "LLM_PIPELINE_QWEN_REPO_FETCH",
                    f"requestId={context.request_id}",
                    f"role={role}",
                    f"objectName={registration_item['objectName']}",
                    f"cacheHit={str(disk_cache_hit).lower()}",
                    f"fetch_ms={fetch_ms:.2f}",
                    f"bytes={registration_item['fileBytes']}",
                    flush=True,
                )
            if not model_path:
                raise RuntimeError(
                    f"selected Qwen role has no resolved artifact: {role}")
            cache_hit = (
                assignment_key in model_cache or model_path in model_cache)
            load_start = time.perf_counter()
            if not cache_hit:
                model_cache[model_path] = qwen_transformer_model_from_stage_package(
                    model_path,
                    device=args.device,
                    require_cuda=args.require_cuda,
                )
            model = model_cache.get(assignment_key) or model_cache[model_path]
            model_cache[model_path] = model
            model_cache[assignment_key] = model
        print(
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

    def prepare_role_with_diagnostics(context) -> None:
        try:
            prepare_role(context)
        except Exception as exc:  # noqa: BLE001
            print(
                "LLM_PIPELINE_QWEN_SELECTION_PREPARE_FAILED",
                f"requestId={context.request_id}",
                f"role={context.role.role}",
                f"errorType={type(exc).__name__}",
                f"error={exc}",
                flush=True,
            )
            raise

    participant = DISelectionParticipant(
        provider=args.provider_identity,
        boot_epoch=core_boot_epoch,
        ledger=ledger,
        offer_lookup=issuer.lookup,
        callbacks=SelectionPreparationCallbacks(
            prepare_role=prepare_role_with_diagnostics,
            start_role=lambda role: print(
                "LLM_PIPELINE_QWEN_SELECTION_ROLE_START",
                f"role={role}", flush=True),
            release_role=lambda role, reason: print(
                "LLM_PIPELINE_QWEN_SELECTION_ROLE_RELEASE",
                f"role={role}", f"reason={reason}", flush=True),
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
            suffix = ",".join(mismatches) if mismatches else "unclassified"
            print(
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

    def cached_shards():
        role = str(residency_template.get("role", ""))
        model_path = artifact_paths.get(role, "")
        assignment_key = ""
        registration_item = repo_registration_by_role.get(role)
        if registration_item is not None:
            digest = str(registration_item["fileSha256"])
            digest_hex = digest[7:] if digest.startswith("sha256:") else digest
            model_path = str(
                Path(args.selection_model_cache_dir)
                / f"{digest_hex}.qwen-transformers.pt")
            assignment_key = str(registration_item["objectName"])
            if (assignment_key not in model_cache
                    and model_path not in model_cache
                    and not Path(model_path).is_file()):
                return ()
        now_ms = int(time.time() * 1000)
        value = dict(residency_template)
        value.update({
            "boot_epoch": core_boot_epoch,
            "cache_epoch": 1,
            "captured_at_ms": now_ms,
            "expires_at_ms": now_ms + args.selection_residency_ttl_ms,
            "pin_until_ms": now_ms + args.selection_residency_ttl_ms,
            "tier": (
                "RELOAD_SAFE_GPU"
                if (model_path and model_path in model_cache)
                or (assignment_key and assignment_key in model_cache)
                else "DISK"
            ),
        })
        return (value,)

    return {
        "selection_offer_issuer": issuer,
        "selection_participant": participant,
        "selection_wal_path": args.selection_wal_path,
        "selection_storage_key": storage_key,
        "selection_storage_key_epoch": core_boot_epoch,
        "selection_max_prepare_ms": args.selection_max_prepare_ms,
        "selection_cached_shards": cached_shards,
    }


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
    line = "LLM_PIPELINE_QWEN_STAGE_TIMING " + " ".join(
        f"{key}={value}" for key, value in normalized.items()) + "\n"
    # The native runtime and Python provider share stdout.  One write syscall
    # prevents native log records from being inserted between the marker and
    # its fields, which would corrupt request-lineage evidence.
    os.write(1, line.encode("utf-8"))


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
                                  compute_delay_ms: float,
                                  device: str = "cpu",
                                  require_cuda: bool = False,
                                  model_cache_lock=None) -> None:
    total_start = time.perf_counter()
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    model = model_cache.get(model_key)
    cache_hit = model is not None
    model_load_ms = 0.0
    if model is None:
        if not model_key:
            raise RuntimeError("Qwen stage execution requires a model artifact path")
        lock = model_cache_lock or Lock()
        with lock:
            model = model_cache.get(model_key)
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
                print(
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
        print(
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
        print(
            "LLM_PIPELINE_QWEN_STAGE_FINAL",
            f"role={ctx.role}",
            f"bytes={len(output)}",
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
                           compute_delay_ms: float,
                           device: str = "cpu",
                           require_cuda: bool = False) -> None:
    total_start = time.perf_counter()
    artifact_paths = getattr(ctx.execution, "artifact_paths", {}) or {}
    model_key = str(artifact_paths.get("model") or "")
    session = session_cache.get(model_key)
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
    parser.add_argument(
        "--device",
        default="cpu",
        help="Qwen transformers stage device (cpu, cuda:0, or auto).",
    )
    parser.add_argument(
        "--require-cuda",
        action="store_true",
        help="Fail before readiness unless the Qwen stage is placed on CUDA.",
    )
    parser.add_argument(
        "--lazy-qwen-load",
        action="store_true",
        help="Load the selected Qwen stage inside the first request and retain it.",
    )
    parser.add_argument("--provider-identity", default="")
    parser.add_argument("--bootstrap-token", default="")
    parser.add_argument("--deployment-control-service", default="")
    parser.add_argument("--deployment-role", default="")
    parser.add_argument("--deployment-revision", default="")
    parser.add_argument("--deployment-artifact-digest", action="append", default=[])
    parser.add_argument("--provider-boot-epoch", default="")
    parser.add_argument("--provider-evidence-private-key", default="")
    parser.add_argument("--selection-dataflow-v2", action="store_true")
    parser.add_argument("--selection-gpu-capacity-mib", type=int, default=0)
    parser.add_argument("--selection-offered-gpu-mib", type=int, default=0)
    parser.add_argument("--selection-offer-lease-ms", type=int, default=120000)
    parser.add_argument("--selection-max-prepare-ms", type=int, default=600000)
    parser.add_argument("--selection-residency-ttl-ms", type=int, default=600000)
    parser.add_argument("--selection-wal-path", default="")
    parser.add_argument("--selection-storage-key-file", default="")
    parser.add_argument("--selection-signing-key-file", default="")
    parser.add_argument("--selection-residency-json", default="")
    parser.add_argument("--selection-repo-registration", default="")
    parser.add_argument("--selection-model-cache-dir", default="")
    parser.add_argument("--repo-client-state-root", default="")
    args = parser.parse_args()
    if args.selection_repo_registration and not args.repo_client_state_root:
        raise SystemExit(
            "Qwen DistributedRepo fetch requires --repo-client-state-root")
    if args.dry_run:
        print("LLM_PIPELINE_PROVIDER_DRY_RUN", args.provider_id, args.roles)
        return 0

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
        bootstrap_token=args.bootstrap_token,
        adapter_registry=adapters,
        signer=signer,
        signer_key_id=signer.key_id if signer is not None else "",
    )
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
        backends = ["transformers"]
    elif args.runtime == QWEN_TRANSFORMERS_RUNTIME:
        qwen_models = (
            {}
            if args.lazy_qwen_load else
            _preload_qwen_stage_models(
                provider,
                _selected_roles(args.roles, provider),
                device=args.device,
                require_cuda=args.require_cuda,
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
        backends = ["transformers"]
        selection_v2 = _selection_v2_for_qwen(
            provider,
            args,
            model_cache=qwen_models,
            model_cache_lock=qwen_model_cache_lock,
        )
    elif args.runtime == QWEN_ONNX_RUNTIME:
        selection_v2 = {}
        selected_roles = _selected_roles(args.roles, provider)
        qwen_sessions = _preload_qwen_onnx_sessions(
            provider,
            selected_roles,
            device=args.device,
            require_cuda=args.require_cuda,
        )
        qwen_metadata = _qwen_onnx_metadata_by_path(provider)
        handler = lambda ctx: handle_qwen_onnx_stage(
            ctx,
            stages=args.stages,
            session_cache=qwen_sessions,
            metadata_cache=qwen_metadata,
            compute_delay_ms=args.compute_delay_ms,
            device=args.device,
            require_cuda=args.require_cuda,
        )
        backends = ["onnxruntime"]
    else:
        selection_v2 = {}
        handler = lambda ctx: handle_stage(ctx, compute_delay_ms=args.compute_delay_ms)
        backends = ["custom"]
    provider.serve_service(
        service=SERVICE,
        roles=args.roles,
        handler=handler,
        backends=backends,
        has_model=True,
        can_provision=False,
        **selection_v2,
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
