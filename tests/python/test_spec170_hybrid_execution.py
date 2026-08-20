from __future__ import annotations

import hashlib
from itertools import product
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core import (  # noqa: E402
    DataSegmentReplayWindow, DataSegmentV1, HybridPlan, LocalTensorGroup, RedistributionEdge,
    TensorDisposition, TensorSlice,
)
from ndnsf_distributed_inference.adapters.qwen.parallel import (  # noqa: E402
    seal_qwen_hybrid_plan,
)
from ndnsf_distributed_inference.adapters.qwen.placement import (  # noqa: E402
    QWEN36_STAGE_ROLES, build_qwen_three_stage_adapter,
)
from ndnsf_distributed_inference.app_sdk.placement import (  # noqa: E402
    AutomaticPlanningCoordinator, PublishedSplit,
)


class HybridExecutionContractTest(unittest.TestCase):
    @staticmethod
    def _rank_labels(degrees):
        return tuple(
            f"S{stage}R{rank}"
            for stage, degree in enumerate(degrees)
            for rank in range(degree))

    @staticmethod
    def _certified_redistributions(degrees):
        """Return the explicit adapter certificate for this test layout.

        The operation is test-fixture input, not a HybridPlan inference from
        rank counts.  Production adapters must certify the operation from the
        source and target tensor layouts.
        """
        offsets = []
        cursor = 0
        for degree in degrees:
            offsets.append(cursor)
            cursor += degree
        edges = []
        for stage, (producer_degree, consumer_degree) in enumerate(
                zip(degrees, degrees[1:])):
            if producer_degree == consumer_degree:
                continue
            producer_ranks = tuple(
                range(offsets[stage], offsets[stage] + producer_degree))
            consumer_ranks = tuple(
                range(offsets[stage + 1], offsets[stage + 1] + consumer_degree))
            edges.append(RedistributionEdge(
                producer_ranks=producer_ranks,
                consumer_ranks=consumer_ranks,
                tensor=f"activation-{stage}",
                operation=("SCATTER" if producer_degree == 1
                           else "GATHER" if consumer_degree == 1
                           else "RESHARD"),
                epoch="epoch-1",
                integrity_digest="sha256:" + hashlib.sha256(
                    f"{stage}:{producer_degree}:{consumer_degree}".encode()
                ).hexdigest(),
                source_layout_digest="sha256:" + hashlib.sha256(
                    f"source:{stage}:{producer_degree}".encode()).hexdigest(),
                target_layout_digest="sha256:" + hashlib.sha256(
                    f"target:{stage}:{consumer_degree}".encode()).hexdigest(),
                temporary_memory_bytes=4096,
            ))
        return tuple(edges)

    def test_all_120_hybrid_vectors_have_exact_cover_and_stable_identity(self):
        vectors = tuple(
            degrees
            for stages in range(1, 5)
            for degrees in product((1, 2, 3), repeat=stages))
        self.assertEqual(len(vectors), 120)
        digests = set()
        for degrees in vectors:
            plan = seal_qwen_hybrid_plan(
                tensor_degrees=degrees,
                redistributions=self._certified_redistributions(degrees),
            )
            self.assertEqual(plan.rank_count, sum(degrees))
            self.assertEqual(plan.digest(), seal_qwen_hybrid_plan(
                tensor_degrees=degrees,
                redistributions=self._certified_redistributions(degrees),
            ).digest())
            digests.add(plan.digest())
        self.assertEqual(len(digests), 120)

    def test_redistribution_certificate_rejects_operation_rank_mismatch(self):
        with self.assertRaisesRegex(ValueError, "operation"):
            RedistributionEdge(
                producer_ranks=(0,), consumer_ranks=(1, 2),
                tensor="activation-0", operation="GATHER", epoch="epoch-1",
                integrity_digest="sha256:" + "a" * 64,
                source_layout_digest="sha256:" + "b" * 64,
                target_layout_digest="sha256:" + "c" * 64,
                temporary_memory_bytes=4096,
            )

    def test_hybrid_plan_rejects_rank_and_redistribution_mutations(self):
        degrees = (1, 2, 1)
        labels = self._rank_labels(degrees)
        edges = self._certified_redistributions(degrees)
        mutations = (
            dict(tensor_degrees=(1, 0, 1), rank_labels=("S0R0", "S2R0"),
                 redistributions=()),
            dict(rank_labels=labels[:-1]),
            dict(rank_labels=(labels[0], labels[1], labels[1], labels[3])),
            dict(rank_labels=("S0R0", "S1R0", "S1R1", "S9R0")),
            dict(redistributions=edges[:-1]),
            dict(redistributions=(edges[0], edges[0], edges[1])),
            dict(redistributions=(RedistributionEdge(
                producer_ranks=(99,), consumer_ranks=(1, 2),
                tensor="activation-0", operation="SCATTER", epoch="epoch-1",
                integrity_digest="sha256:" + "a" * 64,
                source_layout_digest="sha256:" + "b" * 64,
                target_layout_digest="sha256:" + "c" * 64,
                temporary_memory_bytes=4096), edges[1])),
            dict(redistributions=(RedistributionEdge(
                producer_ranks=edges[0].consumer_ranks,
                consumer_ranks=edges[0].producer_ranks,
                tensor=edges[0].tensor, operation="GATHER", epoch=edges[0].epoch,
                integrity_digest=edges[0].integrity_digest,
                source_layout_digest=edges[0].target_layout_digest,
                target_layout_digest=edges[0].source_layout_digest,
                temporary_memory_bytes=edges[0].temporary_memory_bytes),
                edges[1])),
        )
        for mutation in mutations:
            values = dict(
                stages=3, tensor_degrees=degrees, rank_labels=labels,
                redistributions=edges)
            values.update(mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                HybridPlan(**values)

    def test_heterogeneous_degree_vector_has_exact_rank_cover(self):
        plan = HybridPlan(
            stages=3, tensor_degrees=(1, 2, 1),
            rank_labels=("S0R0", "S1R0", "S1R1", "S2R0"),
            redistributions=self._certified_redistributions((1, 2, 1)))
        self.assertEqual(plan.rank_count, 4)
        with self.assertRaises(ValueError):
            HybridPlan(stages=3, tensor_degrees=(1, 2, 1),
                       rank_labels=("S0R0", "S1R0", "S2R0"))

    def test_qwen_split_candidate_carries_rank_artifacts_into_v3_roles(self):
        rank_artifacts = {
            role: tuple(
                "sha256:" + hashlib.sha256(
                    f"{role}:{rank}".encode()).hexdigest()
                for rank in range(degree))
            for role, degree in zip(QWEN36_STAGE_ROLES, (1, 2, 1))
        }
        adapter = build_qwen_three_stage_adapter(
            model_name="Qwen/test",
            revision="immutable-test-revision",
            layer_ranges=((0, 1), (1, 2), (2, 3)),
            artifact_digests_by_role={
                role: values[0] for role, values in rank_artifacts.items()},
            weight_bytes_by_role={role: 1024 for role in QWEN36_STAGE_ROLES},
            tensor_degrees=(1, 2, 1),
            rank_artifact_digests_by_role=rank_artifacts,
            redistributions=self._certified_redistributions((1, 2, 1)),
        )
        model = adapter.describe_model(
            "Qwen/test", "sha256:" + "c" * 64, "sha256:" + "d" * 64,
            source_revision="immutable-test-revision")
        candidate = adapter.splitter.enumerate_candidates(
            model, adapter.graph.inspect(model))[0]
        self.assertEqual(candidate.hybrid_plan.tensor_degrees, (1, 2, 1))
        self.assertEqual(len(candidate.hybrid_plan.redistributions), 2)
        specs = AutomaticPlanningCoordinator._v3_role_specs(candidate)
        self.assertEqual(tuple(item.rank for item in specs), (0, 0, 1, 0))
        self.assertEqual(
            tuple(item.artifact_digest for item in specs),
            tuple(value for role in QWEN36_STAGE_ROLES
                  for value in rank_artifacts[role]),
        )
        flattened = {
            (role if len(values) == 1 else f"{role}#{rank}"): value
            for role, values in rank_artifacts.items()
            for rank, value in enumerate(values)
        }
        published = PublishedSplit(
            candidate.candidate_digest,
            flattened,
            {role: f"/repo/hybrid/{index}"
             for index, role in enumerate(flattened)},
        )
        AutomaticPlanningCoordinator._validate_published_split(
            candidate, published)
        missing = dict(flattened)
        missing.pop(f"{QWEN36_STAGE_ROLES[1]}#1")
        with self.assertRaisesRegex(ValueError, "selected candidate"):
            AutomaticPlanningCoordinator._validate_published_split(
                candidate,
                PublishedSplit(
                    candidate.candidate_digest, missing,
                    {role: f"/repo/hybrid/missing/{index}"
                     for index, role in enumerate(missing)}))

    def test_local_tensor_group_rejects_overlap_and_missing_rank(self):
        tensor_slices = tuple(
            TensorSlice("w", rank=rank, disposition=TensorDisposition.SHARDED,
                        axis=0, begin=rank * 4, end=(rank + 1) * 4, layout="row")
            for rank in (0, 1))
        group = LocalTensorGroup("S1", "e1", ("cuda:0", "cuda:1"), tensor_slices)
        self.assertEqual(len(group.tensors), 2)
        with self.assertRaises(ValueError):
            LocalTensorGroup("S1", "e1", ("cuda:0", "cuda:1"),
                             (tensor_slices[0],))
        with self.assertRaisesRegex(ValueError, "contiguous"):
            LocalTensorGroup(
                "S1", "e1", ("cuda:0", "cuda:1"),
                (TensorSlice("w", 0, TensorDisposition.SHARDED, 0, 0, 4, "row"),
                 TensorSlice("w", 1, TensorDisposition.SHARDED, 0, 5, 8, "row")))

    def test_ndnsf_data_segment_binds_epoch_nonce_and_mac(self):
        key = b"k" * 32
        segment = DataSegmentV1.create(
            operation_id="op-1", epoch="epoch-1", producer="p0",
            consumer="p1", segment_no=0, payload=b"tensor-bytes", key=key,
            aad=b"tensor=h")
        segment.verify(key)
        window = DataSegmentReplayWindow(operation_id="op-1", epoch="epoch-1")
        window.accept(segment, key=key)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            window.accept(segment, key=key)
        with self.assertRaises(ValueError):
            segment.verify(b"x" * 32)


if __name__ == "__main__":
    unittest.main()
