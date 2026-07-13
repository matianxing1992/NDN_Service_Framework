from __future__ import annotations
import json,subprocess,unittest
from contract._support import FIXTURES,REPO
SCRIPT=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/check-storage.py"
class ItigerStorageTest(unittest.TestCase):
    def run_fixture(self,name):
        return subprocess.run([str(SCRIPT),"--facts",str(FIXTURES/"itiger/storage"/name),"--minimum-free-bytes","1000000"],text=True,capture_output=True,check=False)
    def test_valid_storage_separates_shared_capacity_from_quota(self):
        r=self.run_fixture("valid.json"); self.assertEqual(r.returncode,0,r.stderr); v=json.loads(r.stdout)
        self.assertEqual(v["status"],"PASS"); self.assertFalse(v["quota"]["verifiedByCommand"])
    def test_quota_full_fails_despite_shared_capacity(self):
        r=self.run_fixture("quota-full.json"); self.assertEqual(r.returncode,3); self.assertIn("STORAGE_QUOTA_EXHAUSTED",r.stderr)
