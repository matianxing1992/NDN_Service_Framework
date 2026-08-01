#!/usr/bin/env python3
"""User for the validation LLM pipeline distributed inference example."""

from __future__ import annotations

import json
import hashlib
import hmac
import os
import statistics
import threading
import time
from pathlib import Path

from ndnsf_distributed_inference.app_sdk.client import APPClient
from ndnsf_distributed_inference.app_sdk.deployment import APPDeployment
from ndnsf_distributed_inference.app_sdk.provider import ProviderEvidenceVerifier
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    FileRequestEnvelopeKeyProvider, RuntimeJournal,
)
from ndnsf_distributed_inference.app_sdk.status import RequestState, RevisionState
from ndnsf_distributed_inference.ops.cli import definition_from_json
from ndnsf_distributed_inference.adapters.qwen.pilot import (
    BoundedGenerationScheduler,
    GenerationQueueFull,
)
from ndnsf_distributed_inference.adapters.qwen import (
    build_qwen36_27b_three_stage_adapter,
)
from ndnsf_distributed_inference.app_sdk.contracts import PreSplitCatalogSnapshot
from ndnsf_distributed_inference.app_sdk.placement import (
    InferenceTaskRef,
    ModelRef,
)
from ndnsf_distributed_inference.planner.presplit_first import (
    PreSplitFirstStrategy,
)

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
    run_bounded_qwen_generation,
    run_qwen_onnx_stage,
    run_local_pipeline,
    run_local_tiny_transformer_pipeline,
    _decode_native_tensor_bundle,
    _native_tensor_bundle_payload,
)
from deployment_control import (
    CONTROL_SCHEMA, action_from_response, readiness_from_response,
)


def _runtime_journal(args, identity: str, *, request_envelopes: bool = False):
    if args.test_only_allow_ephemeral_app_state:
        return RuntimeJournal.for_test(args.app_state_root, identity)
    key_provider = None
    if request_envelopes:
        if not args.app_envelope_key_file:
            raise RuntimeError(
                "--app-envelope-key-file is required for durable production submit")
        key_provider = FileRequestEnvelopeKeyProvider(
            args.app_envelope_key_file)
    return RuntimeJournal(
        args.app_state_root,
        identity,
        envelope_key_provider=key_provider,
    )


