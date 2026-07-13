from __future__ import annotations
import unittest
from _support import load_tool
matrix=load_tool("spec109_matrix")
class LargeModelPlacementTest(unittest.TestCase):
    def test_one_node_multi_gpu_is_preferred(self):
        value=matrix.placement_admission(model_bytes=120_000_000_000,gpu_memory_bytes=[80_000_000_000]*2,stage_count=3,node_count=1,network_status="BLOCKED")
        self.assertEqual(value["status"],"PASS");self.assertEqual(value["placement"],"one-node-multi-gpu")
    def test_multinode_needs_network_evidence_and_memory(self):
        value=matrix.placement_admission(model_bytes=120_000_000_000,gpu_memory_bytes=[80_000_000_000]*2,stage_count=3,node_count=2,network_status="BLOCKED")
        self.assertEqual(value["status"],"DEFERRED")
        self.assertEqual(matrix.placement_admission(model_bytes=120_000_000_000,gpu_memory_bytes=[80_000_000_000]*2,stage_count=3,node_count=2,network_status="PASS")["status"],"PASS")
        self.assertEqual(matrix.placement_admission(model_bytes=200_000_000_000,gpu_memory_bytes=[80_000_000_000]*2,stage_count=3,node_count=1,network_status="PASS")["reasonCode"],"GPU_MEMORY_INSUFFICIENT")
if __name__=="__main__":unittest.main()
