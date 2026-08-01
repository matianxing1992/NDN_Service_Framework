from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("spec160_llm_pipeline_lib", MODULE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class QwenStageDeviceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def test_cpu_remains_default_for_existing_callers(self):
        device = self.module.resolve_qwen_execution_device()
        self.assertEqual(device.type, "cpu")

    def test_require_cuda_rejects_cpu(self):
        with self.assertRaisesRegex(
            RuntimeError, "QWEN_STAGE_CPU_FALLBACK_FORBIDDEN"
        ):
            self.module.resolve_qwen_execution_device(
                "cpu", require_cuda=True
            )

    def test_unavailable_cuda_fails_closed(self):
        import torch

        if torch.cuda.is_available():
            self.skipTest("host CUDA is available")
        with self.assertRaisesRegex(RuntimeError, "QWEN_STAGE_CUDA_UNAVAILABLE"):
            self.module.resolve_qwen_execution_device("cuda:0")


if __name__ == "__main__":
    unittest.main()
