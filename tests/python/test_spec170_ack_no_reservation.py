from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core.v3_lifecycle import (  # noqa: E402
    V3AdmissionController, V3LifecycleState,
)
from ndnsf_distributed_inference.provider import DIProviderOfferIssuerV3  # noqa: E402
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceTopologyProfile, ExecutionDisposition, PlacementPlanCoreV3,
    ProviderOfferV3, ProviderSelectionProjectionV3, RoleAssemblySpec,
    ResidencyProofV3, ResidencyTierV3,
    UNBOUND_GRAPH_DIGEST_V3,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64
S = "sha256:" + "5" * 64


class Spec170AckNoReservationTest(unittest.TestCase):
    def setUp(self):
        topology = DeviceTopologyProfile("p0", ("cuda:0",), "cuda")
        role = RoleAssemblySpec("stage0", 0, 0, 2, R, A, "cuda", ("cuda:0",))
        self.role = role
        self.plan = PlacementPlanCoreV3(
            "req-1", 1, M, G, (role,), {"stage0": "p0"}, (), S, S,
            "sha256:" + "6" * 64)
        self.projection = ProviderSelectionProjectionV3(
            "p0", "req-1", 1, self.plan.plan_core_digest,
            "sha256:" + "7" * 64, (role,), (), 100)
        self.offer = ProviderOfferV3(
            "req-1", 1, "/LLM/Qwen", "p0", M, G, True,
            ExecutionDisposition.ACCEPT_IF_EXACT_REUSE, False, topology,
            residency=(ResidencyProofV3(
                A, "stage0", 0, ResidencyTierV3.GPU, ("cuda:0",),
                "boot-0001", "process-0001", topology.digest()),),
            accepted_roles=("stage0",), backends=("cuda",),
            boot_epoch="boot-0001", captured_at_ms=1, expires_at_ms=100,
            signer_key_id="key", signature="signed")
        self.controller = V3AdmissionController(
            "p0", boot_epoch="boot-0001", visible_devices=("cuda:0",))

    def test_ack_is_side_effect_free_and_selection_has_no_gpu_hold(self):
        self.controller.observe_ack(self.offer)
        self.assertEqual(self.controller.queue_records, ())
        self.assertEqual(self.controller.held_devices, ())

    def test_provider_v3_issuer_has_no_reservation_dependency(self):
        issuer = DIProviderOfferIssuerV3(
            provider="p0", service="/LLM/Qwen", boot_epoch="boot-0001",
            devices=("cuda:0",), signer_key_id="key",
            sign_offer_digest=lambda digest: "signature", clock_ms=lambda: 10)
        decision = issuer.issue(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            deadline_ms=100, accepted_roles=("stage0",), backends=("cuda",),
            execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation_accepted=True)
        self.assertTrue(decision.status)
        self.assertEqual(decision.pending_state_ttl_ms, 90)
        self.assertEqual(issuer.devices, ("cuda:0",))
        offer = ProviderOfferV3.from_bytes(bytes(decision.payload))
        self.assertEqual(offer.graph_digest, G)
        record = self.controller.accept_selection(self.plan, self.projection, self.offer)
        self.assertEqual(record.state, V3LifecycleState.QUEUE_ACCEPTED)
        self.assertEqual(self.controller.held_devices, ())

    def test_complete_device_set_admission_and_stale_token(self):
        self.controller.accept_selection(self.plan, self.projection, self.offer)
        self.controller.mark_host_ready("req-1", 1)
        token = self.controller.admit_devices("req-1", 1, ("cuda:0",), resource_sequence=1)
        self.assertEqual(self.controller.held_devices, ("cuda:0",))
        self.controller.complete("req-1", 1, token)
        self.assertEqual(self.controller.held_devices, ())
        with self.assertRaises(ValueError):
            self.controller.complete("req-1", 1, token)

    def test_v3_issuer_can_advertise_before_graph_inspection(self):
        issuer = DIProviderOfferIssuerV3(
            provider="p0", service="/LLM/Qwen", boot_epoch="boot-0001",
            devices=("cuda:0",), signer_key_id="key",
            sign_offer_digest=lambda digest: "signature", clock_ms=lambda: 10)
        decision = issuer.issue(
            request_id="req-2", attempt=1, model_digest=M,
            deadline_ms=100, accepted_roles=("stage0",), backends=("cuda",),
            execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation_accepted=True)
        offer = ProviderOfferV3.from_bytes(bytes(decision.payload))
        self.assertEqual(offer.graph_digest, UNBOUND_GRAPH_DIGEST_V3)


if __name__ == "__main__":
    unittest.main()
