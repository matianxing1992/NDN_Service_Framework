"""Typed tensor wire codec preserves request and session identity."""

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_LIB = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py")


class QwenTensorBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("spec166_tensor_codec", PIPELINE_LIB)
        cls.lib = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.lib
        spec.loader.exec_module(cls.lib)

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
