from __future__ import annotations
import json,os,subprocess,tempfile,unittest
from pathlib import Path
from _support import REPO
STAGE=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/stage-qwen-model.py';FINAL=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/finalize-qwen-model.py';REV='a'*40
class ModelTransferTest(unittest.TestCase):
 def test_partial_resumed_and_completed_states(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);source=root/'source';source.mkdir();(source/'config.json').write_text('{}');(source/'LICENSE').write_text('Apache-2.0')
   partial=root/'project/models/.partial/model';manifest=root/'transfer.json';sealed=root/'sealed.json';final=root/'project/models/source/model'
   env=dict(os.environ,NDNSF_SPEC109_ALLOW_TEST_ROOT='1');cmd=[str(STAGE),'--repository','Qwen/Qwen2.5-0.5B-Instruct','--revision',REV,'--destination',str(partial),'--manifest',str(manifest),'--license-class','apache-2.0','--source-dir',str(source)]
   first=subprocess.run(cmd,env=env,text=True,capture_output=True,check=False);self.assertEqual(first.returncode,0,first.stderr);self.assertEqual(json.loads(manifest.read_text())['state'],'STAGED')
   resumed=subprocess.run(cmd,env=env,text=True,capture_output=True,check=False);self.assertNotEqual(resumed.returncode,0);self.assertIn('PARTIAL_ALREADY_EXISTS',resumed.stderr)
   promoted=subprocess.run([str(FINAL),'--partial',str(partial),'--manifest',str(manifest),'--final',str(final),'--sealed-manifest',str(sealed)],text=True,capture_output=True,check=False);self.assertEqual(promoted.returncode,0,promoted.stderr);self.assertEqual(json.loads(sealed.read_text())['state'],'SEALED');self.assertTrue(final.is_dir())
   duplicate=subprocess.run([str(FINAL),'--partial',str(partial),'--manifest',str(manifest),'--final',str(final),'--sealed-manifest',str(sealed)],text=True,capture_output=True,check=False);self.assertNotEqual(duplicate.returncode,0)
 def test_lfs_pointer_is_quarantined(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);source=root/'source';source.mkdir();(source/'model.bin').write_text('version https://git-lfs.github.com/spec/v1\n')
   env=dict(os.environ,NDNSF_SPEC109_ALLOW_TEST_ROOT='1');r=subprocess.run([str(STAGE),'--repository','Qwen/Qwen2.5-0.5B-Instruct','--revision',REV,'--destination',str(root/'partial'),'--manifest',str(root/'manifest.json'),'--license-class','apache-2.0','--source-dir',str(source)],env=env,text=True,capture_output=True,check=False);self.assertNotEqual(r.returncode,0);self.assertIn('LFS_POINTER_REJECTED',r.stderr)
if __name__=='__main__':unittest.main()
