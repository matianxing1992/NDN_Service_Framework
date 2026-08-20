from __future__ import annotations

from dataclasses import replace
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
    DeviceBinding, DeviceBindingMode, DeviceResourceSnapshot,
    DeviceTopologyProfile, ExecutionRole,
    ExecutionDisposition, PlacementPlanCoreV3,
    ProviderOfferV3, ProviderSelectionProjectionV3, RoleAssemblySpec,
    RoleDataflowContract, ResidencyClassV3, ResidencyProofV3,
    ResidencyTierV3, canonical_digest,
    UNBOUND_GRAPH_DIGEST_V3,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64
S = "sha256:" + "5" * 64
MI = "sha256:" + "6" * 64
PF = "sha256:" + "7" * 64
RI = "sha256:" + "8" * 64


class Spec170AckNoReservationTest(unittest.TestCase):
    def setUp(self):
        topology = DeviceTopologyProfile("p0", ("cuda:0",), "cuda")
        role = RoleAssemblySpec("stage0", 0, 0, 2, R, A, "cuda", ("cuda:0",))
        self.role = role
        self.offer = ProviderOfferV3(
            "req-1", 1, "/LLM/Qwen", "p0", M, G, True,
            ExecutionDisposition.ACCEPT_IF_EXACT_REUSE, False, topology,
            residency=(ResidencyProofV3(
                A, "stage0", 0, ResidencyTierV3.GPU, ("cuda:0",),
                "boot-0001", "process-0001", topology.digest(),
                residency_class=ResidencyClassV3.LOADED_RUNTIME,
                identity_digest=RI, assembly_spec_digest=R,
                model_manifest_digest=MI, artifact_profile_digest=PF,
                graph_digest=G, backend="cuda",
                protection_epoch="plaintext-v1", runtime_generation=1,
                fencing_token="fence-1"),),
            accepted_roles=("stage0",), backends=("cuda",),
            boot_epoch="boot-0001", captured_at_ms=1, expires_at_ms=100,
            signer_key_id="key", signature="signed")
        self.plan = PlacementPlanCoreV3(
            "req-1", 1, M, G, (role,), {"stage0": "p0"}, (), S, S,
            "sha256:" + "6" * 64)
        plan_digest = "sha256:" + "7" * 64
        self.projection = ProviderSelectionProjectionV3(
            provider="p0", request_id="req-1", attempt=1,
            plan_core_digest=self.plan.plan_core_digest,
            plan_digest=plan_digest, ack_closed_digest=S,
            offer_digest=self.offer.digest(),
            security_policy_snapshot_digest=S,
            roles=(role,), dependencies=(), deadline_ms=100,
            execution_role=ExecutionRole(
                "stage0", "stage0", 0, 0, 2, "cuda"),
            assembly=role,
            dataflow=RoleDataflowContract(
                "req-1", 1, plan_digest, "stage0",
                terminal_response_owner=True),
            device_binding=DeviceBinding(
                DeviceBindingMode.SINGLE_DEVICE, "p0", "stage0",
                self.offer.digest(), topology.digest(),
                canonical_digest(self.offer.resources), 1,
                offer_scoped_device_handle="cuda:0"),
        )
        self.controller = V3AdmissionController(
            "p0", boot_epoch="boot-0001", visible_devices=("cuda:0",))

    def test_ack_is_side_effect_free_and_selection_has_no_gpu_hold(self):
        self.controller.observe_ack(self.offer)
        self.assertEqual(self.controller.queue_records, ())
        self.assertEqual(self.controller.held_devices, ())

    def test_selection_rejects_substituted_offer(self):
        with self.assertRaisesRegex(ValueError, "offer binding"):
            self.controller.accept_selection(
                self.plan, self.projection,
                replace(self.offer, queue_depth=self.offer.queue_depth + 1))

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
        with self.assertRaisesRegex(ValueError, "snapshot is stale"):
            self.controller.admit_devices(
                "req-1", 1, ("cuda:0",), resource_sequence=2)
        self.assertEqual(self.controller.held_devices, ())
        token = self.controller.admit_devices("req-1", 1, ("cuda:0",), resource_sequence=1)
        self.assertEqual(self.controller.held_devices, ("cuda:0",))
        self.controller.complete("req-1", 1, token)
        self.assertEqual(self.controller.held_devices, ())
        with self.assertRaises(ValueError):
            self.controller.complete("req-1", 1, token)

    def test_cpu_binding_admits_without_accelerator_fallback(self):
        topology = DeviceTopologyProfile("cpu-provider", (), "cpu")
        role = RoleAssemblySpec("cpu-stage", 0, 0, 1, R, A, "onnxruntime-cpu")
        offer = ProviderOfferV3(
            "cpu-req", 1, "/LLM/Qwen", "cpu-provider", M, G, True,
            ExecutionDisposition.ACCEPT_WITH_PREPARATION, True, topology,
            accepted_roles=("cpu-stage",), backends=("onnxruntime-cpu",),
            boot_epoch="boot-0001", captured_at_ms=1, expires_at_ms=100,
            signer_key_id="key", signature="signed")
        base = PlacementPlanCoreV3(
            "cpu-req", 1, M, G, (role,), {"cpu-stage": "cpu-provider"},
            (), S, S)
        plan = replace(base, plan_core_digest=base.digest())
        plan_digest = canonical_digest({"request": "cpu-req"})
        projection = ProviderSelectionProjectionV3(
            provider="cpu-provider", request_id="cpu-req", attempt=1,
            plan_core_digest=plan.plan_core_digest, plan_digest=plan_digest,
            ack_closed_digest=S, offer_digest=offer.digest(),
            security_policy_snapshot_digest=S,
            roles=(role,), dependencies=(), deadline_ms=100,
            execution_role=ExecutionRole(
                "cpu-stage", "cpu-stage", 0, 0, 1, "onnxruntime-cpu"),
            assembly=role,
            dataflow=RoleDataflowContract(
                "cpu-req", 1, plan_digest, "cpu-stage",
                terminal_response_owner=True),
            device_binding=DeviceBinding(
                DeviceBindingMode.CPU, "cpu-provider", "cpu-stage",
                offer.digest(), topology.digest(), canonical_digest(()), 1),
        )
        controller = V3AdmissionController(
            "cpu-provider", boot_epoch="boot-0001", visible_devices=())
        controller.accept_selection(plan, projection, offer)
        controller.mark_host_ready("cpu-req", 1)
        token = controller.admit_devices(
            "cpu-req", 1, (), resource_sequence=1)
        self.assertEqual(controller.held_devices, ())
        controller.complete("cpu-req", 1, token)

    def test_two_requests_may_use_two_devices_independently(self):
        controller = V3AdmissionController(
            "p0", boot_epoch="boot-0001",
            visible_devices=("cuda:0", "cuda:1"))
        cases = []
        for index, device in enumerate(("cuda:0", "cuda:1"), start=1):
            request_id = f"req-{index}"
            topology = DeviceTopologyProfile(
                "p0", ("cuda:0", "cuda:1"), "cuda")
            resources = tuple(DeviceResourceSnapshot(
                item, 1024, 1024, resource_sequence=index,
                topology_digest=topology.digest()) for item in topology.devices)
            role = RoleAssemblySpec(
                "stage0", 0, 0, 1, R, A, "cuda", (device,), 512)
            offer = ProviderOfferV3(
                request_id, 1, "/LLM/Qwen", "p0", M, G, True,
                ExecutionDisposition.ACCEPT_WITH_PREPARATION, True, topology,
                resources=resources, accepted_roles=("stage0",),
                backends=("cuda",), boot_epoch="boot-0001",
                captured_at_ms=1, expires_at_ms=100,
                signer_key_id="key", signature="signed")
            base = PlacementPlanCoreV3(
                request_id, 1, M, G, (role,), {"stage0": "p0"}, (), S, S)
            plan = replace(base, plan_core_digest=base.digest())
            plan_digest = canonical_digest({"request": request_id})
            projection = ProviderSelectionProjectionV3(
                provider="p0", request_id=request_id, attempt=1,
                plan_core_digest=plan.plan_core_digest, plan_digest=plan_digest,
                ack_closed_digest=S, offer_digest=offer.digest(),
                security_policy_snapshot_digest=S,
                roles=(role,), dependencies=(), deadline_ms=100,
                execution_role=ExecutionRole(
                    "stage0", "stage0", 0, 0, 1, "cuda"),
                assembly=role,
                dataflow=RoleDataflowContract(
                    request_id, 1, plan_digest, "stage0",
                    terminal_response_owner=True),
                device_binding=DeviceBinding(
                    DeviceBindingMode.SINGLE_DEVICE, "p0", "stage0",
                    offer.digest(), topology.digest(),
                    canonical_digest(resources), index,
                    offer_scoped_device_handle=device),
            )
            cases.append((plan, projection, offer, device, index))

        for plan, projection, offer, _, _ in cases:
            controller.accept_selection(plan, projection, offer)
            controller.mark_host_ready(plan.request_id, 1)
        for plan, _, _, device, sequence in cases:
            controller.admit_devices(
                plan.request_id, 1, (device,), resource_sequence=sequence)
        self.assertEqual(controller.held_devices, ("cuda:0", "cuda:1"))

        bounded = V3AdmissionController(
            "p0", boot_epoch="boot-0001",
            visible_devices=("cuda:0", "cuda:1"), max_queue_records=1)
        bounded.accept_selection(*cases[0][:3])
        with self.assertRaisesRegex(ValueError, "queue is full"):
            bounded.accept_selection(*cases[1][:3])

    def test_single_role_rejects_multi_device_admission_atomically(self):
        self.controller.visible_devices = ("cuda:0", "cuda:1")
        self.controller.accept_selection(self.plan, self.projection, self.offer)
        self.controller.mark_host_ready("req-1", 1)
        with self.assertRaisesRegex(ValueError, "exact offer handle"):
            self.controller.admit_devices(
                "req-1", 1, ("cuda:0", "cuda:1"), resource_sequence=1)
        self.assertEqual(self.controller.held_devices, ())

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
