#!/usr/bin/env python3
"""One real deferred Collaboration invocation with opaque DI V2 assignments."""

from __future__ import annotations

import hashlib
import time

from ndnsf import CollaborationDependency, CollaborationRole, ServiceUser
from ndnsf_distributed_inference.core import (
    DIRequestEnvelopeV2,
    DIRoleAssignmentV2,
    DISelectionAssignmentV2,
)
from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.planner.presplit_first import (
    PreSplitFirstStrategy,
)
from ndnsf_distributed_inference.sdk.placement import (
    DIProviderOfferV2,
    PlacementRequest,
    ProviderPlanningView,
    evaluate_placement_strategy,
)
from ndnsf_distributed_inference.splitter import (
    AdapterDescriptor,
    GraphNodeView,
    ModelDescriptor,
    ModelGraphSnapshot,
    RoleDependency,
    RoleExecutionPlan,
    RoleResourceRequirement,
    SplitCandidate,
    SplitSource,
    SplitterDescriptor,
    TensorContract,
    TensorEdgeView,
)


def digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode()).hexdigest()


def event(marker: str, **fields: object) -> None:
    rendered = " ".join(f"{key}={value}" for key, value in fields.items())
    print(f"{marker} tsNs={time.time_ns()} {rendered}".rstrip(), flush=True)


def build_dynamic_plan(
    offers: dict[str, DIProviderOfferV2],
    *,
    deadline_ms: int,
) -> tuple[object, ModelGraphSnapshot, SplitCandidate]:
    """Run the production default strategy on the immutable closed ACK set."""

    adapter = AdapterDescriptor(
        name="opaque-container", version="1",
        state_digest=digest("opaque-container-adapter"),
        abi="ndnsf-di-adapter-v1", model_formats=("opaque",),
        tasks=("opaque-container",), backends=("fake-byte-runner",),
        precisions=("bytes",),
        input_schema_digest=digest("opaque-input-schema"),
        options_schema_digest=digest("opaque-options-schema"),
        result_schema_digest=digest("opaque-result-schema"),
        graph_schema_digest=digest("opaque-graph-schema"),
        split_schema_digest=digest("opaque-split-schema"),
        state_schema_digest=digest("opaque-state-schema"),
        graph_inspectable=True, splittable=True,
    )
    graph = ModelGraphSnapshot(
        graph_digest=digest("spec163-fake-dag"),
        adapter=adapter,
        nodes=tuple(
            GraphNodeView(f"node-{role}", "OpaqueStage")
            for role in ("source", "left", "right", "merge")
        ),
        edges=(
            TensorEdgeView(
                "fanout-value", "node-source",
                ("node-left", "node-right"), "bytes", (13,), 13),
            TensorEdgeView(
                "left-value", "node-left", ("node-merge",),
                "bytes", (11,), 11),
            TensorEdgeView(
                "right-value", "node-right", ("node-merge",),
                "bytes", (12,), 12),
        ),
        topological_order=(
            "node-source", "node-left", "node-right", "node-merge"),
        legal_cut_edges=("fanout-value", "left-value", "right-value"),
        model_inputs=(TensorContract("input", "bytes", (1,), 1),),
        model_outputs=(TensorContract("output", "bytes", (1,), 1),),
    )
    model = ModelDescriptor(
        model_name="fake/byte-model",
        content_digest=digest("fake-byte-model-v1"),
        semantics_digest=digest("fake-byte-model-semantics-v1"),
        graph_digest=graph.graph_digest,
        model_format="opaque", precision="bytes", adapter=adapter,
    )
    roles = ("source", "left", "right", "merge")
    candidate = SplitCandidate(
        source=SplitSource.GENERATED,
        splitter=SplitterDescriptor(
            "opaque-graph", "1", digest("opaque-splitter-state")),
        model=model,
        graph_digest=graph.graph_digest,
        execution_plan=RoleExecutionPlan(
            roles=roles,
            dependencies=(
                RoleDependency(
                    "source", "left", ("fanout-value",)),
                RoleDependency(
                    "source", "right", ("fanout-value",)),
                RoleDependency("left", "merge", ("left-value",)),
                RoleDependency("right", "merge", ("right-value",)),
            ),
            node_roles={
                f"node-{role}": role for role in roles
            },
        ),
        fragments_by_role={
            role: digest(f"fragment-{role}") for role in roles
        },
        artifacts_by_role={
            role: (digest(f"artifact-{role}"),) for role in roles
        },
        requirements_by_role={
            role: RoleResourceRequirement(
                ("fake-byte-runner",), 512 * 1024 * 1024,
                0, 0, 0, 0, safety_margin=1.0)
            for role in roles
        },
        cross_partition_tensors=(
            "fanout-value", "left-value", "right-value"),
        estimated_costs={"transfer_bytes": 36},
    )
    providers = tuple(
        ProviderPlanningView(
            provider=offer.provider, service=offer.service,
            boot_epoch=offer.boot_epoch,
            resource_sequence=offer.resource_sequence,
            offer_digest=offer.digest(),
            evidence_digest=offer.evidence_digest,
            expires_at_ms=offer.expires_at_ms,
            accepted_deadline_ms=offer.accepted_deadline_ms,
            accepted_roles=offer.accepted_roles,
            backends=offer.backends,
            usable_gpu_memory_mb=offer.offered_gpu_memory_mb,
            queue_depth=offer.queue_depth,
            estimated_wait_ms=offer.estimated_wait_ms,
            rtt_ms=offer.rtt_ms,
            bandwidth_mbps=offer.bandwidth_mbps,
            cached_shards=offer.cached_shards,
            reusable_state=offer.reusable_state,
        )
        for offer in sorted(offers.values(), key=lambda item: item.provider)
    )
    request = PlacementRequest(
        request_id="/spec163-docker", attempt=1,
        deadline_ms=deadline_ms,
        model_digest=model.model_digest,
        graph_digest=graph.graph_digest,
        candidate_ids=(candidate.candidate_digest,),
        providers=providers, required_roles=roles,
        budget=CandidateBudget(max_candidates=8, max_policy_ms=100),
        constraints={}, model=model, graph=graph,
        candidates=(candidate,), catalog_snapshot=(),
    )
    decision = evaluate_placement_strategy(
        PreSplitFirstStrategy(
            at_ms=max(offer.captured_at_ms for offer in offers.values()),
            security_domain="spec163-local"),
        request,
        replay_deterministic=True,
    )
    return decision, graph, candidate


