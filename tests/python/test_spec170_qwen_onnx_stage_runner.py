from __future__ import annotations

from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_DIR = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"
sys.path.insert(0, str(PIPELINE_DIR))

from llm_pipeline_lib import (  # noqa: E402
    _decode_native_tensor_bundle,
    encode_qwen_pipeline_context,
    run_qwen_onnx_stage,
)


class _FakeSession:
    def __init__(self, output_names: tuple[str, ...], values: tuple[np.ndarray, ...]):
        self._inputs = (
            SimpleNamespace(name="input_ids"),
            SimpleNamespace(name="attention_mask"),
            SimpleNamespace(name="position_ids"),
            SimpleNamespace(name="past_key.0", shape=(1, 1, "past_seq", 2)),
            SimpleNamespace(name="past_value.0", shape=(1, 1, "past_seq", 2)),
        )
        self._outputs = tuple(SimpleNamespace(name=name) for name in output_names)
        self._values = values
        self.feed = None

    def get_inputs(self):
        return self._inputs

    def get_outputs(self):
        return self._outputs

    def run(self, _unused, feed):
        self.feed = feed
        return self._values


class _StatefulSession:
    def __init__(self):
        def value(name, shape, type_name="tensor(float)"):
            return SimpleNamespace(name=name, shape=shape, type=type_name)

        self._inputs = (
            value("input_ids", [1, "tokens"], "tensor(int64)"),
            value("attention_mask", [1, "prefix"], "tensor(int64)"),
            value("position_ids", [4, 1, "tokens"], "tensor(int64)"),
            value("attention_kv_in", [1, "past", 4]),
            value("recurrent_state_in", [1, 4]),
            value("convolution_state_in", [1, 4]),
        )
        self._outputs = tuple(value(name, []) for name in (
            "hidden_states_out", "attention_kv_out",
            "recurrent_state_out", "convolution_state_out"))
        self.feeds = []

    def get_inputs(self):
        return self._inputs

    def get_outputs(self):
        return self._outputs

    def run(self, _unused, feed):
        self.feeds.append({key: np.asarray(value).copy()
                           for key, value in feed.items()})
        tokens = int(feed["input_ids"].shape[1])
        prefix = int(feed["attention_mask"].shape[1])
        return (
            np.zeros((1, tokens, 4), dtype=np.float32),
            np.ones((1, prefix, 4), dtype=np.float32),
            np.ones((1, 4), dtype=np.float32) * len(self.feeds),
            np.ones((1, 4), dtype=np.float32) * (10 + len(self.feeds)),
        )


class _FakeOrtValue:
    def __init__(self, value: np.ndarray, device: str) -> None:
        self.value = np.asarray(value)
        self._device = device
        self.numpy_calls = 0

    def device_name(self):
        return self._device

    def numpy(self):
        self.numpy_calls += 1
        return self.value


class _FakeIoBinding:
    def __init__(self, session) -> None:
        self.session = session
        self.cpu_inputs = {}
        self.ort_inputs = {}
        self.output_bindings = []
        self.outputs = []

    def bind_cpu_input(self, name, value):
        self.cpu_inputs[name] = np.asarray(value)

    def bind_ortvalue_input(self, name, value):
        self.ort_inputs[name] = value

    def bind_output(self, name, device_type="cpu", device_id=0):
        self.output_bindings.append((name, device_type, device_id))

    def get_outputs(self):
        return list(self.outputs)


class _StatefulCudaSession(_StatefulSession):
    def __init__(self):
        super().__init__()
        self.bindings = []
        self.state_outputs = []

    def get_providers(self):
        return ["CUDAExecutionProvider", "CPUExecutionProvider"]

    def io_binding(self):
        binding = _FakeIoBinding(self)
        self.bindings.append(binding)
        return binding

    def run_with_iobinding(self, binding):
        token_count = int(binding.cpu_inputs["input_ids"].shape[1])
        prefix_count = int(binding.cpu_inputs["attention_mask"].shape[1])
        values = {
            "hidden_states_out": _FakeOrtValue(
                np.zeros((1, token_count, 4), dtype=np.float32), "cpu"),
            "attention_kv_out": _FakeOrtValue(
                np.ones((1, prefix_count, 4), dtype=np.float32), "cuda"),
            "recurrent_state_out": _FakeOrtValue(
                np.ones((1, 4), dtype=np.float32), "cuda"),
            "convolution_state_out": _FakeOrtValue(
                np.ones((1, 4), dtype=np.float32), "cuda"),
        }
        binding.outputs = [values[name] for name, _, _ in binding.output_bindings]
        self.state_outputs.append(values)


