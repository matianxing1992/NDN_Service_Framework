from __future__ import annotations

import importlib
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

from ndnsf_distributed_inference.planner.defaults import DefaultOptimizationSuite
from ndnsf_distributed_inference.sdk.contract_tests import validate_suite_contract
from ndnsf_distributed_inference.sdk.loader import discover_optimizers, distribution_record_digest
from ndnsf_distributed_inference.sdk.suite import OptimizationSuite
from ndnsf_distributed_inference.sdk.contracts import (
    AdmissionRequest, CacheRequest, DeploymentRequest, ExecutionTargetRequest,
    ExecutionTuningRequest, ModelVariantRequest, PartitionRequest,
    ProviderAssignmentRequest, RecoveryRequest, SchedulingRequest,
)


ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SRC = ROOT / "tests/fixtures/ndnsf-di-external-optimizer/src"


class FakeDistribution:
    metadata = {"Name": "fixture-dist"}
    version = "1"
    files = ("a.py", "RECORD")


class FakeEntry:
    name = "fixture"; dist = FakeDistribution()
    def load(self): return lambda: "loaded"


class FakeEntries(list):
    def select(self, *, group): return self if group == "ndnsf_di.optimizers" else []


class ExternalOptimizerSdkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(FIXTURE_SRC))
        cls.external = importlib.import_module("ndnsf_di_external_optimizer")

    @classmethod
    def tearDownClass(cls):
        sys.path.remove(str(FIXTURE_SRC))

    def test_direct_ten_policy_suite_uses_public_contract(self):
        suite = self.external.create_suite()
        validate_suite_contract(suite)
        self.assertEqual(suite.name, "external-fixture")

    def test_partial_suite_resolves_named_defaults(self):
        full = self.external.create_suite()
        partial = OptimizationSuite(
            {"model_variant": full.policy("model_variant")}, name="partial",
            version="1", state_digest="sha256:" + "p" * 64)
        resolved, used_default = partial.resolve(DefaultOptimizationSuite())
        validate_suite_contract(resolved)
        self.assertFalse(used_default["model_variant"])
        self.assertTrue(all(used_default[kind] for kind in used_default
                            if kind != "model_variant"))

    def test_entry_point_requires_exact_distribution_allowlist(self):
        distribution = FakeDistribution()
        digest = distribution_record_digest(distribution)
        with patch("ndnsf_distributed_inference.sdk.loader.metadata.entry_points",
                   return_value=FakeEntries([FakeEntry()])):
            self.assertEqual(discover_optimizers([]), {})
            discovered = discover_optimizers([("fixture-dist", "1", digest)])
        self.assertEqual(discovered["fixture"](), "loaded")

    def test_ten_requests_are_distinct_runtime_types(self):
        request_types = (
            ModelVariantRequest, PartitionRequest, DeploymentRequest,
            ProviderAssignmentRequest, SchedulingRequest, AdmissionRequest,
            ExecutionTuningRequest, CacheRequest, RecoveryRequest,
            ExecutionTargetRequest,
        )
        self.assertEqual(len(set(request_types)), 10)

    def test_defaults_replace_build_is_complete_and_deterministic(self):
        custom = self.external.create_suite().policy("provider_assignment")
        first = OptimizationSuite.defaults().replace(
            provider_assignment=custom).build(name="lab", version="1")
        second = OptimizationSuite.defaults().replace(
            provider_assignment=custom).build(name="lab", version="1")
        validate_suite_contract(first)
        self.assertEqual(first.state_digest, second.state_digest)
        self.assertIs(first.policy("provider_assignment"), custom)
        self.assertEqual(first.policy_names(), tuple(first.descriptor()[
            "policies"][index]["kind"] for index in range(10)))


if __name__ == "__main__": unittest.main()
