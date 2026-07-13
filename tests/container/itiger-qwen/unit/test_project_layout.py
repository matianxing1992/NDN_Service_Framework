from __future__ import annotations
import os,stat,subprocess,tempfile,unittest
from pathlib import Path
from _support import REPO
SCRIPT=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/prepare-qwen-project.sh'
class ProjectLayoutTest(unittest.TestCase):
 def test_layout_modes_and_paths(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)/'ndnsf-di';env=dict(os.environ,NDNSF_SPEC109_ALLOW_TEST_ROOT='1');r=subprocess.run([str(SCRIPT),str(root)],env=env,text=True,capture_output=True,check=False);self.assertEqual(r.returncode,0,r.stderr)
   for name in ('src','images','models','cache','manifests','evidence'):self.assertTrue((root/name).is_dir());self.assertEqual(stat.S_IMODE((root/name).stat().st_mode),0o750)
 def test_rejects_home_without_test_override(self):
  r=subprocess.run([str(SCRIPT),'/home/tma1/ndnsf-di'],text=True,capture_output=True,check=False);self.assertNotEqual(r.returncode,0);self.assertIn('PROJECT_ROOT_INVALID',r.stderr)
if __name__=='__main__':unittest.main()