class Spec170QwenOnnxStageRunnerTest(unittest.TestCase):
    def test_stateful_cuda_qwen_keeps_state_as_device_ortvalues(self):
        session = _StatefulCudaSession()
        state = {}
        timing = {}
        metadata = {
            "stageIndex": 0,
            "stageCount": 3,
            "layerRange": {"start": 0, "endExclusive": 21},
            "hiddenSize": 4,
            "modelType": "qwen3_5",
            "contextLength": 16,
            "sequencePolicy": "stateful-prefill-decode-v1",
            "stateInputNames": [
                "attention_kv_in", "recurrent_state_in",
                "convolution_state_in"],
            "stateOutputNames": [
                "attention_kv_out", "recurrent_state_out",
                "convolution_state_out"],
            "tensorContracts": {
                "attention_kv_in": {"initialShape": [1, 0, 4]},
                "recurrent_state_in": {"initialShape": [1, 4]},
                "convolution_state_in": {"initialShape": [1, 4]},
            },
        }
        first = run_qwen_onnx_stage(
            encode_qwen_pipeline_context(
                [[11, 12, 13]], attention_mask=[[1, 1, 1]],
                model_type="qwen3_5", request_id="cuda-stateful"),
            role="/LLM/Pipeline/Stage/0", stages=3,
            session=session, metadata=metadata, state=state, timing=timing)
        self.assertEqual(
            _decode_native_tensor_bundle(first)["hidden_states"].shape,
            (1, 3, 4))
        self.assertEqual(timing["state_storage"], "device")
        self.assertEqual(timing["state_host_round_trip_bytes"], 0)
        for name in (
                "attention_kv_in", "recurrent_state_in",
                "convolution_state_in"):
            self.assertEqual(state[name].device_name(), "cuda")
        for value in session.state_outputs[0].values():
            expected = 1 if value.device_name() == "cpu" else 0
            self.assertEqual(value.numpy_calls, expected)

        run_qwen_onnx_stage(
            encode_qwen_pipeline_context(
                [[11, 12, 13, 14]], attention_mask=[[1, 1, 1, 1]],
                model_type="qwen3_5", request_id="cuda-stateful",
                context_epoch=1),
            role="/LLM/Pipeline/Stage/0", stages=3,
            session=session, metadata=metadata, state=state, timing={})
        self.assertEqual(
            set(session.bindings[1].ort_inputs),
            {"attention_kv_in", "recurrent_state_in", "convolution_state_in"})

    def test_stateful_qwen_prefill_then_decode_reuses_all_state_families(self):
        session = _StatefulSession()
        state = {}
        metadata = {
            "stageIndex": 0,
            "stageCount": 3,
            "layerRange": {"start": 0, "endExclusive": 21},
            "hiddenSize": 4,
            "modelType": "qwen3_5",
            "contextLength": 16,
            "sequencePolicy": "stateful-prefill-decode-v1",
            "stateInputNames": [
                "attention_kv_in", "recurrent_state_in",
                "convolution_state_in"],
            "stateOutputNames": [
                "attention_kv_out", "recurrent_state_out",
                "convolution_state_out"],
            "tensorContracts": {
                "attention_kv_in": {"initialShape": [1, 0, 4]},
                "recurrent_state_in": {"initialShape": [1, 4]},
                "convolution_state_in": {"initialShape": [1, 4]},
            },
        }
        first = run_qwen_onnx_stage(
            encode_qwen_pipeline_context(
                [[11, 12, 13]], attention_mask=[[1, 1, 1]],
                model_type="qwen3_5", request_id="stateful"),
            role="/LLM/Pipeline/Stage/0", stages=3,
            session=session, metadata=metadata, state=state)
        first_bundle = _decode_native_tensor_bundle(first)
        self.assertEqual(first_bundle["hidden_states"].shape, (1, 3, 4))
        self.assertEqual(session.feeds[0]["attention_kv_in"].shape, (1, 0, 4))
        self.assertEqual(state["__ndnsf_prefix_token_count__"], 3)

        second = run_qwen_onnx_stage(
            encode_qwen_pipeline_context(
                [[11, 12, 13, 14]], attention_mask=[[1, 1, 1, 1]],
                model_type="qwen3_5", request_id="stateful",
                context_epoch=1),
            role="/LLM/Pipeline/Stage/0", stages=3,
            session=session, metadata=metadata, state=state)
        second_bundle = _decode_native_tensor_bundle(second)
        self.assertEqual(second_bundle["hidden_states"].shape, (1, 1, 4))
        self.assertEqual(session.feeds[1]["input_ids"].tolist(), [[14]])
        self.assertEqual(session.feeds[1]["attention_mask"].shape, (1, 4))
        self.assertEqual(session.feeds[1]["position_ids"].shape, (4, 1, 1))
        np.testing.assert_array_equal(
            session.feeds[1]["attention_kv_in"],
            np.ones((1, 3, 4), dtype=np.float32))
        self.assertEqual(state["__ndnsf_prefix_token_count__"], 4)

    def test_stateful_qwen_rejects_missing_provider_state(self):
        session = _StatefulSession()
        with self.assertRaisesRegex(ValueError, "requires Provider state"):
            run_qwen_onnx_stage(
                encode_qwen_pipeline_context([[11]]),
                role="/LLM/Pipeline/Stage/0", stages=3,
                session=session,
                metadata={
                    "stageIndex": 0,
                    "stageCount": 3,
                    "layerRange": {"start": 0, "endExclusive": 21},
                    "sequencePolicy": "stateful-prefill-decode-v1",
                    "stateInputNames": [
                        "attention_kv_in", "recurrent_state_in",
                        "convolution_state_in"],
                    "stateOutputNames": [
                        "attention_kv_out", "recurrent_state_out",
                        "convolution_state_out"],
                })

    def test_stage_primary_output_is_selected_by_name(self):
        hidden = np.zeros((1, 3, 4), dtype=np.float32)
        present_key = np.zeros((1, 1, 3, 2), dtype=np.float32)
        present_value = np.ones((1, 1, 3, 2), dtype=np.float32)
        # Deliberately place a cache output before the primary output.  ONNX
        # Runtime output order is not the semantic contract; output names are.
        session = _FakeSession(
            ("present_key.0", "hidden_states_out", "present_value.0"),
            (present_key, hidden, present_value),
        )
        payload = encode_qwen_pipeline_context(
            [[11, 12, 13]], request_id="spec170-output-name")

        result = run_qwen_onnx_stage(
            payload,
            role="/LLM/Pipeline/Stage/0",
            stages=3,
            session=session,
            metadata={
                "stageIndex": 0,
                "stageCount": 3,
                "layerRange": {"start": 0, "endExclusive": 1},
                "hiddenSize": 4,
            },
        )

        decoded = _decode_native_tensor_bundle(result)
        self.assertEqual(decoded["hidden_states"].shape, (1, 3, 4))
        self.assertEqual(decoded["next_layer"].tolist(), [1])
        self.assertEqual(decoded["request_id"].tolist(),
                         list(b"spec170-output-name"))

    def test_qwen35_fixed_context_padding_preserves_active_prefix(self):
        hidden = np.zeros((1, 8, 4), dtype=np.float32)
        present_key = np.zeros((1, 1, 8, 2), dtype=np.float32)
        present_value = np.ones((1, 1, 8, 2), dtype=np.float32)
        session = _FakeSession(
            ("present_key.0", "hidden_states_out", "present_value.0"),
            (present_key, hidden, present_value),
        )
        payload = encode_qwen_pipeline_context(
            [[11, 12, 13]],
            attention_mask=[[1, 1, 1]],
            model_type="qwen3_5",
            request_id="spec170-fixed-context",
        )

        result = run_qwen_onnx_stage(
            payload,
            role="/LLM/Pipeline/Stage/0",
            stages=3,
            session=session,
            metadata={
                "stageIndex": 0,
                "stageCount": 3,
                "layerRange": {"start": 0, "endExclusive": 1},
                "hiddenSize": 4,
                "modelType": "qwen3_5",
                "contextLength": 8,
                "padTokenId": 0,
            },
        )

        self.assertEqual(session.feed["input_ids"].shape, (1, 8))
        self.assertEqual(session.feed["input_ids"].tolist(),
                         [[11, 12, 13, 0, 0, 0, 0, 0]])
        self.assertEqual(session.feed["attention_mask"].tolist(),
                         [[1, 1, 1, 0, 0, 0, 0, 0]])
        self.assertEqual(session.feed["position_ids"].shape, (4, 1, 8))
        decoded = _decode_native_tensor_bundle(result)
        self.assertEqual(decoded["hidden_states"].shape, (1, 8, 4))


if __name__ == "__main__":
    unittest.main()