def main() -> int:
    user = ServiceUser()
    now_ms = int(time.time() * 1000)
    deadline_ms = now_ms + 25_000
    # NDNSF V2 appends request-id as exactly one Name component.
    request_id = "spec163-docker"
    request = DIRequestEnvelopeV2(
        invocation_id="invocation-spec163-docker",
        request_id="/spec163-docker", attempt=1, service="/HELLO",
        model_name="fake/byte-model",
        model_identity_hash=digest("fake-byte-model-v1"),
        task_kind="opaque-container",
        input_manifest_digest=digest("fake-input"),
        input_payload_b64="eA==", options_payload_b64="",
        plan_deadline_ms=deadline_ms, security_domain="spec163-local")
    invocation = user.begin_collaboration(
        "/HELLO", request.to_bytes(), mode="DEFERRED",
        ack_timeout_ms=1200, timeout_ms=15_000, request_id=request_id)
    closed = invocation.acks_closed()
    assignment_deadline_ms = closed.request_deadline_us // 1000
    offers = {
        candidate.provider_name: DIProviderOfferV2.from_bytes(
            candidate.payload)
        for candidate in closed.candidates if candidate.status
    }
    expected = {
        "/example/hello/provider/A",
        "/example/hello/provider/B",
        "/example/hello/provider/C",
    }
    if set(offers) != expected:
        raise RuntimeError(
            f"ACK_CLOSED Provider cover mismatch: {sorted(offers)}")
    scope_key_data_names = {}
    repository_started_ns = time.perf_counter_ns()
    for scope, key_byte in (("fanout", b"f"), ("fanin", b"i")):
        published = user.publish_encrypted_large_data(
            "/HELLO", key_byte * 32,
            object_label=f"spec163-{scope}-scope-key")
        if not published.success or not published.encrypted_data_name:
            raise RuntimeError(
                f"failed to publish {scope} scope key: {published.error}")
        scope_key_data_names[scope] = published.encrypted_data_name
    event(
        "SPEC163_SCOPE_KEY_PUBLICATION",
        durationMs=f"{(time.perf_counter_ns() - repository_started_ns) / 1e6:.6f}",
        plaintextBytes=64, objects=2)
    planning_started_ns = time.perf_counter_ns()
    split_started_ns = time.perf_counter_ns()
    manual_role_provider = {
        "source": "/example/hello/provider/A",
        "left": "/example/hello/provider/B",
        "right": "/example/hello/provider/C",
        "merge": "/example/hello/provider/B",
    }
    decision, graph, candidate = build_dynamic_plan(
        offers, deadline_ms=min(
            offer.accepted_deadline_ms for offer in offers.values()))
    role_provider = {
        assignment.role: assignment.provider
        for assignment in decision.assignments
    }
    if role_provider != manual_role_provider:
        raise RuntimeError(
            "automatic plan differs from matched manual plan: "
            f"automatic={role_provider!r} manual={manual_role_provider!r}")
    if (
        decision.evidence["split_specification"]["source"]
        != "ACK_CAPACITY_GENERATED"
    ):
        raise RuntimeError("default strategy did not generate an ACK-bound split")
    provider_roles = {
        provider: tuple(
            role for role, owner in role_provider.items()
            if owner == provider)
        for provider in expected
    }
    plan_digest = decision.digest()
    assignment_payloads = {}
    for provider, roles in provider_roles.items():
        offer = offers[provider]
        tuple_value = tuple(
            DIRoleAssignmentV2(
                role=role, graph_node_id=f"node-{role}",
                layer_start=None, layer_end=None,
                artifact_digest=digest(f"artifact-{role}"),
                dependency_digest=digest(f"dependency-{role}"),
                adapter_id="opaque-container", adapter_version="1",
                required_gpu_mib=512,
                input_grant_digests=(digest(f"grant-{role}"),))
            for role in roles)
        assignment = DISelectionAssignmentV2(
            invocation_id=request.invocation_id, request_id=request.request_id,
            attempt=1, plan_digest=plan_digest, provider=provider,
            provider_boot_epoch=offer.boot_epoch,
            offer_digest=offer.digest(),
            resource_sequence=offer.resource_sequence, roles=tuple_value,
            artifact_set_digest=digest(f"artifacts-{provider}"),
                dependency_graph_digest=graph.graph_digest,
            deadline_ms=assignment_deadline_ms, generation=1)
        if assignment.required_gpu_mib() > offer.offered_gpu_memory_mb:
            raise RuntimeError("assignment exceeds signed ACK GPU envelope")
        wire = assignment.to_bytes()
        for role in roles:
            assignment_payloads[role] = wire
        event(
            "SPEC163_ASSIGNMENT_FITS", provider=provider,
            roles=",".join(roles),
            assignedMiB=assignment.required_gpu_mib(),
            offeredMiB=offer.offered_gpu_memory_mb)
    event(
        "SPEC163_DYNAMIC_SPLIT",
        durationMs=f"{(time.perf_counter_ns() - split_started_ns) / 1e6:.6f}",
        roles=4, providers=3,
        strategy="PreSplitFirstStrategy",
        source=decision.evidence["split_specification"]["source"],
        splitDigest=decision.split_digest,
        matchedManual=True)
    roles = [
        CollaborationRole(role=role, service="/HELLO")
        for role in ("source", "left", "right", "merge")
    ]
    dependencies = [
        CollaborationDependency(
            producers=["source"], consumers=["left", "right"],
            key_scope="fanout", topic_prefix="/activation/fanout"),
        CollaborationDependency(
            producers=["left", "right"], consumers=["merge"],
            key_scope="fanin", topic_prefix="/activation/fanin"),
    ]
    commit_started_ns = time.perf_counter_ns()
    committed = invocation.commit_plan(
        ack_closed_digest=closed.digest, roles=roles,
        key_scopes={
            "fanout": ["source", "left", "right"],
            "fanin": ["left", "right", "merge"]},
        dependencies=dependencies,
        scope_key_data_names=scope_key_data_names,
        role_scopes={
            "source": ["fanout"],
            "left": ["fanout", "fanin"],
            "right": ["fanout", "fanin"],
            "merge": ["fanin"],
        },
        role_provider_assignments=role_provider,
        assignment_payloads_by_role=assignment_payloads)
    event(
        "SPEC163_PLAN_COMMIT", committed=committed,
        ackClosed=closed.digest,
        commitDurationMs=(
            f"{(time.perf_counter_ns() - commit_started_ns) / 1e6:.6f}"),
        planningDurationMs=(
            f"{(time.perf_counter_ns() - planning_started_ns) / 1e6:.6f}"))
    response = invocation.result(15_000)
    if not response.status or response.payload != b"SPEC163_FAKE_INFERENCE_OK":
        raise RuntimeError(
            f"secure fake inference failed: {response.error!r}")
    event(
        "SPEC163_SECURE_DEFERRED_LIFECYCLE_OK",
        request=request_id, response=response.payload.decode())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
