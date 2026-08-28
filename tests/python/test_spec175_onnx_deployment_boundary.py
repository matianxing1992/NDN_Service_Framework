import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROVIDER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py"
)


def load_provider_module():
    spec = importlib.util.spec_from_file_location(
        "spec175_llm_provider", PROVIDER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    # The production MiniNDN runner supplies the repo client through the
    # sealed source bundle.  Keep direct host imports faithful to that closure
    # instead of relying on a developer's editable installation.
    wrapper = ROOT / "NDNSF-DistributedRepo/pythonWrapper"
    sys.path.insert(0, str(wrapper))
    sys.path.insert(0, str(PROVIDER.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(PROVIDER.parent))
        sys.path.remove(str(wrapper))
    return module


class FakeInput:
    def __init__(self, name, shape):
        self.name = name
        self.shape = shape


class FakeOnnxSession:
    def __init__(self):
        self.calls = []

    def get_providers(self):
        return ["CUDAExecutionProvider", "CPUExecutionProvider"]

    def get_inputs(self):
        return [
            FakeInput("input_ids", [1, 1]),
            FakeInput("attention_mask", [1, 1]),
            FakeInput("position_ids", [1, 1]),
            FakeInput("hidden_states", [1, 1, 16]),
            FakeInput("past_key.0", [1, 2, "past", 8]),
        ]

    def run(self, output_names, feed):
        self.calls.append(feed)
        return []


class Spec175OnnxDeploymentBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_provider_module()

    def test_local_artifact_metadata_is_onnx_for_onnx_runtime(self):
        with tempfile.NamedTemporaryFile(suffix=".onnx") as artifact:
            parsed = self.module._parse_selection_local_artifacts(
                [f"/LLM/Pipeline/Stage/0={artifact.name}"],
                runtime=self.module.QWEN_ONNX_RUNTIME,
            )
        item = parsed["/LLM/Pipeline/Stage/0"]
        self.assertEqual(item["kind"], "onnx-model")
        self.assertEqual(
            item["metadata"]["runtime"], self.module.QWEN_ONNX_RUNTIME)

    def test_onnx_selection_handle_warms_assigned_session(self):
        session = FakeOnnxSession()
        handle = self.module._QwenOnnxRuntimeHandle(
            session, {"hiddenSize": 16})
        self.assertEqual(handle.ndnsf_execution_device, "cuda:0")
        self.assertFalse(handle.ndnsf_cpu_fallback)
        self.assertTrue(self.module._warm_qwen_onnx_runtime(handle))
        self.assertEqual(len(session.calls), 1)
        self.assertEqual(session.calls[0]["past_key.0"].shape, (1, 2, 0, 8))

    def test_qwen35_warmup_uses_four_plane_position_ids(self):
        session = FakeOnnxSession()
        handle = self.module._QwenOnnxRuntimeHandle(
            session, {"hiddenSize": 16, "modelType": "qwen3_5"})
        self.assertTrue(self.module._warm_qwen_onnx_runtime(handle))
        self.assertEqual(session.calls[0]["position_ids"].shape, (4, 1, 1))

    def test_transformers_runtime_is_rejected_for_deployed_selection(self):
        args = types.SimpleNamespace(
            runtime=self.module.QWEN_TRANSFORMERS_RUNTIME,
            deployment_control_service="/NDNSF/DI/DEPLOYMENT",
            selection_dataflow_v2=False,
            selection_dataflow_v3=False,
            require_onnx_runtime=True,
        )
        with self.assertRaisesRegex(RuntimeError, "must use qwen-onnx"):
            self.module._preflight_qwen_runtime(args)

    def test_bfloat16_is_not_coerced_to_float32(self):
        with self.assertRaisesRegex(RuntimeError, "BFLOAT16_UNSUPPORTED"):
            self.module._onnx_numpy_dtype("tensor(bfloat16)", None)


if __name__ == "__main__":
    unittest.main()
