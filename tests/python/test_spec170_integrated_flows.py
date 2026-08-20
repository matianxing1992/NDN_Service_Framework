"""Cross-module Spec 170 flows that run before the MiniNDN gate.

These cases deliberately compose contracts from more than one module.  They
are not a replacement for the C++ DummyClientFace/SVSPubSub target; together
the two targets cover control-plane composition, artifact state, DATA_V1
integrity, protected lifecycle, and device admission without starting NFD.
"""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.app_sdk.canonical_artifacts import (  # noqa: E402
    AssembledOnnxArtifactV1,
    CanonicalLayerCatalog,
)
from ndnsf_distributed_inference.artifact_deployment import (  # noqa: E402
    assemble_onnx_role,
)
from ndnsf_distributed_inference.core import (  # noqa: E402
    DataSegmentReplayWindow,
    DataSegmentV1,
    GrantRequestV1,
    HybridPlan,
    LocalTensorGroup,
    PlaintextLeaseRegistry,
    RedistributionEdge,
    RevocationStateV1,
    TensorDisposition,
    TensorSlice,
    V3AdmissionController,
    V3LifecycleState,
)
from ndnsf_distributed_inference.provider import (  # noqa: E402
    _assignment_deadline_ms,
)
from ndnsf_distributed_inference.core.device_scheduler import (  # noqa: E402
    DeviceJobV3,
    MultiDeviceSchedulerV3,
)
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceBinding,
    DeviceBindingMode,
    DeviceTopologyProfile,
    ExecutionRole,
    ExecutionDisposition,
    PlacementProposalV3,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    ProviderSelectionProjectionV3,
    PlanSealerV3,
    ResidencyClassV3,
    ResidencyProofV3,
    ResidencyTierV3,
    RoleAssemblySpec,
    RoleDataflowContract,
    canonical_digest,
)
from ndnsf_distributed_inference.security import ArtifactPolicyAuthority  # noqa: E402


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64
S = "sha256:" + "5" * 64
MI = "sha256:" + "6" * 64
PF = "sha256:" + "7" * 64
RI = "sha256:" + "8" * 64


def _v3_flow_objects():
    topology = DeviceTopologyProfile("p0", ("cuda:0",), "cuda")
    role = RoleAssemblySpec(
        "stage0", 0, 0, 2, R, A, "cuda", ("cuda:0",))
    offer = ProviderOfferV3(
        request_id="req-1", attempt=1, service="/LLM/Qwen", provider="p0",
        model_digest=M, graph_digest=G, status=True,
        execution_disposition=ExecutionDisposition.ACCEPT_IF_EXACT_REUSE,
        preparation_accepted=False, topology=topology,
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
    view = ProviderPlanningViewV3.from_offer(offer)
    proposal = PlacementProposalV3(
        request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
        roles=(role,), provider_by_role={"stage0": "p0"}, dependencies=(),
        strategy_name="layer-reuse-first", strategy_version="3",
        strategy_state_digest=S)
    core = PlanSealerV3.seal_core(
        {"request_id": "req-1", "ack_closed_digest": S}, proposal, {"p0": view})
    plan_digest = PlanSealerV3.finalize_security(core, (), S)
    projection = ProviderSelectionProjectionV3(
        provider="p0", request_id="req-1", attempt=1,
        plan_core_digest=core.plan_core_digest, plan_digest=plan_digest,
        ack_closed_digest=S, offer_digest=view.offer_digest,
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
            view.offer_digest, topology.digest(), canonical_digest(view.resources),
            1, offer_scoped_device_handle="cuda:0"),
    )
    return offer, core, projection


