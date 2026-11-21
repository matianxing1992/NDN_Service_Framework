"""Memory boundaries and malformed job counts for the shared host build policy."""
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]

class HostBuildJobPolicy(unittest.TestCase):
    def policy(self,memory,jobs):
        script='source "$1"; ndnsf_host_memory_bytes() { printf "%s\\n" '+str(memory)+'; }; '
        script+='ndnsf_host_build_jobs_allowed "$2"'
        return subprocess.run(['bash','-c',script,'policy',str(ROOT/'scripts/host-build-jobs.sh'),jobs],capture_output=True,text=True).returncode

    def test_exact_100gb_does_not_remove_ceiling(self):
        self.assertNotEqual(self.policy(100_000_000_000,'28'),0)

    def test_larger_than_100gb_allows_full_vm_parallelism(self):
        self.assertEqual(self.policy(100_000_000_001,'28'),0)
        self.assertEqual(self.policy(416*1024**3,'128'),0)

    def test_small_hosts_keep_existing_supported_counts(self):
        for jobs in ('1','2','4','6'):self.assertEqual(self.policy(16*1024**3,jobs),0)
        for jobs in ('3','7','28'):self.assertNotEqual(self.policy(16*1024**3,jobs),0)

    def test_large_memory_does_not_accept_invalid_counts(self):
        for jobs in ('0','-1','1.5','2; false','', 'abc'):
            self.assertNotEqual(self.policy(416*1024**3,jobs),0)

    def test_defaults_follow_memory_gate(self):
        helper=str(ROOT/'scripts/host-build-jobs.sh')
        for memory,expected in ((100_000_000_000,'6'),(100_000_000_001,subprocess.check_output(['nproc'],text=True).strip())):
            script='source "$1"; ndnsf_host_memory_bytes() { echo '+str(memory)+'; }; ndnsf_default_host_build_jobs'
            result=subprocess.run(['bash','-c',script,'policy',helper],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),expected)

if __name__=='__main__':unittest.main()
