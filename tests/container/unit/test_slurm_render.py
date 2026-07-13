from __future__ import annotations
import copy, unittest
from contract._support import FIXTURES, REPO, load_impl
profile_impl=load_impl("profile")
slurm_impl=load_impl("adapters/slurm_apptainer")

class SlurmRenderTest(unittest.TestCase):
    def setUp(self):
        self.profile=profile_impl.load_profile(FIXTURES/"profiles/itiger-rtx5000-valid.yaml")
        self.template=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/ndnsf-di.sbatch.in"
    def test_explicit_resources_and_gres(self):
        text=slurm_impl.render_sbatch(self.profile,self.template)
        for value in ("#SBATCH --partition=bigTiger","#SBATCH --gres=gpu:rtx_5000:1","#SBATCH --cpus-per-task=2","#SBATCH --mem=8G","#SBATCH --time=00:05:00"):
            self.assertIn(value,text)
        self.assertNotIn("@@",text)
    def test_injection_is_rejected(self):
        value=copy.deepcopy(self.profile); value["slurm"]["jobName"]="bad\n#SBATCH --nodes=99"
        with self.assertRaisesRegex(slurm_impl.SlurmAdapterError,"UNSAFE"):
            slurm_impl.render_sbatch(value,self.template)
