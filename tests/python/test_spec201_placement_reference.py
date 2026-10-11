"""Placement references come from publication, never invented Producers."""
from types import SimpleNamespace
import unittest

from ndnsf_distributed_inference.app_sdk.placement import (
    AutomaticPlanningCoordinator, ExecutionDisposition)


class PlacementReferenceTest(unittest.TestCase):
    def resolve(self, published=None, *, provision=False, prepared=True, exact=False):
        offer = SimpleNamespace(
            can_provision=provision, preparation_accepted=prepared,
            execution_disposition=(ExecutionDisposition.ACCEPT_IF_EXACT_REUSE if exact
                                   else ExecutionDisposition.ACCEPT_WITH_PREPARATION))
        return AutomaticPlanningCoordinator._v3_artifact_references(published, "stage#1", offer)

    def test_unpublished_local_preparation_and_residency_have_no_data_locator(self):
        self.assertEqual(self.resolve(), ("", ""))
        self.assertEqual(self.resolve(prepared=False, exact=True), ("", ""))

    def test_provisioning_or_unaccepted_local_model_requires_real_publication(self):
        for options in ({"provision": True}, {"prepared": False}):
            with self.assertRaisesRegex(ValueError, "actual published"):
                self.resolve(**options)

    def test_actual_identity_and_transport_are_preserved(self):
        published = SimpleNamespace(
            artifact_data_names_by_role={"stage#1": "/actual/producer/model/object"},
            artifact_fetch_data_names_by_role={"stage#1": "/actual/producer/protected/manifest"})
        self.assertEqual(self.resolve(published, provision=True),
                         ("/actual/producer/model/object", "/actual/producer/protected/manifest"))
        self.assertEqual(self.resolve(published), ("/actual/producer/model/object", ""))

    def test_missing_rank_cannot_use_unranked_publication(self):
        published = SimpleNamespace(artifact_data_names_by_role={"stage": "/actual/model"},
                                    artifact_fetch_data_names_by_role={"stage": "/actual/manifest"})
        with self.assertRaisesRegex(ValueError, "rank artifact"):
            self.resolve(published)


if __name__ == "__main__":
    unittest.main()
