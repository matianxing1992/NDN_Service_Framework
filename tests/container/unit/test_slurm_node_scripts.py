from __future__ import annotations
import json,os
from pathlib import Path
import subprocess,tempfile,unittest
from contract._support import REPO
ROOT=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
class SlurmNodeScriptsTest(unittest.TestCase):
    def test_apptainer_command_is_clean_nv_and_read_only_identity(self):
        text=(ROOT/"run-container.sh").read_text(); self.assertIn("--cleanenv --nv",text); self.assertIn('$identity:/identity:ro',text); self.assertNotIn("nvidia-container",text.lower())
    def test_compute_preflight_requires_allocation(self):
        r=subprocess.run([str(ROOT/"preflight-compute.sh"),"--scratch","/tmp/ndnsf-di-x","--gpu-type","rtx_5000","--gpu-count","1"],env={k:v for k,v in os.environ.items() if k!='SLURM_JOB_ID'},text=True,capture_output=True,check=False)
        self.assertEqual(r.returncode,3);self.assertIn("REQUIRES_SLURM",r.stderr)
    def test_bounded_scratch_fsync(self):
        with tempfile.TemporaryDirectory(dir='/tmp',prefix='ndnsf-di-unit-') as d:
            r=subprocess.run([str(ROOT/"check-scratch.py"),"--path",d,"--bytes","1048576"],text=True,capture_output=True,check=False)
            self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(json.loads(r.stdout)["bytes"],1048576)