class Spec170IntegratedFlowsTest(unittest.TestCase):
    def test_v3_projection_rejects_execution_assembly_substitution(self):
        _, _, projection = _v3_flow_objects()
        with self.assertRaisesRegex(ValueError, "role projection binding"):
            replace(
                projection,
                execution_role=replace(
                    projection.execution_role,
                    layer_end=projection.execution_role.layer_end + 1,
                ),
            )

    def test_v3_projection_preserves_execution_deadline(self):
        _, _, projection = _v3_flow_objects()
        self.assertEqual(_assignment_deadline_ms(projection.to_bytes()), 100)

    def test_v3_ack_closed_selection_jit_admission_and_response(self):
        offer, core, projection = _v3_flow_objects()
        controller = V3AdmissionController(
            "p0", boot_epoch="boot-0001", visible_devices=("cuda:0",))

        controller.observe_ack(offer)
        self.assertEqual(controller.queue_records, ())
        self.assertEqual(controller.held_devices, ())
        record = controller.accept_selection(core, projection, offer)
        self.assertEqual(record.state, V3LifecycleState.QUEUE_ACCEPTED)
        self.assertEqual(controller.held_devices, ())
        controller.mark_host_ready("req-1", 1)
        token = controller.admit_devices(
            "req-1", 1, ("cuda:0",), resource_sequence=1)
        self.assertEqual(controller.held_devices, ("cuda:0",))
        controller.complete("req-1", 1, token)
        self.assertEqual(controller.held_devices, ())
        with self.assertRaisesRegex(ValueError, "not device-admitted"):
            controller.complete("req-1", 1, token)

    def test_canonical_root_assembly_and_exact_reuse(self):
        payload = b"layer-0"
        model = "Qwen/Qwen3-0.6B"
        manifest = __import__(
            "ndnsf_distributed_inference.adapters.qwen.canonical_layers",
            fromlist=["canonical_layer_manifest"],
        ).canonical_layer_manifest(
            model_name=model, model_digest=M, profile="fp16", graph_digest=G,
            role_kind="pipeline", layer_begin=0, layer_end=2, rank=0,
            recipe_digest=R, payload=payload, publisher="/provider/p0")
        catalog = CanonicalLayerCatalog()
        name = catalog.publish_layer(manifest, payload)
        self.assertEqual(catalog.publish_layer(manifest, payload), name)
        root = catalog.publish_root()
        self.assertTrue(root.startswith("sha256:"))

        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "stage.ndnsf-onnx-artifact"
            result = assemble_onnx_role(
                role="stage0", model_name=model, model_digest=M,
                profile="qwen-onnx-cpu", graph_digest=G,
                layer_payloads={"/layer/0": payload},
                layer_digests={"/layer/0": "sha256:" + hashlib.sha256(payload).hexdigest()},
                recipe_digest=R, provider="/provider/p0", signature="signed",
                output_path=target)
            self.assertEqual(result.object_digest, result.object_digest)
            restored = AssembledOnnxArtifactV1.from_bytes(target.read_bytes())
            restored.verify_provider("/provider/p0")
            self.assertEqual(restored.object_digest, result.object_digest)
            with self.assertRaises(ValueError):
                restored.verify_provider("/provider/p1")

    def test_cross_provider_data_v1_rank_epoch_replay_and_failure(self):
        plan = HybridPlan(
            stages=3, tensor_degrees=(1, 2, 1),
            rank_labels=("S0R0", "S1R0", "S1R1", "S2R0"),
            redistributions=(
                RedistributionEdge(
                    producer_ranks=(0,), consumer_ranks=(1, 2), tensor="h-in",
                    operation="SCATTER", epoch="e1",
                    integrity_digest="sha256:" + "a" * 64,
                    source_layout_digest="sha256:" + "c" * 64,
                    target_layout_digest="sha256:" + "d" * 64,
                    temporary_memory_bytes=4096),
                RedistributionEdge(
                    producer_ranks=(1, 2), consumer_ranks=(3,), tensor="h-out",
                    operation="GATHER", epoch="e1",
                    integrity_digest="sha256:" + "b" * 64,
                    source_layout_digest="sha256:" + "d" * 64,
                    target_layout_digest="sha256:" + "e" * 64,
                    temporary_memory_bytes=4096),
            ))
        self.assertEqual(plan.rank_count, 4)
        group = LocalTensorGroup(
            "S1", "e1", ("p0", "p1"),
            (TensorSlice("w", 0, TensorDisposition.SHARDED, 0, 0, 4, "row"),
             TensorSlice("w", 1, TensorDisposition.SHARDED, 0, 4, 8, "row")))
        self.assertEqual(len(group.tensors), 2)

        key = b"k" * 32
        segment = DataSegmentV1.create(
            operation_id="op-1", epoch="e1", producer="p0", consumer="p1",
            segment_no=0, payload=b"tensor-bytes", key=key, aad=b"tensor=w")
        window = DataSegmentReplayWindow(operation_id="op-1", epoch="e1")
        window.accept(segment, key=key)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            window.accept(segment, key=key)
        with self.assertRaises(ValueError):
            segment.verify(b"x" * 32)

    def test_heterogeneous_profiles_cover_both_frozen_d2h_mappings(self):
        profiles = (
            ((1, 2, 1), ("S0R0", "S1R0", "S1R1", "S2R0"),
             ((0,), (1, 2), "SCATTER"), ((1, 2), (3,), "GATHER"),
             {"P0/G0": ("S0R0", "S1R0"),
              "P1/G1": ("S1R1", "S2R0")}),
            ((2, 1, 2), ("S0R0", "S0R1", "S1R0", "S2R0", "S2R1"),
             ((0, 1), (2,), "GATHER"), ((2,), (3, 4), "SCATTER"),
             {"P0/G0": ("S0R0", "S1R0", "S2R0"),
              "P1/G1": ("S0R1", "S2R1")}),
        )
        for degrees, labels, first_edge, second_edge, mapping in profiles:
            mapped = tuple(role for roles in mapping.values() for role in roles)
            self.assertEqual(set(mapped), set(labels))
            self.assertEqual(len(mapped), len(set(mapped)))
            edges = (
                RedistributionEdge(
                    producer_ranks=first_edge[0],
                    consumer_ranks=first_edge[1],
                    tensor="activation-0",
                    operation=first_edge[2],
                    epoch="epoch-1",
                    integrity_digest="sha256:" + "a" * 64,
                    source_layout_digest="sha256:" + "c" * 64,
                    target_layout_digest="sha256:" + "d" * 64,
                    temporary_memory_bytes=4096),
                RedistributionEdge(
                    producer_ranks=second_edge[0],
                    consumer_ranks=second_edge[1],
                    tensor="activation-1",
                    operation=second_edge[2],
                    epoch="epoch-1",
                    integrity_digest="sha256:" + "b" * 64,
                    source_layout_digest="sha256:" + "d" * 64,
                    target_layout_digest="sha256:" + "e" * 64,
                    temporary_memory_bytes=4096),
            )
            plan = HybridPlan(
                stages=3,
                tensor_degrees=degrees,
                rank_labels=labels,
                redistributions=edges)
            self.assertEqual(plan.rank_count, sum(degrees))
            self.assertEqual(len(plan.rank_labels), plan.rank_count)
            self.assertEqual(len(plan.redistributions), 2)
            self.assertEqual(
                set(plan.redistributions[0].producer_ranks) |
                set(plan.redistributions[0].consumer_ranks),
                set(first_edge[0]) | set(first_edge[1]))

    def test_protected_grant_revocation_lease_and_zeroization(self):
        request = GrantRequestV1(
            provider="/provider/p0", request_id="req-1", attempt=1,
            plan_core_digest=M, grant_view_digest=G, artifact_digest=A,
            recipient="/provider/p0")
        authority = ArtifactPolicyAuthority("/authority", b"k" * 32)
        grant = authority.issue(request, wrapped_key=b"wrapped", expires_at_ms=500)
        grant.verify(authority="/authority", key=b"k" * 32, now_ms=100)
        state = RevocationStateV1(
            "/authority", "epoch-1", 1,
            revoked_grants=(grant.request_digest,), next_check_at_ms=500)
        self.assertTrue(state.is_revoked(grant, now_ms=100))

        registry = PlaintextLeaseRegistry()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "plaintext.onnx"
            registry.register("lease-1", path, b"secret-model")
            registry.zeroize("lease-1")
            self.assertFalse(path.exists())

    def test_d2a_two_device_roles_and_unsplittable_rejection(self):
        scheduler = MultiDeviceSchedulerV3({"cuda:0": 12_000, "cuda:1": 12_000})
        scheduler.submit(DeviceJobV3("req-0", "stage-0", 8_000))
        scheduler.submit(DeviceJobV3("req-1", "stage-1", 8_000))
        self.assertEqual(scheduler.used_memory_mb, {"cuda:0": 0, "cuda:1": 0})
        first = scheduler.admit("req-0")
        second = scheduler.admit("req-1")
        self.assertEqual((first.device, second.device), ("cuda:0", "cuda:1"))
        scheduler.complete("req-0")
        scheduler.complete("req-1")
        group = LocalTensorGroup(
            "stage1", "epoch-1", ("cuda:0", "cuda:1"),
            (TensorSlice("hidden", 0, TensorDisposition.SHARDED,
                         0, 0, 4, "row"),
             TensorSlice("hidden", 1, TensorDisposition.SHARDED,
                         0, 4, 8, "row")))
        self.assertEqual(group.participants, ("cuda:0", "cuda:1"))
        self.assertEqual({item.rank for item in group.tensors}, {0, 1})
        with self.assertRaisesRegex(ValueError, "single visible device"):
            scheduler.submit(DeviceJobV3("req-large", "stage", 20_000))


if __name__ == "__main__":
    unittest.main()
