from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core import (  # noqa: E402
    DataSegmentReplayWindow, DataSegmentV1, HybridPlan, LocalTensorGroup, RedistributionEdge,
    TensorDisposition, TensorSlice,
)


class HybridExecutionContractTest(unittest.TestCase):
    def test_heterogeneous_degree_vector_has_exact_rank_cover(self):
        plan = HybridPlan(
            stages=3, tensor_degrees=(1, 2, 1),
            rank_labels=("S0R0", "S1R0", "S1R1", "S2R0"),
            redistributions=(RedistributionEdge(
                producer_ranks=(0,), consumer_ranks=(1, 2), tensor="h",
                operation="SCATTER", epoch="e1",
                integrity_digest="sha256:" + "a" * 64),))
        self.assertEqual(plan.rank_count, 4)
        with self.assertRaises(ValueError):
            HybridPlan(stages=3, tensor_degrees=(1, 2, 1),
                       rank_labels=("S0R0", "S1R0", "S2R0"))

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
