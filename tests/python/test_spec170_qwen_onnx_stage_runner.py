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


class Spec170QwenOnnxStageRunnerTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
