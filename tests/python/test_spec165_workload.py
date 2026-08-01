import unittest
from pathlib import Path

from Experiments.ndnsf_validation.workload import canonical_workload


class WorkloadContractTests(unittest.TestCase):
    def test_canonical_profile_without_disk_scan(self):
        workload = canonical_workload(
            snapshot=Path("/nonexistent/frozen-model"),
            include_snapshot_manifest=False,
        )
        self.assertEqual(len(workload["prompts"]), 2)
        self.assertEqual(workload["warmupPerPrompt"], 1)
        self.assertEqual(workload["measuredPerPrompt"], 3)
        self.assertGreaterEqual(workload["minimumGeneratedTokens"], 8)
        self.assertTrue(workload["workloadDigest"].startswith("sha256:"))

    def test_weaker_profile_is_rejected(self):
        with self.assertRaises(ValueError):
            canonical_workload(
                snapshot=Path("/unused"),
                include_snapshot_manifest=False,
                prompts=("only one",),
            )
        with self.assertRaises(ValueError):
            canonical_workload(
                snapshot=Path("/unused"),
                include_snapshot_manifest=False,
                minimum_generated_tokens=1,
            )


if __name__ == "__main__":
    unittest.main()
