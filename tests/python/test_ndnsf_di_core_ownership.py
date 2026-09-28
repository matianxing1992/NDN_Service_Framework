from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CoreOwnershipGateTest(unittest.TestCase):
    def test_generic_opaque_selection_store_has_no_di_semantics(self):
        sources = "\n".join(
            (ROOT / "ndn-service-framework" / name).read_text()
            for name in (
                "GenericSelectionTxnStore.hpp",
                "GenericSelectionTxnStore.cpp",
            ))
        for forbidden in (
                "Qwen", "LLM", "GPU", "KV", "model shard",
                "layer_start", "DISelectionAssignmentV2",
                "SELECTION_DATAFLOW_V2",
        ):
            self.assertNotIn(forbidden, sources)
        self.assertIn("OpaqueSelectionParticipant", sources)
        self.assertIn("commitBlob", sources)

    def test_base_core_only_projects_inline_request_input_for_discovery(self):
        sources = "\n".join(
            path.read_text(errors="replace")
            for path in (ROOT / "ndn-service-framework").glob("*.[ch]pp")
        )
        # Request-scoped confidentiality deliberately keeps the public DI
        # envelope available for provider discovery while removing its inline
        # application bytes. Keep that one projection localized; the core must
        # not parse offers, selections, or model-specific contracts.
        self.assertEqual(
            sources.count("ndnsf-di-request-envelope-v2"), 1,
        )
        service_user = (
            ROOT / "ndn-service-framework" / "ServiceUser.cpp"
        ).read_text(errors="replace")
        helper_start = service_user.index("makeRequestScopedDiscoveryPayload")
        helper_end = service_user.index(
            "bool ServiceUser::prepareRequestScopedRequest", helper_start,
        )
        projection = service_user[helper_start:helper_end]
        self.assertIn("ndnsf-di-request-envelope-v2", projection)
        self.assertIn(r'\"input_payload_b64\":\"', projection)
        for application_schema in (
                "ndnsf-di-provider-offer-v2",
                "ndnsf-di-selection-assignment-v2",
                "ndnsf-di-selection-acceptance-v2",
                "EXACT_PREFIX_KV_V1"):
            self.assertNotIn(application_schema, sources)

    def test_non_di_core_fixture_is_retained(self):
        fixture = (
            ROOT / "tests" / "unit-tests" /
            "opaque-selection-lifecycle.t.cpp").read_text()
        self.assertIn("GenericOpaqueSelection", fixture)
        self.assertIn("ServiceProviderSeamCommitsBeforeProjectionAndReplays",
                      fixture)
        self.assertNotIn("Qwen", fixture)


if __name__ == "__main__":
    unittest.main()
