from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core.decision_validation import validate_model_selection
from ndnsf_distributed_inference.core.ports import ModelCandidate
from ndnsf_distributed_inference.planner.model_variant_policy import ExactOrCompatibleModelVariantPolicy
from support.spec111_policy import request
from ndnsf_distributed_inference.llm_stub_planner import qwen_model_candidates


def model(name: str, exact: bool, precision: str = "fp16") -> ModelCandidate:
    return ModelCandidate("qwen", name, exact, precision, "sha256:" + name.encode().hex().ljust(64, "0")[:64])


class ModelVariantPolicyTest(unittest.TestCase):
    def test_exact_candidate_wins_over_compatible_replacement(self):
        candidates = (model("qwen-7b-int8", False, "int8"), model("qwen-7b-fp16", True))
        result = ExactOrCompatibleModelVariantPolicy().propose(request(candidates))
        self.assertEqual(result.value.variant_id, "qwen-7b-fp16")
        validate_model_selection(result.value, candidates, exact_required=True)

    def test_out_of_set_and_semantic_replacement_rejected(self):
        allowed = (model("qwen-7b-fp16", True),)
        with self.assertRaisesRegex(ValueError, "outside"):
            validate_model_selection(model("qwen-14b-fp16", True), allowed, exact_required=True)
        with self.assertRaisesRegex(ValueError, "exact model"):
            validate_model_selection(model("qwen-7b-int8", False), (model("qwen-7b-int8", False),), exact_required=True)

    def test_qwen_registry_is_bounded_and_exact_model_is_preserved(self):
        candidates = qwen_model_candidates(exact_model_id="Qwen2.5-32B", max_candidates=14)
        self.assertEqual(len(candidates), 14)
        exact = [item for item in candidates if item.exact_semantics]
        self.assertEqual({item.model_id for item in exact}, {"Qwen2.5-32B"})


if __name__ == "__main__": unittest.main()
