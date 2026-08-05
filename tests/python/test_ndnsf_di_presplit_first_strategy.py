from __future__ import annotations

import unittest

from ndnsf_distributed_inference.app_sdk.contracts import (
    PreSplitCatalogSnapshot,
)
from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.planner.presplit_first import (
    PreSplitFirstStrategy,
    ReusableStateView,
)
from ndnsf_distributed_inference.sdk.placement import (
    ArtifactPreparationMode,
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


def digest(char: str) -> str:
    return "sha256:" + char * 64


MIB = 1024 * 1024


class PreSplitFirstStrategyTest(unittest.TestCase):
    def setUp(self):
        adapter = AdapterDescriptor(
            name="generic-onnx",
            version="1",
            state_digest=digest("a"),
            abi="ndnsf-di-adapter-v1",
            model_formats=("onnx",),
            tasks=("classification",),
            backends=("onnxruntime", "onnxruntime-cpu"),
            precisions=("fp32",),
            input_schema_digest=digest("1"),
            options_schema_digest=digest("2"),
            result_schema_digest=digest("3"),
            graph_schema_digest=digest("4"),
            split_schema_digest=digest("5"),
            state_schema_digest=digest("6"),
            graph_inspectable=True,
            splittable=True,
        )
        self.graph = ModelGraphSnapshot(
            graph_digest=digest("d"),
            adapter=adapter,
            nodes=(
                GraphNodeView("n0", "Encoder"),
                GraphNodeView("n1", "Head"),
            ),
            edges=(
                TensorEdgeView(
                    "hidden", "n0", ("n1",), "float32", (1, 8), 32),
            ),
            topological_order=("n0", "n1"),
            legal_cut_edges=("hidden",),
            model_inputs=(
                TensorContract("input", "float32", (1, 8), 32),),
            model_outputs=(
                TensorContract("output", "float32", (1, 2), 8),),
        )
        self.model = ModelDescriptor(
            model_name="example/model",
            content_digest=digest("b"),
            semantics_digest=digest("c"),
            graph_digest=self.graph.graph_digest,
            model_format="onnx",
            precision="fp32",
            adapter=adapter,
        )

    def candidate(
        self, source=SplitSource.PRE_SPLIT, *,
        unknown=False, marker="7",
    ):
        return SplitCandidate(
            source=source,
            splitter=SplitterDescriptor(
                "onnx-graph", "1", digest("e")),
            model=self.model,
            graph_digest=self.graph.graph_digest,
            execution_plan=RoleExecutionPlan(
                roles=("stage-0", "stage-1"),
                dependencies=(
                    RoleDependency(
                        "stage-0", "stage-1", ("hidden",)),),
                node_roles={"n0": "stage-0", "n1": "stage-1"},
            ),
            fragments_by_role={
                "stage-0": digest("f"),
                "stage-1": digest("0"),
            },
            artifacts_by_role={
                "stage-0": (digest(marker),),
                "stage-1": (digest("8"),),
            },
            requirements_by_role={
                "stage-0": RoleResourceRequirement(
                    ("onnxruntime", "onnxruntime-cpu"), 60 * MIB, 0, 0, 0, 0,
                    safety_margin=1.0),
                "stage-1": RoleResourceRequirement(
                    ("onnxruntime", "onnxruntime-cpu"), 60 * MIB, 0, 0,
                    None if unknown else 0, 0,
                    safety_margin=1.0),
            },
            cross_partition_tensors=("hidden",),
            estimated_costs={"transfer_bytes": 32},
        )

    def shard(self, candidate, role, tier, *, boot="boot-epoch-a",
              semantics=None, partition=None, pin_until=3000,
              backend="onnxruntime"):
        return {
            "artifact_digest": candidate.artifacts_by_role[role][0],
            "model_content_digest": self.model.content_digest,
            "semantics_digest": semantics or self.model.semantics_digest,
            "graph_digest": self.graph.graph_digest,
            "partition_digest": partition or candidate.candidate_digest,
            "backend": backend,
            "precision": "fp32",
            "tier": tier,
            "boot_epoch": boot,
            "cache_epoch": "cache-1",
            "captured_at_ms": 900,
            "expires_at_ms": 4000,
            "pin_until_ms": pin_until,
        }

    def provider(self, name, capacity=120, *, cached=(), state=(),
                 rtt=1.0, wait=0.0, queue=0, bandwidth=1000.0,
                 backends=("onnxruntime",), devices=("cuda:0",)):
        return ProviderPlanningView(
            provider=name,
            service="/inference",
            boot_epoch="boot-epoch-a",
            resource_sequence=1,
            offer_digest=digest("a"),
            evidence_digest=digest("9"),
            expires_at_ms=4000,
            accepted_deadline_ms=3000,
            accepted_roles=("stage-0", "stage-1"),
            backends=tuple(backends),
            devices=tuple(devices),
            usable_gpu_memory_mb=capacity,
            queue_depth=queue,
            estimated_wait_ms=wait,
            rtt_ms=rtt,
            bandwidth_mbps=bandwidth,
            cached_shards=tuple(cached),
            reusable_state=tuple(state),
        )

    def catalog(self, candidate):
        return PreSplitCatalogSnapshot(
            alias="model-fp32",
            manifest_digest=digest("a"),
            model_content_digest=self.model.content_digest,
            semantics_digest=self.model.semantics_digest,
            graph_digest=self.graph.graph_digest,
            candidate_digest=candidate.candidate_digest,
            backend="onnxruntime",
            precision="fp32",
            artifact_data_names={
                role: (f"/repo/{role}",)
                for role in candidate.execution_plan.roles
            },
            status="ACTIVE",
            created_at_ms=500,
        )

    def request(self, candidates, providers, *, catalog=(), constraints=None):
        return PlacementRequest(
            request_id="request-placement",
            attempt=1,
            deadline_ms=2000,
            model_digest=self.model.model_digest,
            graph_digest=self.graph.graph_digest,
            candidate_ids=tuple(
                item.candidate_digest for item in candidates),
            providers=tuple(providers),
            required_roles=candidates[0].execution_plan.roles,
            budget=CandidateBudget(
                max_candidates=8, max_policy_ms=100),
            constraints=constraints or {},
            model=self.model,
            graph=self.graph,
            candidates=tuple(candidates),
            catalog_snapshot=tuple(catalog),
        )

    def strategy(self, domain="tenant-a"):
        return PreSplitFirstStrategy(
            at_ms=1000, security_domain=domain)

    def test_exact_presplit_and_residency_tier_order_are_deterministic(self):
        candidate = self.candidate()
        repository = self.provider("/provider/repository")
        disk = self.provider("/provider/d", cached=(
            self.shard(candidate, "stage-0", "DISK"),
            self.shard(candidate, "stage-1", "DISK"),
        ))
        ram = self.provider("/provider/ram", cached=(
            self.shard(candidate, "stage-0", "HOST_RAM"),
            self.shard(candidate, "stage-1", "HOST_RAM"),
        ))
        reload_safe = self.provider("/provider/reload", cached=(
            self.shard(candidate, "stage-0", "RELOAD_SAFE_GPU"),
            self.shard(candidate, "stage-1", "RELOAD_SAFE_GPU"),
        ))
        pinned = self.provider("/provider/p", cached=(
            self.shard(candidate, "stage-0", "PINNED_GPU"),
            self.shard(candidate, "stage-1", "PINNED_GPU"),
        ))
        request = self.request(
            (candidate,), (repository, disk, ram, reload_safe, pinned,),
            catalog=(self.catalog(candidate),),
        )
        decision = evaluate_placement_strategy(
            self.strategy(), request, replay_deterministic=True)
        self.assertEqual(
            {item.provider for item in decision.assignments},
            {"/provider/p"},
        )
        self.assertEqual(
            decision.evidence["roles"]["stage-0"]["residency_tier"],
            "PINNED_GPU",
        )
        self.assertEqual(
            decision.evidence["split_specification"]["source"],
            "EXACT_PROVIDER_CACHE",
        )
        self.assertIs(
            decision.artifact_preparation,
            ArtifactPreparationMode.REUSE_CACHED,
        )
        self.assertEqual(
            decision.fallback_order["stage-0"],
            ("/provider/p", "/provider/reload", "/provider/ram",
             "/provider/d", "/provider/repository"),
        )

    def test_catalog_artifact_backend_is_not_provider_execution_backend(self):
        candidate = self.candidate()
        cpu = self.provider(
            "/provider/cpu",
            cached=(
                self.shard(
                    candidate, "stage-0", "HOST_RAM",
                    backend="onnxruntime-cpu"),
                self.shard(
                    candidate, "stage-1", "HOST_RAM",
                    backend="onnxruntime-cpu"),
            ),
            backends=("onnxruntime-cpu",),
            devices=("cpu",),
        )
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request(
                (candidate,), (cpu,), catalog=(self.catalog(candidate),)),
        )
        self.assertIs(
            decision.artifact_preparation,
            ArtifactPreparationMode.REUSE_CACHED,
        )
        self.assertEqual(
            {assignment.backend for assignment in decision.assignments},
            {"onnxruntime-cpu"},
        )

    def test_published_candidate_precedes_new_materialization(self):
        published = self.candidate(
            SplitSource.PRE_SPLIT, marker="7")
        generated = self.candidate(
            SplitSource.GENERATED, marker="6")
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request(
                (generated, published),
                (self.provider("/provider/a"),),
                catalog=(self.catalog(published),),
            ),
        )
        self.assertEqual(published.candidate_digest, decision.split_id)
        self.assertEqual(
            "EXACT_PRE_SPLIT",
            decision.evidence["split_specification"]["source"],
        )
        self.assertIs(
            ArtifactPreparationMode.PRE_SPLIT,
            decision.artifact_preparation,
        )

    def test_stale_or_identity_mismatched_cache_is_not_reused(self):
        candidate = self.candidate()
        invalid = self.provider("/provider/a", cached=(
            self.shard(
                candidate, "stage-0", "PINNED_GPU",
                semantics=digest("f")),
            self.shard(
                candidate, "stage-1", "PINNED_GPU",
                boot="other-boot"),
        ))
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request(
                (candidate,), (invalid,),
                catalog=(self.catalog(candidate),),
            ),
        )
        self.assertTrue(all(
            item["residency_tier"] == "REPOSITORY"
            for item in decision.evidence["roles"].values()
        ))

    def test_different_partition_digest_cannot_reuse_cached_shards(self):
        candidate = self.candidate()
        invalid = self.provider("/provider/a", cached=(
            self.shard(
                candidate, "stage-0", "PINNED_GPU",
                partition=digest("e")),
            self.shard(
                candidate, "stage-1", "PINNED_GPU",
                partition=digest("e")),
        ))
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request(
                (candidate,), (invalid,),
                catalog=(self.catalog(candidate),),
            ),
        )
        self.assertTrue(all(
            item["residency_tier"] == "REPOSITORY"
            for item in decision.evidence["roles"].values()
        ))

    def test_provider_cache_without_active_catalog_requires_first_publication(self):
        candidate = self.candidate()
        disk = self.provider("/provider/d", cached=(
            self.shard(candidate, "stage-0", "DISK"),
            self.shard(candidate, "stage-1", "DISK"),
        ))
        decision = evaluate_placement_strategy(
            self.strategy(), self.request((candidate,), (disk,)),
        )
        self.assertEqual(
            decision.evidence["split_specification"]["source"],
            "ACK_CAPACITY_GENERATED",
        )
        self.assertIs(
            decision.artifact_preparation,
            ArtifactPreparationMode.GENERATED,
        )

    def test_provider_cache_with_active_catalog_can_reuse_existing_names(self):
        candidate = self.candidate()
        disk = self.provider("/provider/d", cached=(
            self.shard(candidate, "stage-0", "DISK"),
            self.shard(candidate, "stage-1", "DISK"),
        ))
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request((candidate,), (disk,), catalog=(self.catalog(candidate),)),
        )
        self.assertEqual(
            decision.evidence["split_specification"]["source"],
            "EXACT_PROVIDER_CACHE",
        )
        self.assertIs(
            decision.artifact_preparation,
            ArtifactPreparationMode.REUSE_CACHED,
        )

    def test_closed_ack_capacity_generates_balanced_graph_valid_split(self):
        generated = self.candidate(
            SplitSource.GENERATED, marker="6")
        request = self.request(
            (generated,),
            (
                self.provider("/provider/a", capacity=60),
                self.provider("/provider/b", capacity=60),
            ),
        )
        decision = evaluate_placement_strategy(
            self.strategy(), request, replay_deterministic=True)
        self.assertEqual(
            {item.provider for item in decision.assignments},
            {"/provider/a", "/provider/b"},
        )
        self.assertEqual(
            decision.evidence["split_specification"]["source"],
            "ACK_CAPACITY_GENERATED",
        )
        self.assertIs(
            decision.artifact_preparation,
            ArtifactPreparationMode.GENERATED,
        )
        self.assertEqual(
            sum(decision.evidence["aggregate_gpu_memory_mb"].values()),
            120,
        )

    def test_exact_ack_set_and_score_inputs_are_retained(self):
        candidate = self.candidate()
        cached = (
            self.shard(candidate, "stage-0", "HOST_RAM"),
            self.shard(candidate, "stage-1", "HOST_RAM"),
        )
        busy = self.provider(
            "/provider/busy", cached=cached, wait=12.0, queue=4,
            rtt=20.0, bandwidth=100.0)
        fast = self.provider(
            "/provider/fast", cached=cached, wait=1.0, queue=0,
            rtt=2.0, bandwidth=1000.0)
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request(
                (candidate,), (busy, fast),
                catalog=(self.catalog(candidate),),
            ),
        )
        self.assertEqual(
            [item["provider"] for item in decision.evidence["ack_set"]],
            ["/provider/busy", "/provider/fast"],
        )
        scores = decision.evidence["roles"]["stage-0"]["candidate_scores"]
        self.assertEqual(2, len(scores))
        selected = next(item for item in scores if item["selected"])
        self.assertEqual("/provider/fast", selected["provider"])
        self.assertEqual(1.0, selected["score"]["estimated_wait_ms"])
        self.assertEqual(0, selected["score"]["queue_depth"])
        self.assertEqual(2.0, selected["score"]["rtt_ms"])
        self.assertEqual(1000.0, selected["score"]["bandwidth_mbps"])

    def test_heterogeneous_capacity_uses_cached_small_then_large_provider(self):
        candidate = self.candidate()
        small = self.provider(
            "/provider/small", capacity=60, cached=(
                self.shard(candidate, "stage-0", "PINNED_GPU"),
            ))
        large = self.provider("/provider/large", capacity=180)
        decision = evaluate_placement_strategy(
            self.strategy(),
            self.request(
                (candidate,), (small, large),
                catalog=(self.catalog(candidate),),
            ),
        )
        assignments = {
            item.role: item.provider for item in decision.assignments
        }
        self.assertEqual("/provider/small", assignments["stage-0"])
        self.assertEqual("/provider/large", assignments["stage-1"])
        stage_one_scores = decision.evidence["roles"]["stage-1"][
            "candidate_scores"]
        rejected = next(
            item for item in stage_one_scores
            if item["provider"] == "/provider/small")
        self.assertFalse(rejected["feasible"])
        self.assertEqual("GPU_CAPACITY", rejected["rejection"])

    def test_unknown_runtime_bound_fails_closed(self):
        unknown = self.candidate(
            SplitSource.GENERATED, unknown=True)
        with self.assertRaisesRegex(ValueError, "unknown runtime peak"):
            evaluate_placement_strategy(
                self.strategy(),
                self.request(
                    (unknown,),
                    (self.provider("/provider/a", capacity=1000),),
                ),
            )

    def test_derived_state_is_separate_bounded_cost_evidence(self):
        candidate = self.candidate()
        state = ReusableStateView(
            state_digest=digest("5"),
            state_class="KV_CACHE",
            model_content_digest=self.model.content_digest,
            semantics_digest=self.model.semantics_digest,
            security_domain="tenant-a",
            layer_begin=0,
            layer_end=8,
            boot_epoch="boot-epoch-a",
            cache_epoch="state-cache-1",
            captured_at_ms=900,
            expires_at_ms=4000,
            pin_until_ms=3000,
            estimated_saved_ms=50.0,
        )
        shared_cache = (
            self.shard(candidate, "stage-0", "HOST_RAM"),
            self.shard(candidate, "stage-1", "HOST_RAM"),
        )
        with_state = self.provider(
            "/provider/b", cached=shared_cache, state=(state,))
        without_state = self.provider(
            "/provider/a", cached=shared_cache)
        request = self.request(
            (candidate,), (without_state, with_state),
            catalog=(self.catalog(candidate),),
            constraints={
                "role_layers": {
                    "stage-0": (0, 4),
                    "stage-1": (4, 8),
                },
            },
        )
        decision = evaluate_placement_strategy(
            self.strategy(), request)
        self.assertEqual(
            {item.provider for item in decision.assignments},
            {"/provider/b"},
        )
        self.assertEqual(
            decision.evidence["roles"]["stage-0"]
            ["derived_state_saved_ms"],
            50.0,
        )
        self.assertNotIn("payload", repr(decision.evidence).lower())


if __name__ == "__main__":
    unittest.main()
