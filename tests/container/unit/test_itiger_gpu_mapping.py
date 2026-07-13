from __future__ import annotations
import json,subprocess,unittest
from contract._support import FIXTURES,REPO
SCRIPT=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/collect-gpu.py"
class ItigerGpuMappingTest(unittest.TestCase):
    def test_uuid_correlates_physical_and_container_indices(self):
        r=subprocess.run([str(SCRIPT),"--host",str(FIXTURES/"itiger/gres/host.csv"),"--container",str(FIXTURES/"itiger/gres/container.csv"),"--requested-gres","gpu:rtx_5000:1","--slurm-job-gpus","4","--cuda-visible-devices","0"],text=True,capture_output=True,check=False)
        self.assertEqual(r.returncode,0,r.stderr); v=json.loads(r.stdout); self.assertEqual(v["status"],"PASS"); self.assertEqual(v["host"][0]["index"],4); self.assertEqual(v["container"][0]["index"],0); self.assertEqual(v["host"][0]["uuid"],v["container"][0]["uuid"])
