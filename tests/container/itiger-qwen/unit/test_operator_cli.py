from __future__ import annotations
import json,subprocess,tempfile,unittest
from pathlib import Path
from _support import REPO,source_snapshot
CLI=REPO/"tools/ndnsf-di/ndnsf-di-qwen"
class OperatorCliTest(unittest.TestCase):
    def test_validate_is_offline_and_render_does_not_submit_by_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/"source.json";source.write_text(json.dumps(source_snapshot()))
            result=subprocess.run([str(CLI),"validate","--schema","source","--input",str(source)],text=True,capture_output=True,check=False)
            self.assertEqual(result.returncode,0,result.stderr);self.assertIn('"status": "PASS"',result.stdout)
            profile={"jobName":"qwen","partition":"bigTiger","account":"devs","qos":"normal","wallTime":"00:05:00","cpus":2,"memory":"8G","gpuType":"rtx_5000","gpuCount":1,"runId":"r1","command":"/bin/true"}
            p=root/"job.json";p.write_text(json.dumps(profile));script=root/"job.sbatch";ledger=root/"ledger.json"
            result=subprocess.run([str(CLI),"render","--job-profile",str(p),"--output",str(script),"--ledger",str(ledger)],text=True,capture_output=True,check=False)
            self.assertEqual(result.returncode,0,result.stderr);self.assertTrue(script.is_file());self.assertFalse(ledger.exists());self.assertIn('"submitted": false',result.stdout)
    def test_transfer_render_is_cpu_only_and_requires_explicit_submit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);entry={"repository":"Qwen/Qwen2.5-0.5B-Instruct","revision":"a"*40,"licenseClass":"apache-2.0","projectRoot":"/project/tma1/ndnsf-di","partialPath":"/project/tma1/ndnsf-di/models/.partial/qwen05","transferManifest":"/project/tma1/ndnsf-di/manifests/qwen05-transfer.json","slurm":{"partition":"bigTiger","account":"devs","qos":"normal","wallTime":"00:30:00","cpus":4,"memory":"16G"}}
            source=root/'entry.json';source.write_text(json.dumps(entry));script=root/'transfer.sbatch';ledger=root/'ledger.json'
            result=subprocess.run([str(CLI),'transfer','--model-entry',str(source),'--run-id','transfer-05','--output',str(script),'--ledger',str(ledger)],text=True,capture_output=True,check=False)
            self.assertEqual(result.returncode,0,result.stderr);text=script.read_text();self.assertNotIn('--gres',text);self.assertIn('stage-qwen-model.py',text);self.assertFalse(ledger.exists());self.assertIn('"submitted": false',result.stdout)
if __name__=="__main__":unittest.main()
