"""Memory boundaries and malformed job counts for the shared host build policy."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts/host-build-jobs.sh"


class HostBuildJobPolicy(unittest.TestCase):
    def run_policy(self, memory, *command):
        script = '''
source "$1"
test_memory_bytes=$2
shift 2
ndnsf_host_memory_bytes() {
    [[ -n $test_memory_bytes ]] || return 1
    printf '%s\\n' "$test_memory_bytes"
}
nproc() { printf '28\\n'; }
"$@"
'''
        return subprocess.run(
            ["bash", "-c", script, "policy", str(HELPER),
             "" if memory is None else str(memory), *command],
            capture_output=True, text=True,
        )

    def test_defaults_and_admission_at_memory_boundaries(self):
        tiers = (
            (None, "6", {"1", "2", "4", "6"}),
            (12_000_000_000, "4", {"1", "2", "4"}),
            (12_000_000_001, "6", {"1", "2", "4", "6"}),
            (16_000_000_000, "6", {"1", "2", "4", "6"}),
            (16_000_000_001, "6", {"1", "2", "4", "6"}),
            (99_999_999_999, "6", {"1", "2", "4", "6"}),
            (100_000_000_000, "28", {"1", "2", "3", "4", "6", "7", "28", "128"}),
        )
        for memory, expected, allowed in tiers:
            with self.subTest(memory=memory):
                result = self.run_policy(memory, "ndnsf_default_host_build_jobs")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
            for jobs in ("1", "2", "3", "4", "6", "7", "28", "128"):
                with self.subTest(memory=memory, jobs=jobs):
                    result = self.run_policy(memory, "ndnsf_host_build_jobs_allowed", jobs)
                    self.assertEqual(result.returncode == 0, jobs in allowed)

    def test_large_memory_does_not_accept_invalid_counts(self):
        for jobs in ("0", "-1", "1.5", "2; false", "", "abc"):
            with self.subTest(jobs=jobs):
                result = self.run_policy(416 * 1024 ** 3, "ndnsf_host_build_jobs_allowed", jobs)
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
