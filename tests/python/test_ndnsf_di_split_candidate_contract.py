from __future__ import annotations

import unittest

from ndnsf_distributed_inference.adapters import build_object_detection_adapter
from ndnsf_distributed_inference.app_sdk.placement import AutomaticPlanningCoordinator
from ndnsf_distributed_inference.adapters.onnx.graph import (
    OnnxGraphSummary,
    OnnxNodeInfo,
    OnnxTensorInfo,
    estimate_split_candidates,
    to_model_graph_snapshot,
    to_split_candidate,
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
    seal_hybrid_plan,
)
from ndnsf_distributed_inference.core import RedistributionEdge


def digest(char: str) -> str:
    return "sha256:" + char * 64


class SplitCandidateContractTest(unittest.TestCase):
    def setUp(self):
        self.adapter = AdapterDescriptor(
            name="onnx-family",
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
        self.model = ModelDescriptor(
            model_name="example/model",
            content_digest=digest("b"),
            semantics_digest=digest("c"),
            graph_digest=digest("d"),
            model_format="onnx",
            precision="fp32",
            adapter=self.adapter,
        )
        self.graph = ModelGraphSnapshot(
            graph_digest=digest("d"),
            adapter=self.adapter,
            nodes=(
                GraphNodeView("n0", "Input"),
                GraphNodeView("n1", "Encoder"),
                GraphNodeView("n2", "Head"),
            ),
            edges=(
                TensorEdgeView(
                    "e0", "n0", ("n1",), "float32", (1, 8), 32),
                TensorEdgeView(
                    "e1", "n1", ("n2",), "float32", (1, "N"), None),
            ),
            topological_order=("n0", "n1", "n2"),
            legal_cut_edges=("e1",),
            model_inputs=(TensorContract("input", "float32", (1, 8), 32),),
            model_outputs=(TensorContract("output", "float32", (1, 2), 8),),
        )

    def test_common_candidate_binds_graph_model_splitter_and_unknown_tensor(self):
        plan = RoleExecutionPlan(
            roles=("stage-0", "stage-1"),
            dependencies=(
                RoleDependency("stage-0", "stage-1", ("e1",)),
            ),
            node_roles={
                "n0": "stage-0",
                "n1": "stage-0",
                "n2": "stage-1",
            },
        )
        candidate = SplitCandidate(
            source=SplitSource.GENERATED,
            splitter=SplitterDescriptor(
                "onnx-graph", "1", digest("e"), deterministic=True),
            model=self.model,
            graph_digest=self.graph.graph_digest,
            execution_plan=plan,
            fragments_by_role={
                "stage-0": digest("f"),
                "stage-1": digest("0"),
            },
            artifacts_by_role={
                "stage-0": (digest("7"),),
                "stage-1": (digest("8"),),
            },
            requirements_by_role={
                "stage-0": RoleResourceRequirement(
                    ("onnxruntime",), 1024, 128, 64, 128, 64),
                "stage-1": RoleResourceRequirement(
                    ("onnxruntime",), 1024, 128, 64, None, 64),
            },
            cross_partition_tensors=("e1",),
            estimated_costs={"transfer_bytes": None},
        )

        candidate.validate_against(self.graph)
        self.assertTrue(candidate.candidate_digest.startswith("sha256:"))
        self.assertIsNone(
            candidate.requirements_by_role["stage-1"].activation_bytes)
        self.assertEqual(candidate.cross_partition_tensors, ("e1",))
        with self.assertRaises(TypeError):
            candidate.fragments_by_role["stage-0"] = digest("9")

    def test_cycle_and_graph_binding_mismatch_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "acyclic"):
            RoleExecutionPlan(
                roles=("a", "b"),
                dependencies=(
                    RoleDependency("a", "b", ("e0",)),
                    RoleDependency("b", "a", ("e1",)),
                ),
                node_roles={"n0": "a", "n1": "b", "n2": "b"},
            )

        with self.assertRaisesRegex(ValueError, "graph digest"):
            ModelDescriptor(
                model_name="bad/model",
                content_digest=digest("b"),
                semantics_digest=digest("c"),
                graph_digest=digest("9"),
                model_format="onnx",
                precision="fp32",
                adapter=self.adapter,
            ).validate_graph(self.graph)

    def test_hybrid_candidate_expands_every_rank_for_v3_selection(self):
        plan = RoleExecutionPlan(
            roles=("stage-0", "stage-1"),
            dependencies=(RoleDependency("stage-0", "stage-1", ("e1",)),),
            node_roles={"n0": "stage-0", "n1": "stage-0", "n2": "stage-1"},
        )
        candidate = SplitCandidate(
            source=SplitSource.GENERATED,
            splitter=SplitterDescriptor("hybrid", "1", digest("e")),
            model=self.model,
            graph_digest=self.graph.graph_digest,
            execution_plan=plan,
            fragments_by_role={"stage-0": digest("f"), "stage-1": digest("0")},
            artifacts_by_role={
                "stage-0": (digest("7"),),
                "stage-1": (digest("8"), digest("9")),
            },
            requirements_by_role={
                role: RoleResourceRequirement(
                    ("onnxruntime",), 1024, 128, 64, 128, 64)
                for role in plan.roles
            },
            cross_partition_tensors=("e1",),
            estimated_costs={"transfer_bytes": None},
            tensor_degrees_by_role={"stage-0": 1, "stage-1": 2},
            rank_artifact_digests_by_role={
                "stage-0": (digest("7"),),
                "stage-1": (digest("8"), digest("9")),
            },
            hybrid_plan=seal_hybrid_plan(
                tensor_degrees=(1, 2),
                redistributions=(RedistributionEdge(
                    producer_ranks=(0,), consumer_ranks=(1, 2),
                    tensor="e1", operation="SCATTER", epoch="epoch-1",
                    integrity_digest=digest("a"),
                    source_layout_digest=digest("b"),
                    target_layout_digest=digest("c"),
                    temporary_memory_bytes=4096),),
            ),
        )
        candidate.validate_against(self.graph)
        specs = AutomaticPlanningCoordinator._v3_role_specs(candidate)
        self.assertEqual(
            tuple((item.role, item.rank, item.artifact_digest) for item in specs),
            (("stage-0", 0, digest("7")),
             ("stage-1", 0, digest("8")),
             ("stage-1", 1, digest("9"))),
        )
        self.assertEqual(
            tuple(item.role_kind for item in specs),
            ("PIPELINE_RANGE", "HYBRID_RANK", "HYBRID_RANK"),
        )

    def test_onnx_dependency_graph_keeps_unknown_tensor_size_explicit(self):
        adapter = build_object_detection_adapter()
        summary = OnnxGraphSummary(
            model_path="fixture.onnx",
            inputs=("input",),
            outputs=("result",),
            initializers=(),
            tensors={
                "input": OnnxTensorInfo("input", "float32", (1, 4), 16),
                "hidden": OnnxTensorInfo(
                    "hidden", "float32", (1, "dynamic"), None,
                ),
                "result": OnnxTensorInfo("result", "float32", (1, 2), 8),
            },
            nodes=(
                OnnxNodeInfo(0, "feature", "Conv", ("input",), ("hidden",)),
                OnnxNodeInfo(1, "head", "Gemm", ("hidden",), ("result",)),
            ),
            tensor_producers={"hidden": 0, "result": 1},
            tensor_consumers={"input": (0,), "hidden": (1,)},
        )
        graph = to_model_graph_snapshot(summary, adapter.descriptor)
        model = ModelDescriptor(
            model_name="detector",
            content_digest=digest("b"),
            semantics_digest=digest("c"),
            graph_digest=graph.graph_digest,
            model_format="onnx",
            precision="float32",
            adapter=adapter.descriptor,
        )
        common = to_split_candidate(
            summary=summary,
            candidate=estimate_split_candidates(summary)[0],
            model=model,
            graph=graph,
            splitter=SplitterDescriptor("onnx-dag", "1", digest("e")),
        )

        common.validate_against(graph)
        self.assertIsNone(graph.edges[0].estimated_bytes)
        self.assertIsNone(
            common.requirements_by_role["stage-0"].activation_bytes
        )
        self.assertEqual(common.cross_partition_tensors, ("hidden",))


if __name__ == "__main__":
    unittest.main()
