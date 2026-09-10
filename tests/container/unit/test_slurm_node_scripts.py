from __future__ import annotations
import json,os
from pathlib import Path
import shutil
import subprocess,tempfile,unittest
from contract._support import REPO
ROOT=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
class SlurmNodeScriptsTest(unittest.TestCase):
    def test_apptainer_command_is_clean_nv_and_read_only_identity(self):
        text=(ROOT/"run-container.sh").read_text(); self.assertIn("--cleanenv --nv",text); self.assertIn('$identity:/identity:ro',text); self.assertNotIn("nvidia-container",text.lower())
    def test_compute_preflight_requires_allocation(self):
        r=subprocess.run([str(ROOT/"preflight-compute.sh"),"--scratch","/tmp/ndnsf-di-x","--gpu-type","rtx_5000","--gpu-count","1"],env={k:v for k,v in os.environ.items() if k!='SLURM_JOB_ID'},text=True,capture_output=True,check=False)
        self.assertEqual(r.returncode,3);self.assertIn("REQUIRES_SLURM",r.stderr)
    def test_compute_preflight_accepts_job_bound_run_suffix(self):
        with tempfile.TemporaryDirectory(dir="/tmp", prefix="ndnsf-preflight-") as d:
            fake_bin=Path(d)/"bin"; fake_bin.mkdir()
            apptainer=fake_bin/"apptainer"
            apptainer.write_text("#!/bin/sh\n[ \"$1\" = version ]\necho fake-apptainer\n")
            apptainer.chmod(0o700)
            target=Path(tempfile.mkdtemp(prefix="ndnsf-di-701-", dir="/tmp"))
            try:
                r=subprocess.run(
                    [str(ROOT/"preflight-compute.sh"), "--scratch", str(target),
                     "--gpu-type", "cpu", "--gpu-count", "0"],
                    env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                         "SLURM_JOB_ID": "701"}, text=True, capture_output=True,
                    check=False)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("fake-apptainer", (target/"evidence/apptainer-version.txt").read_text())
            finally:
                shutil.rmtree(target, ignore_errors=True)

    def test_compute_preflight_rejects_other_job_or_nested_scratch(self):
        with tempfile.TemporaryDirectory(dir="/tmp", prefix="ndnsf-preflight-") as d:
            fake_bin=Path(d)/"bin"; fake_bin.mkdir()
            apptainer=fake_bin/"apptainer"
            apptainer.write_text("#!/bin/sh\nexit 99\n")
            apptainer.chmod(0o700)
            env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                 "SLURM_JOB_ID": "701"}
            for scratch in ("/tmp/ndnsf-di-702-runA", "/tmp/ndnsf-di-701-runA/nested"):
                r=subprocess.run(
                    [str(ROOT/"preflight-compute.sh"), "--scratch", scratch,
                     "--gpu-type", "cpu", "--gpu-count", "0"],
                    env=env, text=True, capture_output=True, check=False)
                self.assertEqual(r.returncode, 3, scratch)
                self.assertIn("COMPUTE_SCRATCH_POLICY_INVALID", r.stderr)

    def test_compute_preflight_rejects_job_named_scratch_symlink(self):
        with tempfile.TemporaryDirectory(dir="/tmp", prefix="ndnsf-preflight-") as d:
            fake_bin=Path(d)/"bin"; fake_bin.mkdir()
            apptainer=fake_bin/"apptainer"
            apptainer.write_text("#!/bin/sh\nexit 99\n")
            apptainer.chmod(0o700)
            target=Path(tempfile.mkdtemp(prefix="ndnsf-di-preflight-target-", dir="/tmp"))
            link=Path("/tmp/ndnsf-di-702-symlink")
            link.unlink(missing_ok=True)
            link.symlink_to(target, target_is_directory=True)
            try:
                r=subprocess.run(
                    [str(ROOT/"preflight-compute.sh"), "--scratch", str(link),
                     "--gpu-type", "cpu", "--gpu-count", "0"],
                    env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
                         "SLURM_JOB_ID": "702"}, text=True, capture_output=True,
                    check=False)
                self.assertEqual(r.returncode, 3)
                self.assertIn("COMPUTE_SCRATCH_SYMLINK_FORBIDDEN", r.stderr)
                self.assertFalse((target/"evidence").exists())
            finally:
                link.unlink(missing_ok=True)
                shutil.rmtree(target, ignore_errors=True)
    def test_bounded_scratch_fsync(self):
        with tempfile.TemporaryDirectory(dir='/tmp',prefix='ndnsf-di-unit-') as d:
            r=subprocess.run([str(ROOT/"check-scratch.py"),"--path",d,"--bytes","1048576"],text=True,capture_output=True,check=False)
            self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(json.loads(r.stdout)["bytes"],1048576)
