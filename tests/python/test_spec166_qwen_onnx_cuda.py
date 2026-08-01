import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
PROVIDER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py"
)


def load_provider_module():
    spec = importlib.util.spec_from_file_location(
        "spec166_llm_provider", PROVIDER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.path.insert(0, str(PROVIDER.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(PROVIDER.parent))
    return module


class FakeOptions:
    def __init__(self):
        self.entries = {}
        self.enable_profiling = False
        self.profile_file_prefix = ""

    def add_session_config_entry(self, key, value):
        self.entries[key] = value


class FakeSession:
    def __init__(self, providers):
        self._providers = [
            item[0] if isinstance(item, tuple) else item
            for item in providers
        ]

    def get_providers(self):
        return list(self._providers)


class QwenOnnxCudaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_provider_module()
        cls.lib = sys.modules["llm_pipeline_lib"]

    def test_typed_tensor_bundle_preserves_utf8_request_identity(self):
        request_id = "campaign-prompt-1-token-7"
        session_id = "session-alpha"
        payload = self.lib._native_tensor_bundle_payload({
            "request_id": self.lib._utf8_text_tensor(request_id),
            "session_id": self.lib._utf8_text_tensor(session_id),
        })
        decoded = self.lib._decode_native_tensor_bundle(payload)

        self.assertEqual(
            self.lib._safe_array_text(decoded["request_id"]), request_id)
        self.assertEqual(
            self.lib._safe_array_text(decoded["session_id"]), session_id)

    def test_timing_marker_is_emitted_with_one_atomic_write(self):
        with mock.patch.object(self.module.os, "write") as write:
            self.module._print_qwen_stage_timing(
                role="/LLM/Pipeline/Stage/2",
                requestId="campaign-token-7",
                device="cuda:0",
            )

        write.assert_called_once()
        fd, payload = write.call_args.args
        self.assertEqual(fd, 1)
        self.assertEqual(
            payload,
            b"LLM_PIPELINE_QWEN_STAGE_TIMING "
            b"role=/LLM/Pipeline/Stage/2 requestId=campaign-token-7 "
            b"device=cuda:0\n",
        )

    def fake_ort(self, available):
        state = {}

        def create(path, *, sess_options=None, providers):
            state.update(
                path=path,
                options=sess_options,
                providers=providers,
            )
            return FakeSession(providers)

        return types.SimpleNamespace(
            get_available_providers=lambda: list(available),
            SessionOptions=FakeOptions,
            InferenceSession=create,
        ), state

    def test_required_cuda_profiles_cuda_first_with_cpu_control_support(self):
        ort, state = self.fake_ort(
            ["CUDAExecutionProvider", "CPUExecutionProvider"])
        with mock.patch.dict(sys.modules, {"onnxruntime": ort}):
            session = self.module._qwen_onnx_session(
                "stage.onnx",
                device="cuda:0",
                require_cuda=True,
                profile_prefix="/output/stage-0",
            )
        self.assertEqual(
            state["providers"][0][0], "CUDAExecutionProvider")
        self.assertEqual(state["providers"][1], "CPUExecutionProvider")
        self.assertNotIn(
            "session.disable_cpu_ep_fallback", state["options"].entries)
        self.assertTrue(state["options"].enable_profiling)
        self.assertEqual(
            state["options"].profile_file_prefix, "/output/stage-0")
        self.assertEqual(
            self.module._qwen_onnx_session_placement(session),
            ("cuda:0", False),
        )

    def test_required_cuda_rejects_missing_provider(self):
        ort, _ = self.fake_ort(["CPUExecutionProvider"])
        with mock.patch.dict(sys.modules, {"onnxruntime": ort}):
            with self.assertRaisesRegex(
                    RuntimeError, "CUDAExecutionProvider is unavailable"):
                self.module._qwen_onnx_session(
                    "stage.onnx", device="cuda:0", require_cuda=True)

    def test_cpu_default_remains_available_for_local_gate(self):
        ort, state = self.fake_ort(
            ["CUDAExecutionProvider", "CPUExecutionProvider"])
        with mock.patch.dict(sys.modules, {"onnxruntime": ort}):
            session = self.module._qwen_onnx_session(
                "stage.onnx", device="cpu", require_cuda=False)
        self.assertEqual(state["providers"], ["CPUExecutionProvider"])
        self.assertEqual(
            self.module._qwen_onnx_session_placement(session),
            ("cpu", True),
        )

    def profile_session(self, events):
        directory = tempfile.TemporaryDirectory()
        path = Path(directory.name) / "profile.json"
        path.write_text(json.dumps(events), encoding="utf-8")
        session = types.SimpleNamespace(end_profiling=lambda: str(path))
        return directory, session

    @staticmethod
    def node_event(provider, op_name, name=None, *, input_types=None,
                   output_types=None):
        args = {"provider": provider, "op_name": op_name}
        if provider == "CPUExecutionProvider":
            args["input_type_shape"] = (
                input_types if input_types is not None else [{"int64": []}]
            )
            args["output_type_shape"] = (
                output_types if output_types is not None else [{"int64": []}]
            )
        return {
            "cat": "Node",
            "name": name or f"{op_name}_kernel_time",
            "args": args,
        }

    def test_profile_accepts_cuda_core_and_allowlisted_cpu_control_ops(self):
        events = [
            self.node_event("CUDAExecutionProvider", "MatMul"),
            self.node_event("CUDAExecutionProvider", "Softmax"),
            self.node_event(
                "CUDAExecutionProvider", "SimplifiedLayerNormalization"),
            self.node_event("CPUExecutionProvider", "Shape"),
            self.node_event(
                "CPUExecutionProvider", "Reshape", "shape_reshape_kernel_time"),
            self.node_event(
                "CPUExecutionProvider", "Gather", "shape_gather_kernel_time",
                input_types=[{"int64": [3]}, {"int64": []}],
                output_types=[{"int64": []}],
            ),
        ]
        directory, session = self.profile_session(events)
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "PASS")
        self.assertEqual(summary["schemaVersion"],
                         "ndnsf-qwen-onnx-ep-profile-v2")
        self.assertEqual(summary["cpuControlOps"],
                         ["Gather", "Reshape", "Shape"])
        self.assertEqual(summary["missingCudaCoreGroups"], [])
        self.assertEqual(
            summary["cpuNodeNamesByOp"]["Reshape"],
            ["shape_reshape_kernel_time"],
        )
        self.assertEqual(summary["violations"], [])

    def test_profile_rejects_core_compute_on_cpu(self):
        events = [
            self.node_event("CUDAExecutionProvider", "MatMul"),
            self.node_event("CUDAExecutionProvider", "Softmax"),
            self.node_event("CUDAExecutionProvider", "ReduceMean"),
            self.node_event("CPUExecutionProvider", "MatMul"),
        ]
        directory, session = self.profile_session(events)
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "FAIL")
        self.assertEqual(summary["cpuCoreOps"], ["MatMul"])
        self.assertTrue(any(
            item.startswith("UNKNOWN_CPU_OPS:")
            for item in summary["violations"]))
        self.assertTrue(any(
            item.startswith("CPU_CORE_OPS:")
            for item in summary["violations"]))

    def test_profile_rejects_missing_cuda_core_evidence(self):
        events = [
            self.node_event("CUDAExecutionProvider", "MatMul"),
            self.node_event("CPUExecutionProvider", "Shape"),
        ]
        directory, session = self.profile_session(events)
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "FAIL")
        self.assertEqual(summary["missingCudaCoreGroups"],
                         ["normalization", "softmax"])

    def test_profile_rejects_float_tensor_on_allowlisted_cpu_operator(self):
        events = [
            self.node_event("CUDAExecutionProvider", "MatMul"),
            self.node_event("CUDAExecutionProvider", "Softmax"),
            self.node_event(
                "CUDAExecutionProvider", "SimplifiedLayerNormalization"),
            self.node_event(
                "CPUExecutionProvider", "Add", "model_add_kernel_time",
                input_types=[{"float": [1, 8, 1024]}],
                output_types=[{"float": [1, 8, 1024]}],
            ),
        ]
        directory, session = self.profile_session(events)
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "FAIL")
        self.assertEqual(summary["cpuNonControlTensorNodes"],
                         ["model_add_kernel_time"])
        self.assertTrue(any(
            item.startswith("CPU_NON_CONTROL_TENSORS:")
            for item in summary["violations"]))

    def test_profile_rejects_missing_cpu_tensor_type_evidence(self):
        events = [
            self.node_event("CUDAExecutionProvider", "MatMul"),
            self.node_event("CUDAExecutionProvider", "Softmax"),
            self.node_event("CUDAExecutionProvider", "ReduceMean"),
            {
                "cat": "Node",
                "name": "shape_add_kernel_time",
                "args": {
                    "provider": "CPUExecutionProvider",
                    "op_name": "Add",
                },
            },
        ]
        directory, session = self.profile_session(events)
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "FAIL")
        self.assertEqual(summary["cpuMissingTypeShapeNodes"],
                         ["shape_add_kernel_time"])

    def test_profile_rejects_empty_node_evidence(self):
        directory, session = self.profile_session([])
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "FAIL")
        self.assertIn("EMPTY_NODE_PROFILE", summary["violations"])

    def test_profile_rejects_missing_cpu_node_name(self):
        events = [
            self.node_event("CUDAExecutionProvider", "MatMul"),
            self.node_event("CUDAExecutionProvider", "Softmax"),
            self.node_event("CUDAExecutionProvider", "ReduceMean"),
            {
                "cat": "Node",
                "args": {
                    "provider": "CPUExecutionProvider",
                    "op_name": "Shape",
                },
            },
        ]
        directory, session = self.profile_session(events)
        with directory:
            summary = self.module._qwen_onnx_profile_summary(session)
        self.assertEqual(summary["state"], "FAIL")
        self.assertIn("MISSING_CPU_NODE_NAME", summary["violations"])


if __name__ == "__main__":
    unittest.main()
