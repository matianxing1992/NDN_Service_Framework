from __future__ import annotations
from pathlib import Path
import tempfile,unittest
from _support import load_tool
job=load_tool("spec109_job")
class JobRenderTest(unittest.TestCase):
    def test_resources_render_and_injection_is_rejected(self):
        values={"jobName":"qwen-05b","partition":"bigTiger","account":"devs","qos":"normal","wallTime":"00:05:00","cpus":2,"memory":"8G","gpuType":"rtx_5000","gpuCount":1,"runId":"r1","command":"/bin/true"}
        text=job.render_sbatch(values); self.assertIn("--gres=gpu:rtx_5000:1",text); self.assertIn("--cpus-per-task=2",text)
        values["runId"]="r1;touch /tmp/bad"
        with self.assertRaisesRegex(job.JobError,"UNSAFE"): job.render_sbatch(values)
    def test_exact_once_ledger(self):
        with tempfile.TemporaryDirectory() as tmp:
            ledger=job.reserve_run(Path(tmp)/"ledger.json","r1","sha256:"+"a"*64); self.assertEqual(ledger["state"],"RESERVED")
            with self.assertRaisesRegex(job.JobError,"RUN_ALREADY_RESERVED"): job.reserve_run(Path(tmp)/"ledger.json","r1","sha256:"+"a"*64)
if __name__ == "__main__": unittest.main()
