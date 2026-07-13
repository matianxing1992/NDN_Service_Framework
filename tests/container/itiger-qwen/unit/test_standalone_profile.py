from __future__ import annotations
import unittest
from pathlib import Path
from _support import REPO
class StandaloneProfileTest(unittest.TestCase):
 def test_templates_bind_gpu_scratch_readonly_and_traps(self):
  for name in ('qwen-oracle.sbatch.in','qwen-staged-baseline.sbatch.in'):
   text=(REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/templates'/name).read_text();self.assertIn('apptainer exec --nv',text);self.assertIn(':ro',text);self.assertIn('/tmp/$USER/ndnsf-di/$SLURM_JOB_ID',text);self.assertIn('trap ',text)
if __name__=='__main__':unittest.main()