def _deployment_control_request(client, service: str, action: str, revision: str,
                                artifact_digests, args) -> bytes:
    payload = json.dumps({
        "schema": CONTROL_SCHEMA,
        "action": action,
        "revision": revision,
        "artifactDigests": list(artifact_digests),
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")
    result = client.distributed_inference(
        service, payload, deployment_revision=revision,
        dynamic_provisioning=False,
        ack_timeout_ms=args.ack_timeout_ms,
        timeout_ms=args.timeout_ms,
    )
    if not result.status:
        raise RuntimeError(
            f"deployment control {action} failed for {service}: {result.error}")
    return result.payload


def _start_deployment_workflow(client, args):
    trust = json.loads(Path(args.provider_trust_bundle).read_text(encoding="utf-8"))
    verifier = ProviderEvidenceVerifier({
        str(key_id): str(pem).encode("utf-8")
        for key_id, pem in trust["trustedProviderKeys"].items()
    })
    services = tuple(args.deployment_control_service)
    if not services:
        services = tuple(str(item) for item in trust.get("controlServices", ()))
    definition = definition_from_json(args.deployment_definition)
    deployment = APPDeployment(
        _runtime_journal(args, "deployment-operator"),
        readiness_verifier=verifier)
    revision = deployment.resolve(definition)
    if revision.revision != args.deployment_revision:
        raise RuntimeError("deployment workflow revision differs from harness identity")
    artifacts = tuple(item.digest for item in definition.artifacts)
    readiness = tuple(readiness_from_response(_deployment_control_request(
        client, service, "PREPARE", revision.revision, artifacts, args))
        for service in services)
    activations = tuple(action_from_response(_deployment_control_request(
        client, service, "ACTIVATE", revision.revision, artifacts, args))
        for service in services)
    operation = deployment.apply(
        revision, readiness=readiness, activation_receipts=activations)
    if operation.status != "ACTIVE":
        raise RuntimeError(f"deployment apply did not activate: {operation.reason}")
    restarted = APPDeployment(
        _runtime_journal(args, "deployment-operator"),
        readiness_verifier=verifier)
    if restarted.status(definition.deployment_id) != RevisionState.ACTIVE:
        raise RuntimeError("deployment restart did not recover ACTIVE")
    print(
        "LLM_PIPELINE_DEPLOYMENT_ACTIVE "
        f"revision={revision.revision} roles={len(readiness)}",
        flush=True,
    )
    return {
        "deployment": restarted,
        "definition": definition,
        "revision": revision,
        "services": services,
        "artifacts": artifacts,
        "verifier": verifier,
    }


def _finish_deployment_workflow(client, workflow, args) -> None:
    deployment = workflow["deployment"]
    definition = workflow["definition"]
    revision = workflow["revision"]
    services = workflow["services"]
    artifacts = workflow["artifacts"]
    drains = tuple(action_from_response(_deployment_control_request(
        client, service, "DRAIN", revision.revision, artifacts, args))
        for service in services)
    if deployment.drain(
            definition.deployment_id,
            action_receipts=drains).status != RevisionState.INACTIVE.value:
        raise RuntimeError("deployment did not become INACTIVE")

    preview = type(revision).resolve(
        definition, epoch=revision.lifecycle_epoch + 1)
    rollback_readiness = tuple(readiness_from_response(_deployment_control_request(
        client, service, "PREPARE", preview.revision, artifacts, args))
        for service in services)
    rollback_activations = tuple(action_from_response(_deployment_control_request(
        client, service, "ACTIVATE", preview.revision, artifacts, args))
        for service in services)
    rollback = deployment.rollback(
        definition, readiness=rollback_readiness,
        activation_receipts=rollback_activations)
    if rollback.revision != preview.revision or rollback.lifecycle_epoch != 2:
        raise RuntimeError("rollback did not create the fenced epoch-2 revision")

    final_drains = tuple(action_from_response(_deployment_control_request(
        client, service, "DRAIN", preview.revision, artifacts, args))
        for service in services)
    deployment.drain(
        definition.deployment_id, action_receipts=final_drains)
    deletes = tuple(action_from_response(_deployment_control_request(
        client, service, "DELETE", preview.revision, artifacts, args))
        for service in services)
    deployment.delete(
        definition.deployment_id, action_receipts=deletes)
    final = APPDeployment(
        _runtime_journal(args, "deployment-operator"),
        readiness_verifier=workflow["verifier"])
    if final.status(definition.deployment_id) != RevisionState.DELETED:
        raise RuntimeError("deployment restart did not recover DELETED")
    wire_bindings = [record["payload"] for record in client.journal.records()
                     if record["kind"] == "request-wire-binding"]
    if not wire_bindings:
        raise RuntimeError("deployment workflow observed no durable NDNSF wire binding")
    summary = {
        "schema": "ndnsf-di-spec111-minindn-deployment-workflow-v1",
        "status": "PASS",
        "initialRevision": revision.revision,
        "rollbackRevision": rollback.revision,
        "rollbackEpoch": rollback.lifecycle_epoch,
        "terminalState": RevisionState.DELETED.value,
        "providerCount": len(services),
        "wireRequestIds": [item["wireRequestId"] for item in wire_bindings],
    }
    Path(args.deployment_workflow_summary).write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "LLM_PIPELINE_DEPLOYMENT_WORKFLOW_PASS " +
        json.dumps(summary, sort_keys=True, separators=(",", ":")),
        flush=True,
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


def _configure_qwen_automatic_planning(client, args) -> None:
    manifest = json.loads(Path(
        args.automatic_planning_manifest).read_text(encoding="utf-8"))
    stages = tuple(manifest.get("stages", ()))
    if len(stages) != 3:
        raise RuntimeError("automatic planning manifest requires three stages")
    artifact_digests = {
        str(item["role"]): (
            str(item["sha256"])
            if str(item["sha256"]).startswith("sha256:")
            else "sha256:" + str(item["sha256"])
        )
        for item in stages
    }
    weight_bytes = {
        str(item["role"]): int(item["bytes"]) for item in stages
    }
    adapter = build_qwen36_27b_three_stage_adapter(
        artifact_digests_by_role=artifact_digests,
        weight_bytes_by_role=weight_bytes,
    )
    model_doc = dict(manifest["model"])
    model = ModelRef(
        model_name=str(model_doc["name"]),
        content_digest=str(model_doc["contentDigest"]),
        semantics_digest=str(model_doc["semanticsDigest"]),
        source_revision=str(model_doc["revision"]),
    )
    described = adapter.describe_model(
        model.model_name,
        model.content_digest,
        model.semantics_digest,
        source_revision=model.source_revision or "",
    )
    graph = adapter.graph.inspect(described)
    candidate = adapter.splitter.enumerate_candidates(described, graph)[0]
    catalog_doc = dict(manifest["preSplitCatalog"])
    if catalog_doc.get("publicationState") != "ACTIVE":
        raise RuntimeError(
            "automatic planning manifest is not active in DistributedRepo")
    if str(catalog_doc.get("candidateDigest", candidate.candidate_digest)) != (
            candidate.candidate_digest):
        raise RuntimeError("automatic planning candidate digest mismatch")
    snapshot = PreSplitCatalogSnapshot(
        alias=str(catalog_doc["alias"]),
        manifest_digest=str(catalog_doc["manifestDigest"]),
        model_content_digest=model.content_digest,
        semantics_digest=model.semantics_digest,
        graph_digest=graph.graph_digest,
        candidate_digest=candidate.candidate_digest,
        backend="transformers",
        precision="bfloat16",
        artifact_data_names={
            str(item["role"]): (str(item["dataName"]),)
            for item in stages
        },
        status="ACTIVE",
        created_at_ms=int(catalog_doc["createdAtMs"]),
    )
    key_paths = json.loads(Path(
        args.selection_offer_key_map).read_text(encoding="utf-8"))
    offer_keys = {
        str(provider): Path(path).read_bytes()
        for provider, path in key_paths.items()
    }
    if not offer_keys or any(len(value) != 32 for value in offer_keys.values()):
        raise RuntimeError("selection offer verifier requires 32-byte Provider keys")

    def verify_offer(offer) -> bool:
        key = offer_keys.get(offer.provider)
        if key is None:
            return False
        if offer.signer_key_id != (
                "sha256:" + hashlib.sha256(key).hexdigest()):
            return False
        expected = hmac.new(
            key, offer.digest().encode("utf-8"), hashlib.sha256).hexdigest()
        return hmac.compare_digest(expected, offer.signature)

    client.configure_automatic_planning(
        service_name=SERVICE,
        adapters=(adapter,),
        strategy=PreSplitFirstStrategy(
            at_ms=int(time.time() * 1000),
            maximum_cache_age_ms=args.selection_cache_max_age_ms,
            clock_ms=lambda: int(time.time() * 1000),
        ),
        catalog_snapshot_provider=lambda: (snapshot,),
        verify_offer_signature=verify_offer,
        ack_timeout_ms=args.ack_timeout_ms,
    )
    args._automatic_adapter = adapter
    args._automatic_model = model
    args._automatic_task = InferenceTaskRef.from_adapter(adapter)


def _fixed_rate_slot_time(started: float, ordinal: int, interval_s: float,
                          deadline: "Optional[float]") -> "Optional[float]":
    """Return an absolute start slot without accumulating request latency."""
    scheduled = started + max(0, ordinal) * max(0.0, interval_s)
    if deadline is not None and scheduled >= deadline:
        return None
    return scheduled


def _stable_timeline_sample_allows(request_id: str, sample_rate: int) -> bool:
    value = 1469598103934665603
    for byte in request_id.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return sample_rate <= 1 or value % sample_rate == 0


class _Spec107ClientTimingWriter:
    """Thread-safe diagnostic event writer; never records model data."""

    def __init__(self, path: Path, *, candidate_id: str, campaign_id: str,
                 sample_rate: int):
        if not candidate_id.startswith("spec107-c1-") or "spec105" in candidate_id.lower():
            raise ValueError("invalid Spec 107 timing candidate identity")
        if not campaign_id.startswith("spec107-c1-diagnostic-"):
            raise ValueError("Spec 107 timing output requires diagnostic campaign")
        if sample_rate < 1:
            raise ValueError("Spec 107 timing sample rate must be >= 1")
        path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = path.open("x", encoding="utf-8")
        self._candidate_id = candidate_id
        self._campaign_id = campaign_id
        self._sample_rate = sample_rate
        self._lock = threading.Lock()

    def event(self, *, generation_id: str, token_epoch: int, request_id: str,
              component: str, event: str, started_ms: float,
              ended_ms: float, status: str = "COMPLETED") -> None:
        if not _stable_timeline_sample_allows(request_id, self._sample_rate):
            return
        record = {
            "schema": "ndnsf-di-spec107-client-timing-event-v1",
            "candidateId": self._candidate_id,
            "campaignId": self._campaign_id,
            "generationId": generation_id,
            "tokenEpoch": int(token_epoch),
            "requestId": request_id,
            "attemptEpoch": 0,
            "component": component,
            "event": event,
            "startMs": float(started_ms),
            "endMs": float(ended_ms),
            "status": status,
            "sampled": True,
        }
        encoded = json.dumps(record, sort_keys=True, separators=(",", ":"))
        with self._lock:
            self._stream.write(encoded + "\n")
            self._stream.flush()

    def close(self) -> None:
        with self._lock:
            if not self._stream.closed:
                self._stream.close()


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


def _native_step_payload(context_doc: dict, manifest: dict, token_index: int,
                         first_kv_mode: str):
    import numpy as np

    full_ids = np.asarray(context_doc["inputIds"], dtype=np.int64)
    full_mask = np.asarray(context_doc["attentionMask"], dtype=np.int64)
    if token_index == 0:
        ids = full_ids
        positions = np.arange(full_ids.shape[1], dtype=np.int64)[None, :]
    else:
        ids = full_ids[:, -1:]
        positions = np.asarray([[full_ids.shape[1] - 1]], dtype=np.int64)
    tensors = {
        "input_ids": ids,
        "attention_mask": full_mask,
        "position_ids": positions,
    }
    if token_index == 0 and first_kv_mode == "full-context":
        for stage in manifest["stages"]:
            for name in stage["cacheInputs"]:
                shape = stage["tensorContracts"][name]["shape"]
                tensors[name] = np.empty(
                    (ids.shape[0], int(shape[1]), 0, int(shape[3])),
                    dtype=np.float32,
                )
    return _native_tensor_bundle_payload(tensors)


def _native_role_requirements(manifest: dict, logical_session: str,
                              token_index: int, first_kv_mode: str) -> dict[str, bytes]:
    kv_mode = first_kv_mode if token_index == 0 else "cache-hit"
    requirement = (
        f"kvMode={kv_mode};kvSessionId={logical_session};"
        f"kvContextEpoch={token_index};kvNextContextEpoch={token_index + 1};"
        "kvSecurityEpoch=0;"
    ).encode("utf-8")
    return {str(stage["role"]): requirement for stage in manifest["stages"]}


def _run_native_open_loop(client, args, qwen_summary: dict, manifest: dict,
                          expected_tokens: list[int], metrics_file,
                          timing_writer=None) -> int:
    """Submit fixed-rate generations while workers own complete token loops."""
    import numpy as np

    if args.request_interval_ms <= 0:
        raise RuntimeError("open-loop native campaign requires --request-interval-ms")
    if len(expected_tokens) != args.max_new_tokens:
        raise RuntimeError(
            "open-loop expected token count must equal --max-new-tokens")
    if not args.campaign_id or any(
        character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
        for character in args.campaign_id
    ):
        raise RuntimeError(
            "open-loop native campaign requires a safe immutable --campaign-id")
    interval_s = args.request_interval_ms / 1000.0
    offered = max(1, int(args.measured_duration_s / interval_s))
    generation_workers = 4
    condition = threading.Condition()
    completed: list[dict] = []
    states: list[dict] = []
    live_generation_futures = []
    live_network_futures = []
    scheduler = BoundedGenerationScheduler(
        max_workers=generation_workers,
        max_queued=max(0, offered - generation_workers),
    )

    def finish(state: dict, status: str, error: str = "") -> None:
        with condition:
            if state["status"] != "pending":
                return
            state["status"] = status
            state["error"] = error
            state["elapsed_ms"] = (
                time.perf_counter() - state["started"]) * 1000.0
            completed.append(state)
            if metrics_file:
                metrics_file.write(
                    f"measured,{state['index']},{state['elapsed_ms']:.3f},"
                    f"{status},{json.dumps(error)}\n")
                metrics_file.flush()
            condition.notify_all()

    def run_generation(state: dict, report_progress) -> dict:
        try:
            while len(state["generated"]) < args.max_new_tokens:
                token_index = len(state["generated"])
                request_id = f"{state['logical_session']}-token-{token_index}"
                token_started_ms = time.perf_counter() * 1000.0
                previous_done = state.get("last_token_done_ms")
                encode_started_ms = time.perf_counter() * 1000.0
                payload = _native_step_payload(
                    state["context"], manifest, token_index,
                    args.native_first_kv_mode)
                requirements = _native_role_requirements(
                    manifest, state["logical_session"], token_index,
                    args.native_first_kv_mode)
                encode_done_ms = time.perf_counter() * 1000.0
                request_started_ms = time.perf_counter() * 1000.0
                future = client.async_distributed_inference(
                    SERVICE,
                    payload,
                    deployment_revision=args.deployment_revision,
                    dynamic_provisioning=False,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    role_app_requirements=requirements,
                )
                with condition:
                    live_network_futures.append(future)
                result = future.result()
                request_done_ms = time.perf_counter() * 1000.0
                if timing_writer:
                    request_id = result.request_id
                    if not request_id:
                        raise RuntimeError(
                            "Spec 107 diagnostic requires the NDNSF wire request ID")
                    timing_writer.event(
                        generation_id=state["logical_session"],
                        token_epoch=token_index,
                        request_id=request_id,
                        component="inter-token",
                        event="inter_token",
                        started_ms=(previous_done if previous_done is not None
                                    else token_started_ms),
                        ended_ms=token_started_ms,
                    )
                    timing_writer.event(
                        generation_id=state["logical_session"],
                        token_epoch=token_index,
                        request_id=request_id,
                        component="encode-decode",
                        event="request_encode",
                        started_ms=encode_started_ms,
                        ended_ms=encode_done_ms,
                    )
                    timing_writer.event(
                        generation_id=state["logical_session"],
                        token_epoch=token_index,
                        request_id=request_id,
                        component="observed-network",
                        event="network_request",
                        started_ms=request_started_ms,
                        ended_ms=request_done_ms,
                        status="COMPLETED" if result.status else "FAILED",
                    )
                if not result.status:
                    raise RuntimeError(str(result.error))
                decode_started_ms = time.perf_counter() * 1000.0
                response = _decode_native_tensor_bundle(result.payload)
                logits = np.asarray(response["logits"])
                token = int(np.argmax(logits[:, -1, :], axis=-1)[0])
                expected = expected_tokens[token_index]
                if token != expected:
                    raise RuntimeError(f"TOKEN_MISMATCH index={token_index}")
                state["generated"].append(token)
                state["context"]["inputIds"] = [
                    [*row, token] for row in state["context"]["inputIds"]
                ]
                state["context"]["attentionMask"] = [
                    [*row, 1] for row in state["context"]["attentionMask"]
                ]
                decode_done_ms = time.perf_counter() * 1000.0
                if timing_writer:
                    timing_writer.event(
                        generation_id=state["logical_session"],
                        token_epoch=token_index,
                        request_id=request_id,
                        component="encode-decode",
                        event="response_decode",
                        started_ms=decode_started_ms,
                        ended_ms=decode_done_ms,
                    )
                    timing_writer.event(
                        generation_id=state["logical_session"],
                        token_epoch=token_index,
                        request_id=request_id,
                        component="observed-step",
                        event="token_step",
                        started_ms=token_started_ms,
                        ended_ms=decode_done_ms,
                    )
                state["last_token_done_ms"] = decode_done_ms
                report_progress(len(state["generated"]))
            finish(state, "ok")
            return state
        except BaseException as exc:
            finish(state, "failed", str(exc))
            raise

    campaign_started = time.perf_counter()
    for index in range(offered):
        target = campaign_started + index * interval_s
        delay = target - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
        state = {
            "index": index,
            "logical_session": f"{args.request_id}-open-{index}",
            "context": {
                "inputIds": [list(row) for row in qwen_summary["inputIds"]],
                "attentionMask": [
                    list(row) for row in qwen_summary.get(
                        "attentionMask",
                        [[1] * len(qwen_summary["inputIds"][0])],
                    )
                ],
            },
            "generated": [],
            "status": "pending",
            "error": "",
            "started": time.perf_counter(),
        }
        states.append(state)
        try:
            future = scheduler.submit(
                state["logical_session"],
                lambda report_progress, state=state: run_generation(
                    state, report_progress),
            )
            live_generation_futures.append(future)
        except GenerationQueueFull as exc:
            finish(state, "failed", str(exc))
    completion_deadline = (
        campaign_started + args.measured_duration_s + args.timeout_ms / 1000.0 + 5.0)
    with condition:
        while len(completed) < offered:
            remaining = completion_deadline - time.perf_counter()
            if remaining <= 0:
                break
            condition.wait(timeout=min(1.0, remaining))
    ok = [state for state in completed if state["status"] == "ok"]
    failed = [state for state in completed if state["status"] != "ok"]
    unfinished = offered - len(completed)
    latencies = [float(state["elapsed_ms"]) for state in ok]
    scheduler_snapshot = scheduler.snapshot()
    expected_token_digest = hashlib.sha256(json.dumps(
        expected_tokens, separators=(",", ":")).encode("utf-8")).hexdigest()
    print(
        "LLM_PIPELINE_OPEN_LOOP_SUMMARY",
        f"offered={offered}", f"completed={len(ok)}", f"failed={len(failed)}",
        f"unfinished={unfinished}",
        f"completionRate={len(ok) / offered:.6f}",
        f"offeredRps={1.0 / interval_s:.6f}",
        f"p50_ms={_percentile(latencies, 0.50):.2f}",
        f"p95_ms={_percentile(latencies, 0.95):.2f}",
        f"p99_ms={_percentile(latencies, 0.99):.2f}",
        f"campaignId={args.campaign_id}",
        f"generationWorkers={generation_workers}",
        f"activeAtCutoff={scheduler_snapshot.active}",
        f"queuedAtCutoff={scheduler_snapshot.queued}",
        f"maxActiveObserved={scheduler_snapshot.max_active_observed}",
        f"maxQueuedObserved={scheduler_snapshot.max_queued_observed}",
        f"schedulerCompleted={scheduler_snapshot.completed}",
        f"schedulerFailed={scheduler_snapshot.failed}",
        f"expectedTokenCount={len(expected_tokens)}",
        f"expectedTokenDigest={expected_token_digest}",
        flush=True,
    )
    print(
        "LLM_PIPELINE_OPEN_LOOP_PROGRESS",
        "tokenProgress=" + json.dumps(
            scheduler_snapshot.token_progress, sort_keys=True,
            separators=(",", ":")),
        flush=True,
    )
    for future in live_generation_futures:
        future.cancel()
    for future in live_network_futures:
        future.cancel()
    scheduler.shutdown(wait=False)
    client.shutdown(wait=False)
    if len(ok) != offered:
        if metrics_file:
            metrics_file.flush()
        os._exit(2)
    return 0


def _run_qwen_transformer_generation_sample(
    client,
    args,
    *,
    prompt_case: dict,
    generation_id: str,
    decoder,
    require_eos: bool = True,
):
    input_token_ids = tuple(
        int(token) for token in prompt_case["formattedInputIds"])
    expected_token_ids = tuple(
        int(token) for token in prompt_case["referenceGeneratedTokenIds"])
    eos_token_ids = tuple(
        int(token) for token in prompt_case["eosTokenIds"])
    if not expected_token_ids:
        raise ValueError("referenceGeneratedTokenIds must not be empty")
    if len(expected_token_ids) > args.max_new_tokens:
        raise ValueError("reference generation exceeds max_new_tokens")
    if require_eos and expected_token_ids[-1] not in eos_token_ids:
        raise ValueError("reference generation must terminate with EOS")

    def token_step(context, token_epoch, request_id):
        token_step_started = time.perf_counter()
        application_encode_started = token_step_started
        request_payload = encode_qwen_pipeline_context(
            [list(context)],
            attention_mask=[[1] * len(context)],
            request_id=request_id,
            session_id=generation_id,
            context_epoch=token_epoch,
        )
        if getattr(args, "automatic_planning_manifest", ""):
            application_input = args._automatic_adapter.task.encode_input(
                request_payload,
                {
                    "greedy": True,
                    "maxNewTokens": 1,
                    "useCache": False,
                },
            )
            application_encode_ms = (
                time.perf_counter() - application_encode_started
            ) * 1000.0
            client_request_started = time.perf_counter()
            handle = client.request(
                model=args._automatic_model,
                task=args._automatic_task,
                input=application_input,
                timeout_ms=args.timeout_ms,
                request_id=request_id,
            )
            client_request_ms = (
                time.perf_counter() - client_request_started
            ) * 1000.0
            response_started = time.perf_counter()
            response_payload = handle.result(args.timeout_ms)
            response_wait_decode_ms = (
                time.perf_counter() - response_started
            ) * 1000.0
            phase_timings = dict(handle.planning_timings_ms)
            phase_timings.update({
                "application_input_encode_ms": application_encode_ms,
                "client_request_ms": client_request_ms,
                "response_wait_decode_ms": response_wait_decode_ms,
                "token_step_total_ms": (
                    time.perf_counter() - token_step_started
                ) * 1000.0,
            })
            print(
                "LLM_PIPELINE_QWEN_REQUEST_PHASE_TIMING",
                f"requestId={request_id}",
                json.dumps(
                    phase_timings,
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                flush=True,
            )
            result = type("AutomaticInferenceResult", (), {
                "status": True,
                "payload": response_payload,
                "error": "",
                "request_id": handle.collaboration.request_id,
            })()
        else:
            result = client.distributed_inference(
                SERVICE,
                request_payload,
                deployment_revision=args.deployment_revision,
                dynamic_provisioning=False,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
                request_id=request_id,
            )
        if not result.status:
            raise RuntimeError(str(result.error) or "distributed token step failed")
        wire_request_id = str(getattr(result, "request_id", "") or request_id)
        if wire_request_id != request_id:
            raise RuntimeError(
                "distributed token response request ID mismatch: "
                f"expected={request_id} actual={wire_request_id}"
            )
        response = decode_payload(result.payload)
        if "topToken" not in response:
            raise RuntimeError("distributed token response lacks topToken")
        expected_stages = int(getattr(args, "stages", 0) or 0)
        if (
            expected_stages
            and int(response.get("stageCount", 0) or 0) != expected_stages
        ):
            raise RuntimeError(
                "distributed token response stageCount mismatch")
        return {
            "topToken": int(response["topToken"]),
            "wireRequestId": wire_request_id,
            "attempt": 1,
            "planId": args.deployment_revision,
            "modelIdentityDigest": getattr(args, "model_identity_digest", ""),
            "responseSchema": str(response.get("schema", "")),
            "stageCount": int(response.get("stageCount", 0) or 0),
            "layerRanges": response.get("layerRanges", []),
        }

    return run_bounded_qwen_generation(
        input_token_ids=input_token_ids,
        max_new_tokens=args.max_new_tokens,
        eos_token_ids=eos_token_ids,
        generation_id=generation_id,
        token_step=token_step,
        expected_token_ids=expected_token_ids,
        require_eos=require_eos,
        decode=decoder,
    )


def _run_qwen_transformer_generation_campaign(client, args, campaign: dict) -> int:
    if campaign.get("schemaVersion") != "ndnsf-di-qwen-generation-campaign-v1":
        raise RuntimeError("unsupported Qwen generation campaign schema")
    generation = dict(campaign.get("generation", {}))
    repetitions = dict(campaign.get("repetitions", {}))
    if generation.get("strategy") != "greedy":
        raise RuntimeError("Qwen generation campaign must use greedy strategy")
    manifest_max = int(generation.get("maxNewTokens", 0))
    if manifest_max != args.max_new_tokens:
        raise RuntimeError(
            "campaign maxNewTokens differs from --max-new-tokens")
    require_eos = generation.get("requireEos", True)
    if not isinstance(require_eos, bool):
        raise RuntimeError("campaign requireEos must be boolean")
    warmup_count = int(repetitions.get("warmupPerPrompt", 0))
    measured_count = int(repetitions.get("measuredPerPrompt", 0))
    if warmup_count < 0 or measured_count < 1:
        raise RuntimeError(
            "generation campaign requires non-negative warmup and positive measured counts")
    prompts = list(campaign.get("prompts", []))
    if not prompts:
        raise RuntimeError("generation campaign requires at least one prompt")
    prompt_ids = [str(prompt.get("promptId", "")) for prompt in prompts]
    if any(not prompt_id for prompt_id in prompt_ids):
        raise RuntimeError("campaign prompt IDs must not be empty")
    if len(set(prompt_ids)) != len(prompt_ids):
        raise RuntimeError("campaign prompt IDs must be unique")
    safe_characters = frozenset(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
    if any(
        any(character not in safe_characters for character in prompt_id)
        for prompt_id in prompt_ids
    ):
        raise RuntimeError("campaign prompt IDs contain unsafe characters")
    if not args.generation_jsonl:
        raise RuntimeError(
            "--generation-jsonl is required for a generation campaign")
    if not args.qwen_tokenizer_dir:
        raise RuntimeError(
            "--qwen-tokenizer-dir is required for a generation campaign")

    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(
        args.qwen_tokenizer_dir,
        local_files_only=True,
        trust_remote_code=False,
    )
    decoder = lambda values: tokenizer.decode(
        list(values), skip_special_tokens=True)
    output_path = Path(args.generation_jsonl)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    campaign_id = str(campaign.get("campaignId", ""))
    if not campaign_id:
        raise RuntimeError("campaignId must not be empty")
    if any(character not in safe_characters for character in campaign_id):
        raise RuntimeError("campaignId contains unsafe characters")

    with output_path.open("x", encoding="utf-8") as output:
        for prompt_case in prompts:
            prompt_id = str(prompt_case["promptId"])
            phases = (
                [("warmup", index) for index in range(warmup_count)]
                + [("measured", index) for index in range(measured_count)]
            )
            for phase, repetition in phases:
                marker = "w" if phase == "warmup" else "m"
                generation_id = (
                    f"{campaign_id}-{prompt_id}-{marker}{repetition}")
                result = _run_qwen_transformer_generation_sample(
                    client,
                    args,
                    prompt_case=prompt_case,
                    generation_id=generation_id,
                    decoder=decoder,
                    require_eos=require_eos,
                )
                row = {
                    **result.to_dict(),
                    "schemaVersion": "ndnsf-di-qwen-generation-sample-v1",
                    "campaignId": campaign_id,
                    "promptId": prompt_id,
                    "phase": phase,
                    "repetition": repetition,
                    "inputTokenCount": len(
                        prompt_case["formattedInputIds"]),
                    "workloadDigest": getattr(args, "workload_digest", ""),
                    "modelIdentityDigest": getattr(
                        args, "model_identity_digest", ""),
                    "planId": args.deployment_revision,
                }
                output.write(
                    json.dumps(row, sort_keys=True, separators=(",", ":"))
                    + "\n")
                output.flush()
                print(
                    "LLM_PIPELINE_GENERATION_SAMPLE",
                    f"campaignId={campaign_id}",
                    f"promptId={prompt_id}",
                    f"phase={phase}",
                    f"repetition={repetition}",
                    f"generationId={generation_id}",
                    f"status={result.status}",
                    f"stopReason={result.stop_reason}",
                    f"tokenCount={len(result.generated_token_ids)}",
                    f"totalMs={result.total_ms:.3f}",
                    flush=True,
                )
                if result.status != "OK":
                    print(
                        "LLM_PIPELINE_GENERATION_CAMPAIGN_FAIL",
                        f"campaignId={campaign_id}",
                        f"promptId={prompt_id}",
                        f"phase={phase}",
                        f"repetition={repetition}",
                        flush=True,
                    )
                    return 2
    print(
        "LLM_PIPELINE_GENERATION_CAMPAIGN_PASS",
        f"campaignId={campaign_id}",
        f"promptCount={len(prompts)}",
        f"warmupSamples={len(prompts) * warmup_count}",
        f"measuredSamples={len(prompts) * measured_count}",
        flush=True,
    )
    return 0


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
    parser.add_argument("--generation-campaign-manifest", default="")
    parser.add_argument("--generation-jsonl", default="")
    parser.add_argument("--qwen-tokenizer-dir", default="")
    parser.add_argument("--native-cpu-provider", action="store_true")
    parser.add_argument("--qwen-service-manifest", default="")
    parser.add_argument("--max-new-tokens", type=int, default=1)
    parser.add_argument("--expected-token-ids", default="")
    parser.add_argument(
        "--native-first-kv-mode",
        choices=("full-context", "delta-only"),
        default="full-context",
    )
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
    parser.add_argument("--workload-digest", default="")
    parser.add_argument("--model-identity-digest", default="")
    parser.add_argument("--campaign-id", default="")
    parser.add_argument("--spec107-candidate-id", default="")
    parser.add_argument("--spec107-diagnostic-timing-jsonl", default="")
    parser.add_argument("--durable-app-submit", action="store_true")
    parser.add_argument("--deployment-revision", default="")
    parser.add_argument(
        "--app-state-root", default="/tmp/ndnsf-di-app-state")
    parser.add_argument("--app-envelope-key-file", default="")
    parser.add_argument(
        "--test-only-allow-ephemeral-app-state",
        action="store_true",
        help="MiniNDN/unit-only override for volatile APP state and test key",
    )
    parser.add_argument("--deployment-workflow", action="store_true")
    parser.add_argument("--deployment-definition", default="")
    parser.add_argument("--provider-trust-bundle", default="")
    parser.add_argument("--deployment-control-service", action="append", default=[])
    parser.add_argument("--deployment-workflow-summary", default="")
    parser.add_argument(
        "--automatic-planning-manifest",
        default="",
        help="Spec 162 exact model/graph/pre-split manifest for DEFERRED planning.",
    )
    parser.add_argument("--selection-offer-key-map", default="")
    parser.add_argument(
        "--selection-cache-max-age-ms", type=int, default=600000)
    args = parser.parse_args()
    if args.max_new_tokens < 1 or args.max_new_tokens > 64:
        raise SystemExit("--max-new-tokens must be between 1 and 64")
    if args.native_cpu_provider and args.publish_input_reference:
        raise SystemExit("native CPU pilot does not yet accept referenced request bundles")
    if args.durable_app_submit and not args.deployment_revision:
        raise SystemExit("--durable-app-submit requires --deployment-revision")
    if args.deployment_workflow and not all((
            args.deployment_revision, args.deployment_definition,
            args.provider_trust_bundle, args.deployment_workflow_summary)):
        raise SystemExit(
            "--deployment-workflow requires revision/definition/trust/summary")
    if bool(args.automatic_planning_manifest) != bool(
            args.selection_offer_key_map):
        raise SystemExit(
            "automatic planning requires both manifest and offer key map")

    qwen_summary = {}
    generation_campaign = {}
    if args.generation_campaign_manifest:
        generation_campaign = json.loads(
            Path(args.generation_campaign_manifest).read_text(encoding="utf-8"))
        if args.runtime not in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
            raise SystemExit(
                "generation campaigns require a real Qwen runtime "
                "(qwen-transformers or qwen-onnx)")
    if args.runtime in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME) and args.qwen_runtime_summary:
        qwen_summary = json.loads(Path(args.qwen_runtime_summary).read_text(encoding="utf-8"))
    if args.runtime == QWEN_ONNX_RUNTIME:
        # Inter-stage transport is the NDITB001 typed tensor bundle. Legacy NPZ
        # decoding exists only inside the library as a labeled comparison fixture.
        print("LLM_PIPELINE_TENSOR_TRANSPORT typed-tensor-bundle", flush=True)
    if args.runtime in (QWEN_TRANSFORMERS_RUNTIME, QWEN_ONNX_RUNTIME):
        if not qwen_summary and not generation_campaign:
            raise SystemExit(
                "--qwen-runtime-summary or --generation-campaign-manifest "
                "is required for Qwen runtimes")
        first_prompt = (
            list(generation_campaign.get("prompts", [{}]))[0]
            if generation_campaign else {}
        )
        campaign_input_ids = first_prompt.get("formattedInputIds")
        input_ids = (
            [list(campaign_input_ids)]
            if campaign_input_ids is not None else qwen_summary["inputIds"]
        )
        session_id = args.session_id or (
            args.request_id if args.context_input_mode != "full" else ""
        )
        payload = encode_qwen_pipeline_context(
            input_ids,
            attention_mask=(
                [[1] * len(campaign_input_ids)]
                if campaign_input_ids is not None
                else qwen_summary.get("attentionMask")
            ),
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
        first_reference = first_prompt.get(
            "referenceGeneratedTokenIds", [0])
        local = type("LocalResult", (), {
            "payload": json.dumps({
                "schema": "ndnsf-di-qwen-transformer-response-v1",
                "runtime": args.runtime,
                "topToken": int(
                    first_reference[0]
                    if generation_campaign
                    else qwen_summary["expectedTopToken"]),
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
        state_root=args.app_state_root,
        envelope_key_file=(args.app_envelope_key_file or None),
        test_only_allow_ephemeral_state_root=(
            args.test_only_allow_ephemeral_app_state),
    )
    if args.automatic_planning_manifest:
        _configure_qwen_automatic_planning(client, args)
    startup_settle_ms = int(os.environ.get("NDNSF_DI_STARTUP_SETTLE_MS", "0"))
    if startup_settle_ms < 0:
        raise ValueError("NDNSF_DI_STARTUP_SETTLE_MS must be non-negative")
    if startup_settle_ms:
        # APPClient starts the native ServiceUser's Face loop during
        # construction.  Give its producer prefix and first SVS mapping a
        # bounded opportunity to register before the first request is
        # published; this is especially important on a multi-node mesh.
        time.sleep(startup_settle_ms / 1000.0)
    if not args.deployment_revision:
        args.deployment_revision = "sha256:" + hashlib.sha256(
            Path(args.config).read_bytes()).hexdigest()
    deployment_workflow = (
        _start_deployment_workflow(client, args)
        if args.deployment_workflow else None)
    if generation_campaign:
        try:
            return _run_qwen_transformer_generation_campaign(
                client, args, generation_campaign)
        finally:
            client.shutdown(wait=False)
    local_qwen_onnx = None
    if (
        args.runtime == QWEN_ONNX_RUNTIME and
        ((args.native_cpu_provider and args.measured_duration_s <= 0) or
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
    timing_writer = None
    if args.spec107_diagnostic_timing_jsonl:
        timing_writer = _Spec107ClientTimingWriter(
            Path(args.spec107_diagnostic_timing_jsonl),
            candidate_id=args.spec107_candidate_id,
            campaign_id=args.campaign_id,
            sample_rate=max(1, int(os.environ.get(
                "NDNSF_TIMELINE_TRACE_SAMPLE_RATE", "100"))),
        )

    if args.native_cpu_provider and args.measured_duration_s > 0:
        try:
            manifest = json.loads(
                Path(args.qwen_service_manifest).read_text(encoding="utf-8"))
            expected_tokens = _parse_delta_token_ids(args.expected_token_ids)
            return _run_native_open_loop(
                client, args, qwen_summary, manifest, expected_tokens,
                metrics_file, timing_writer)
        finally:
            if metrics_file:
                metrics_file.close()
            if timing_writer:
                timing_writer.close()

    measured_latencies: list[float] = []
    measured_count = 0
    # Start the measured-duration clock only after all warmup requests finish.
    # The Spec 111 matched campaign freezes warmup outside the measured window.
    deadline = None
    warmup_started = time.perf_counter()
    measured_started = None
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
            if phase == "measured" and measured_started is None:
                measured_started = time.perf_counter()
                if args.measured_duration_s > 0:
                    deadline = measured_started + args.measured_duration_s
            if phase == "measured" and args.measured_duration_s <= 0 and measured_count >= args.measured_requests:
                break
            if deadline is None and index >= total_limit:
                break

            if args.request_interval_ms > 0:
                interval_s = args.request_interval_ms / 1000.0
                phase_started = (
                    warmup_started if phase == "warmup" else measured_started)
                ordinal = index if phase == "warmup" else measured_count
                scheduled = _fixed_rate_slot_time(
                    phase_started, ordinal, interval_s, deadline)
                if scheduled is None:
                    break
                delay = scheduled - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                if deadline is not None and time.perf_counter() >= deadline:
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
                    if token_index == 0 and args.native_first_kv_mode == "full-context":
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
                    kv_mode = (
                        args.native_first_kv_mode if token_index == 0 else "cache-hit"
                    )
                    requirement = (
                        f"kvMode={kv_mode};kvSessionId={logical_session};"
                        f"kvContextEpoch={token_index};"
                        f"kvNextContextEpoch={token_index + 1};"
                        "kvSecurityEpoch=0;"
                    ).encode("utf-8")
                    result = client.distributed_inference(
                        SERVICE,
                        native_payload,
                        deployment_revision=args.deployment_revision,
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
                if args.durable_app_submit:
                    handle = client.submit(
                        service=SERVICE,
                        input=request_payload,
                        deployment_revision=args.deployment_revision,
                        deadline=(args.timeout_ms + 10_000) / 1000.0,
                        inference_options={
                            "dynamic_provisioning": False,
                            "ack_timeout_ms": args.ack_timeout_ms,
                            "timeout_ms": args.timeout_ms,
                        },
                    )
                    state = client.wait(
                        handle, timeout_ms=args.timeout_ms + 10_000)
                    if state == RequestState.SUCCEEDED:
                        result = type("DurableInferenceResult", (), {
                            "status": True,
                            "payload": client.result(handle),
                            "error": "",
                            "request_id": handle.request_id,
                        })()
                    else:
                        result = type("DurableInferenceResult", (), {
                            "status": False,
                            "payload": b"",
                            "error": state.value,
                            "request_id": handle.request_id,
                        })()
                    restarted = APPClient(
                        _runtime_journal(
                            args,
                            client.journal.root.name,
                            request_envelopes=True,
                        ),
                        requester_identity=client.requester_identity,
                    )
                    reopened = restarted.open_request(handle.request_id)
                    if (restarted.status(reopened) != state or
                            (state == RequestState.SUCCEEDED and
                             restarted.result(reopened) != result.payload)):
                        raise RuntimeError("durable request reopen mismatch")
                    print(
                        "LLM_PIPELINE_DURABLE_REQUEST "
                        f"requestId={handle.request_id} "
                        f"wireRequestId={result.request_id} "
                        f"state={state.value}",
                        flush=True,
                    )
                else:
                    result = client.distributed_inference(
                        SERVICE,
                        request_payload,
                        deployment_revision=args.deployment_revision,
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
    finally:
        if metrics_file:
            metrics_file.close()
        if timing_writer:
            timing_writer.close()
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
    if deployment_workflow is not None:
        _finish_deployment_workflow(client, deployment_workflow, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
