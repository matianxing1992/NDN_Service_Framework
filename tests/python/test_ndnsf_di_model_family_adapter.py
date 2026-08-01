from __future__ import annotations

import unittest
from pathlib import Path

from ndnsf_distributed_inference.adapters import (
    InferenceStateClass,
    ModelFamilyAdapter,
    build_llm_text_adapter,
    build_object_detection_adapter,
    build_opaque_container_adapter,
)
from ndnsf_distributed_inference.splitter import SplitCandidate


DIGEST_A = "sha256:" + "a" * 64
DIGEST_B = "sha256:" + "b" * 64


class ModelFamilyAdapterTest(unittest.TestCase):
    def _candidate(self, adapter: ModelFamilyAdapter) -> SplitCandidate:
        model = adapter.describe_model(
            model_name="fixture",
            content_digest=DIGEST_A,
            semantics_digest=DIGEST_B,
        )
        graph = adapter.graph.inspect(model)
        candidates = adapter.splitter.enumerate_candidates(model, graph)
        self.assertTrue(candidates)
        candidates[0].validate_against(graph)
        return candidates[0]

    def test_three_model_families_share_one_candidate_carrier(self):
        llm = build_llm_text_adapter()
        detector = build_object_detection_adapter()
        opaque = build_opaque_container_adapter()

        for adapter in (llm, detector, opaque):
            with self.subTest(adapter=adapter.descriptor.name):
                candidate = self._candidate(adapter)
                self.assertIsInstance(candidate, SplitCandidate)
                self.assertEqual(
                    adapter.composition_digest,
                    adapter.recompute_composition_digest(),
                )
                self.assertEqual(
                    adapter.task.descriptor.input_schema_digest,
                    adapter.descriptor.input_schema_digest,
                )

        llm_classes = {item.state_class for item in llm.state.contracts}
        self.assertIn(InferenceStateClass.EXACT_PREFIX_REUSABLE, llm_classes)
        self.assertEqual(
            {item.state_class for item in detector.state.contracts},
            {InferenceStateClass.STATELESS},
        )

        opaque_graph = opaque.graph.inspect(
            opaque.describe_model("opaque", DIGEST_A, DIGEST_B)
        )
        self.assertFalse(opaque.descriptor.graph_inspectable)
        self.assertFalse(opaque.descriptor.splittable)
        self.assertEqual(len(opaque_graph.nodes), 1)
        self.assertEqual(opaque_graph.legal_cut_edges, ())
        self.assertEqual(len(self._candidate(opaque).execution_plan.roles), 1)

    def test_task_and_state_schemas_are_adapter_owned_and_runner_is_post_plan(self):
        adapter = build_llm_text_adapter()
        encoded = adapter.task.encode_input(
            {"text": "hello"},
            {"maximum_output_units": 8},
        )
        self.assertIsInstance(encoded.payload, bytes)
        self.assertEqual(
            encoded.input_schema_digest,
            adapter.descriptor.input_schema_digest,
        )
        self.assertNotIn("provider", encoded.metadata)

        reusable = next(
            item for item in adapter.state.contracts
            if item.state_class is InferenceStateClass.EXACT_PREFIX_REUSABLE
        )
        self.assertGreater(reusable.maximum_retention_ms, 0)
        self.assertIn("GPU", reusable.allowed_tiers)
        self.assertFalse(reusable.cross_security_domain)

        self.assertTrue(adapter.runner.requires_accepted_selection)
        self.assertFalse(hasattr(adapter.runner, "enumerate_candidates"))
        adapter.validate_pin(
            adapter_descriptor_digest=adapter.descriptor.descriptor_digest,
            composition_digest=adapter.composition_digest,
        )
        with self.assertRaisesRegex(ValueError, "composition pin"):
            adapter.validate_pin(
                adapter_descriptor_digest=adapter.descriptor.descriptor_digest,
                composition_digest=DIGEST_A,
            )
        with self.assertRaisesRegex(ValueError, "accepted Selection"):
            adapter.runner.create(
                accepted_selection_digest="",
                role="stage-0",
                artifacts=(DIGEST_A,),
            )

    def test_base_ndnsf_wire_schema_has_no_adapter_or_llm_fields(self):
        root = Path(__file__).resolve().parents[2]
        core_wire = (
            (root / "ndn-service-framework" / "NDNSFMessages.hpp").read_text()
            + (root / "ndn-service-framework" / "NDNSFMessages.cpp").read_text()
        )
        for forbidden in (
            "ModelFamilyAdapter",
            "InferenceStateContract",
            "EXACT_PREFIX_KV",
            "Qwen",
            "logits",
            "promptTokens",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, core_wire)


if __name__ == "__main__":
    unittest.main()
