from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]


def load_validator():
    path = REPO / "tools" / "ndnsf-di" / "validate_spec109.py"
    spec = importlib.util.spec_from_file_location("spec109_exact_validator", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec109ExactTokensTest(unittest.TestCase):
    def test_candidate_matches_oracle_for_every_output_length(self):
        validator = load_validator()
        for length in (1, 2, 32):
            expected = list(range(100, 100 + length))
            value = {
                "inputTokenIds": [1, 2], "outputTokenIds": expected,
                "referenceOutputTokenIds": expected, "exactMatch": True,
                "checkpoints": [{"name": "hidden", "kind": "hidden", "rtol": 0.01,
                                 "atol": 0.001, "maxAbsError": 0.0,
                                 "maxRelError": 0.0, "pass": True}],
            }
            self.assertEqual(validator.validate_correctness(value)["status"], "PASS")
        mismatch = dict(value, outputTokenIds=expected[:-1] + [999])
        with self.assertRaisesRegex(validator.ValidationError, "CORRECTNESS_TOKEN_MISMATCH"):
            validator.validate_correctness(mismatch)


if __name__ == "__main__":
    unittest.main()
