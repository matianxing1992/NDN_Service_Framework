#!/usr/bin/env python3
"""Spec 168: numerical tie acceptance is explicit and fail-closed."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py"
SPEC = importlib.util.spec_from_file_location("spec168_llm_pipeline_lib", MODULE_PATH)
assert SPEC and SPEC.loader
pipeline = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pipeline
SPEC.loader.exec_module(pipeline)


class NumericEquivalenceTests(unittest.TestCase):
    def _call(self, policy=None):
        return pipeline.run_full_qwen_generation(
            input_token_ids=(10, 11),
            max_new_tokens=8,
            eos_token_ids=(99,),
            generation_id="spec168-test",
            generation_call=lambda _context, _limit, _request: {
                "generatedTokenIds": [1, 2, 34859, 99],
            },
            expected_token_ids=(1, 2, 104399, 99),
            decode=lambda values: "answer " + ",".join(str(item) for item in values),
            numeric_equivalence=policy,
        )

    def test_registered_tie_is_accepted_but_not_called_exact(self):
        result = self._call({
            "classification": "NUMERICALLY_EQUIVALENT_DIVERGENCE",
            "evidenceDigest": "sha256:" + "a" * 64,
            "firstDivergence": {"tokenIndex": 2},
            "allowedTokenIds": [34859],
        })
        self.assertEqual(result.status, "OK")
        self.assertFalse(result.exact_reference_match)
        self.assertEqual(
            result.reference_acceptance,
            "NUMERICALLY_EQUIVALENT_DIVERGENCE",
        )
        self.assertEqual(result.to_dict()["referenceEvidenceDigest"],
                         "sha256:" + "a" * 64)

    def test_unregistered_mismatch_still_fails(self):
        result = self._call()
        self.assertEqual(result.status, "FAILED")
        self.assertEqual(result.stop_reason, "TOKEN_MISMATCH")
        self.assertEqual(result.reference_acceptance, "EXACT")

    def test_policy_cannot_skip_a_prefix_mismatch(self):
        result = pipeline.run_full_qwen_generation(
            input_token_ids=(10,),
            max_new_tokens=8,
            eos_token_ids=(99,),
            generation_id="spec168-prefix-test",
            generation_call=lambda _context, _limit, _request: {
                "generatedTokenIds": [77, 34859, 99],
            },
            expected_token_ids=(1, 2, 104399, 99),
            decode=lambda values: "answer",
            numeric_equivalence={
                "classification": "NUMERICALLY_EQUIVALENT_DIVERGENCE",
                "evidenceDigest": "sha256:" + "a" * 64,
                "firstDivergence": {"tokenIndex": 2},
                "allowedTokenIds": [34859],
            },
        )
        self.assertEqual(result.status, "FAILED")
        self.assertEqual(result.stop_reason, "TOKEN_MISMATCH")


if __name__ == "__main__":
    unittest.main()
