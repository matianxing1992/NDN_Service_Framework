from __future__ import annotations
import sys,unittest
from _support import TOOLS
sys.path.insert(0,str(TOOLS))
import run_spec109_ladder as ladder
class PartitionPlannerTest(unittest.TestCase):
    def test_balances_layers_and_accounts_transfer_boundaries(self):
        rows=ladder.partition_layers(48,[48_000_000_000,48_000_000_000],500_000_000)
        self.assertEqual(sum(row["layerCount"] for row in rows),48)
        self.assertLessEqual(max(row["layerCount"] for row in rows)-min(row["layerCount"] for row in rows),1)
        self.assertEqual(rows[0]["endLayer"],rows[1]["startLayer"])
    def test_rejects_insufficient_memory(self):
        with self.assertRaisesRegex(ValueError,"PARTITION_MEMORY_INSUFFICIENT"):
            ladder.partition_layers(80,[1_000_000_000],1_000_000_000)
if __name__=="__main__":unittest.main()
