from __future__ import annotations

import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core import (  # noqa: E402
    AcceleratorMode, AcceleratorPolicy,
)


class AcceleratorPolicyTest(unittest.TestCase):
    def test_cpu_and_auto_modes_are_explicit(self):
        self.assertEqual(
            AcceleratorPolicy(AcceleratorMode.NONE).resolve(("cuda:0",)),
            ("cpu",),
        )
        self.assertEqual(
            AcceleratorPolicy(AcceleratorMode.AUTO).resolve(("cpu",)),
            ("cpu",),
        )

    def test_gpu_required_never_silently_falls_back(self):
        with self.assertRaisesRegex(RuntimeError, "GPU-required"):
            AcceleratorPolicy(require_gpu=True).resolve(("cpu",))

    def test_explicit_subset_is_bounded_by_runtime_visibility(self):
        policy = AcceleratorPolicy(
            mode=AcceleratorMode.EXPLICIT_SUBSET,
            requested=("cuda:1",), require_gpu=True)
        self.assertEqual(policy.resolve(("cuda:0", "cuda:1")), ("cuda:1",))
        with self.assertRaisesRegex(RuntimeError, "not runtime-visible"):
            policy.resolve(("cuda:0",))


if __name__ == "__main__":
    unittest.main()
