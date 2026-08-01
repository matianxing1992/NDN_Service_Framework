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
            backends=("onnxruntime",),
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
                    ("onnxruntime",), 60 * MIB, 0, 0, 0, 0,
                    safety_margin=1.0),
                "stage-1": RoleResourceRequirement(
                    ("onnxruntime",), 60 * MIB, 0, 0,
                    None if unknown else 0, 0,
                    safety_margin=1.0),
            },
            cross_partition_tensors=("hidden",),
            estimated_costs={"transfer_bytes": 32},
        )

    def shard(self, candidate, role, tier, *, boot="boot-epoch-a",
              semantics=None, pin_until=3000):
        return {
            "artifact_digest": candidate.artifacts_by_role[role][0],
            "model_content_digest": self.model.content_digest,
            "semantics_digest": semantics or self.model.semantics_digest,
            "graph_digest": self.graph.graph_digest,
            "backend": "onnxruntime",
            "precision": "fp32",
            "tier": tier,
            "boot_epoch": boot,
            "cache_epoch": "cache-1",
            "captured_at_ms": 900,
            "expires_at_ms": 4000,
            "pin_until_ms": pin_until,
        }

    def provider(self, name, capacity=120, *, cached=(), state=(),
                 rtt=1.0):
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
            backends=("onnxruntime",),
            usable_gpu_memory_mb=capacity,
            queue_depth=0,
            estimated_wait_ms=0.0,
            rtt_ms=rtt,
            bandwidth_mbps=1000.0,
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
        disk = self.provider("/provider/d", cached=(
            self.shard(candidate, "stage-0", "DISK"),
            self.shard(candidate, "stage-1", "DISK"),
        ))
        pinned = self.provider("/provider/p", cached=(
            self.shard(candidate, "stage-0", "PINNED_GPU"),
            self.shard(candidate, "stage-1", "PINNED_GPU"),
        ))
        request = self.request(
            (candidate,), (disk, pinned,),
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
            "EXACT_PRE_SPLIT",
        )
        self.assertIs(
            decision.artifact_preparation,
            ArtifactPreparationMode.PRE_SPLIT,
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
