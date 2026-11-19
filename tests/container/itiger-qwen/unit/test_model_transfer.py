from __future__ import annotations
import hashlib,importlib.util,io,json,os,subprocess,tempfile,unittest
from unittest.mock import patch
from pathlib import Path
from _support import REPO
STAGE=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/stage-qwen-model.py';FINAL=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/finalize-qwen-model.py';REV='a'*40
class ModelTransferTest(unittest.TestCase):
 def downloader(self):
  spec=importlib.util.spec_from_file_location('model_transfer',STAGE)
  module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  return module
 def test_selected_download_does_not_request_unlisted_precision_variants(self):
  module=self.downloader();calls=[];payload=b'one fixed model'
  required={'onnx/model_q4.onnx':{'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest()}}
  def request(url,timeout):
   calls.append(url)
   if '/api/' in url:return io.BytesIO(json.dumps({'sha':REV,'siblings':[{'rfilename':name} for name in ['onnx/model_q4.onnx','onnx/model_fp32.onnx']]}).encode())
   self.assertTrue(url.endswith('/onnx/model_q4.onnx'));return io.BytesIO(payload)
  with tempfile.TemporaryDirectory() as tmp,patch.object(module.urllib.request,'urlopen',request):
   root=Path(tmp);module.public_snapshot_download('owner/model',REV,root,required)
   self.assertEqual((root/'onnx/model_q4.onnx').read_bytes(),payload)
   self.assertEqual(len(calls),2)
 def test_bad_commitment_and_stream_failure_remove_only_owned_partial(self):
  module=self.downloader()
  for payload,size,digest in [(b'too large',2,'0'*64),(b'bad',3,'0'*64)]:
   with self.subTest(payload=payload),tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp);sentinel=root/'keep';sentinel.write_text('retained')
    responses=[io.BytesIO(json.dumps({'sha':REV,'siblings':[{'rfilename':'model'}]}).encode()),io.BytesIO(payload)]
    with patch.object(module.urllib.request,'urlopen',side_effect=responses):
     with self.assertRaisesRegex(ValueError,'TRANSFER_FILE_'):module.public_snapshot_download('owner/model',REV,root,{'model':{'bytes':size,'sha256':digest}})
    self.assertFalse((root/'model').exists());self.assertEqual(sentinel.read_text(),'retained')
 def test_missing_file_and_absolute_path_are_rejected(self):
  module=self.downloader()
  for name,available,error in [('missing',['other'],'REQUIRED_FILE_MISSING'),('/escape',['/escape'],'REMOTE_PATH_INVALID'),('../escape',['../escape'],'REMOTE_PATH_INVALID')]:
   with self.subTest(name=name),tempfile.TemporaryDirectory() as tmp:
    metadata=io.BytesIO(json.dumps({'sha':REV,'siblings':[{'rfilename':item} for item in available]}).encode())
    with patch.object(module.urllib.request,'urlopen',return_value=metadata):
     with self.assertRaisesRegex(ValueError,error):module.public_snapshot_download('owner/model',REV,Path(tmp),{name:{'bytes':1,'sha256':'0'*64}})
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
