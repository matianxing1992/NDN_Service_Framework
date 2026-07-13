from __future__ import annotations
import unittest
from _support import FIXTURES,load_json,load_tool
storage=load_tool("spec109_storage")
class LargeModelAdmissionTest(unittest.TestCase):
    def test_file_manifest_peak_and_quota_gate_precede_transfer(self):
        fixture=load_json(FIXTURES/"large-model/file-manifests.json");a=fixture["amplification"]
        peak=storage.large_model_peak(fixture["32B"],export_multiplier_milli=a["exportMultiplierMilli"],cache_multiplier_milli=a["cacheMultiplierMilli"],evidence_bytes=a["evidenceBytes"])
        self.assertEqual(peak["source"],65_000_000_000)
        record={"targetPath":"/project/tma1/ndnsf-di","quotaSource":"admin-allocation","quotaVerified":True,"limitBytes":200_000_000_000,"usedBytes":1_000_000_000,"sharedFreeBytes":900_000_000_000_000,"projected":{k:peak[k] for k in ("source","export","cache","evidence")},"reserveBytes":20_000_000_000,"protectedPaths":[]}
        self.assertEqual(storage.evaluate_storage(record)["status"],"PASS")
        record["limitBytes"]=100_000_000_000
        blocked=storage.evaluate_storage(record);self.assertEqual(blocked["status"],"BLOCKED")
        self.assertEqual(blocked["reasonCode"],"QUOTA_RESERVE_INSUFFICIENT")
    def test_tampered_or_duplicate_manifest_fails_before_transfer(self):
        with self.assertRaisesRegex(storage.StorageError,"LARGE_MODEL_MANIFEST_INVALID"):
            storage.large_model_peak([{"path":"x","bytes":1,"sha256":"bad"}],export_multiplier_milli=1000,cache_multiplier_milli=0,evidence_bytes=0)
if __name__=="__main__":unittest.main()
