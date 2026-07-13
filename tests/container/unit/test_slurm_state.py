from __future__ import annotations
import unittest
from contract._support import FIXTURES,load_impl
slurm_impl=load_impl("adapters/slurm_apptainer")
class SlurmStateTest(unittest.TestCase):
    def test_completed(self):
        value=slurm_impl.parse_sacct((FIXTURES/"itiger/slurm/sacct-completed.txt").read_text(),"145855")
        self.assertEqual(value["state"],"COMPLETED"); self.assertEqual(value["exitCode"],"0:0"); self.assertTrue(value["terminal"])
    def test_timeout_is_terminal_failure(self):
        value=slurm_impl.parse_sacct((FIXTURES/"itiger/slurm/sacct-timeout.txt").read_text(),"145901")
        self.assertEqual(value["state"],"TIMEOUT"); self.assertFalse(value["successful"])
    def test_wrong_job_rejected(self):
        with self.assertRaisesRegex(slurm_impl.SlurmAdapterError,"JOB_ID"):
            slurm_impl.parse_sacct((FIXTURES/"itiger/slurm/sacct-completed.txt").read_text(),"999")
