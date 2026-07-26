from __future__ import annotations

import unittest

from ndnsf_distributed_inference.sdk.worker import decode_worker_envelope, encode_worker_envelope


class LeastInputTest(unittest.TestCase):
    def test_sensitive_payload_is_removed_recursively(self):
        wire = encode_worker_envelope("model_variant", {
            "prompt": "secret", "tenant": "a", "nested": {"token": "x", "score": 1}}, 3)
        decoded = decode_worker_envelope(wire)
        self.assertEqual(decoded["payload"], {"nested": {"score": 1}, "tenant": "a"})

    def test_invalid_epoch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "decision epoch"):
            encode_worker_envelope("cache", {}, 0)


if __name__ == "__main__": unittest.main()
