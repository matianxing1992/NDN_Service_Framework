from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core.decision_validation import validate_target
from ndnsf_distributed_inference.core.ports import ExecutionTargetProposal
from ndnsf_distributed_inference.sdk.adapters import RunnerAdapterRegistry


class Adapter:
    name = "onnx-gpu"; version = "1"
    def supports(self, target): return target.device == "cuda"
    def create_runner(self, target, artifacts): return (target, tuple(artifacts))


class ModelAdapterTest(unittest.TestCase):
    def test_selected_adapter_only_is_created(self):
        registry = RunnerAdapterRegistry(); registry.register(Adapter())
        target = ExecutionTargetProposal("decode", "/gpu", "onnx-gpu", "cuda")
        validate_target(target, registry.snapshot())
        runner = registry.create("onnx-gpu", target, ("sha256:model",))
        self.assertEqual(runner[0], target)

    def test_duplicate_and_incompatible_adapter_rejected(self):
        registry = RunnerAdapterRegistry(); registry.register(Adapter())
        with self.assertRaisesRegex(ValueError, "duplicate"):
            registry.register(Adapter())
        with self.assertRaisesRegex(ValueError, "does not support"):
            registry.create("onnx-gpu", ExecutionTargetProposal("r", "/p", "onnx-gpu", "cpu"), ())


if __name__ == "__main__": unittest.main()
