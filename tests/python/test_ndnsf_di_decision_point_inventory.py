from __future__ import annotations

import json
from pathlib import Path
import unittest

from ndnsf_distributed_inference.sdk.contracts import POLICY_KINDS


ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "specs/111-ndnsf-di-core-app-separation/contracts/decision-point-inventory.json"
PLANNER = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/planner"


class DecisionPointInventoryTest(unittest.TestCase):
    def setUp(self):
        self.payload = json.loads(INVENTORY.read_text())
        self.entries = self.payload["entries"]

    def test_inventory_is_closed_classified_and_has_no_native_only_policy(self):
        self.assertTrue(self.payload["closed"])
        self.assertEqual(len(self.entries), 12)
        self.assertEqual(len({entry["id"] for entry in self.entries}), 12)
        for entry in self.entries:
            self.assertIn(entry["classification"], {"policy", "mechanism-adapter"})
            self.assertFalse(entry["nativeOnly"])
            for field in ("sources", "port", "owner", "default", "validator", "evidence"):
                self.assertTrue(entry[field], f"{entry['id']} missing {field}")

    def test_all_ten_ports_and_runner_are_classified(self):
        ports = {entry["port"] for entry in self.entries}
        expected = {
            "ModelVariantPolicy", "PartitionPlanner", "DeploymentPolicy",
            "ProviderAssignmentPolicy", "SchedulingPolicy", "AdmissionPolicy",
            "ExecutionTuningPolicy", "CachePolicy", "RecoveryPolicy",
            "ExecutionTargetPolicy", "RunnerAdapter",
        }
        self.assertEqual(ports, expected)
        self.assertEqual(len(POLICY_KINDS), 10)

    def test_every_reference_policy_module_is_in_inventory(self):
        classified = "\n".join(entry["default"] for entry in self.entries)
        modules = {path.stem for path in PLANNER.glob("*_policy.py")}
        expected_modules = {
            "admission_policy", "cache_policy", "cost_policy", "deployment_policy",
            "execution_target_policy", "execution_tuning_policy", "fixed_policy",
            "model_variant_policy", "provider_assignment_policy", "recovery_policy",
            "scheduling_policy", "split_policy",
        }
        self.assertEqual(modules, expected_modules)
        self.assertIn("CostProviderAssignmentPolicy", classified)


if __name__ == "__main__": unittest.main()
