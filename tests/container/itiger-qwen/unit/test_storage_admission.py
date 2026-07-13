from __future__ import annotations
import copy,unittest
from _support import FIXTURES,load_json,load_tool
storage=load_tool("spec109_storage")
class StorageAdmissionTest(unittest.TestCase):
    def setUp(self): self.cases=load_json(FIXTURES/"storage/cases.json")
    def test_projection_reserve_and_actual_quota_precedence(self):
        report=storage.evaluate_storage(self.cases["base"]); self.assertEqual(report["status"],"PASS"); self.assertEqual(report["projectedPeakBytes"],5_000_000_000)
        full=copy.deepcopy(self.cases["base"]); full.update(self.cases["quotaFull"]); self.assertEqual(storage.evaluate_storage(full)["status"],"BLOCKED")
        shared=copy.deepcopy(self.cases["base"]); shared.update(self.cases["sharedCapacityOnly"]); self.assertEqual(storage.evaluate_storage(shared)["reasonCode"],"QUOTA_NOT_VERIFIED")
    def test_cleanup_protects_referenced_current_and_evidence_paths(self):
        candidates=["models/source/qwen25-0.5b","models/source/old","images/current.sif","images/old.sif","evidence/run-1"]
        plan=storage.plan_cleanup(candidates,protected=self.cases["protectedCleanup"])
        self.assertEqual(plan["delete"],["images/old.sif","models/source/old"]); self.assertEqual(len(plan["protected"]),3)
if __name__ == "__main__": unittest.main()
