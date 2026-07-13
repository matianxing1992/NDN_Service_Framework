from __future__ import annotations
from pathlib import Path
import subprocess,tempfile,unittest
from contract._support import FIXTURES,load_impl
profile_impl=load_impl("profile"); slurm_impl=load_impl("adapters/slurm_apptainer")
class Runner:
    def __init__(self): self.commands=[]
    def __call__(self,cmd,**kwargs):
        self.commands.append(list(cmd)); return subprocess.CompletedProcess(cmd,0,stdout="145999\n",stderr="")
class SlurmSubmitTest(unittest.TestCase):
    def test_submit_exactly_once_for_run_id(self):
        profile=profile_impl.load_profile(FIXTURES/"profiles/itiger-rtx5000-valid.yaml")
        with tempfile.TemporaryDirectory() as d:
            runner=Runner(); adapter=slurm_impl.SlurmApptainerAdapter(runner=runner,state_root=Path(d))
            job=adapter.submit(profile,preflight=False,materialize=False); self.assertEqual(job["jobId"],"145999")
            with self.assertRaisesRegex(slurm_impl.SlurmAdapterError,"RUN_ALREADY_SUBMITTED"): adapter.submit(profile,preflight=False,materialize=False)
            self.assertEqual(sum(1 for c in runner.commands if c[0]=="sbatch"),1)
