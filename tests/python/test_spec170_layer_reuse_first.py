from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.planner.layer_reuse_first import LayerReuseFirstStrategy  # noqa: E402
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceResourceSnapshot, DeviceTopologyProfile, ExecutionDisposition,
    ProviderOfferV3,
    ProviderPlanningViewV3, ResidencyClassV3, ResidencyProofV3,
    ResidencyTierV3, RoleAssemblySpec,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64
MI = "sha256:" + "6" * 64
PF = "sha256:" + "7" * 64
RI = "sha256:" + "8" * 64


def provider(name: str, *, exact: bool, queue: int) -> ProviderPlanningViewV3:
    topology = DeviceTopologyProfile(name, (), "cpu")
    proof = (ResidencyProofV3(
        A, "stage0", 0, ResidencyTierV3.DISK, (), "boot-0001",
        "process-0001", topology.digest(),
        residency_class=ResidencyClassV3.ASSEMBLED_FRAGMENT,
        identity_digest=RI, assembly_spec_digest=R,
        model_manifest_digest=MI, artifact_profile_digest=PF,
        graph_digest=G, backend="cpu",
        protection_epoch="plaintext-v1"),) if exact else ()
    offer = ProviderOfferV3(
        "req-1", 1, "/LLM/Qwen", name, M, G, True,
        ExecutionDisposition.ACCEPT_IF_EXACT_REUSE if exact
        else ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        not exact, topology, residency=proof, accepted_roles=("stage0",),
        backends=("cpu",), queue_depth=queue, boot_epoch="boot-0001",
        captured_at_ms=1, expires_at_ms=100, signer_key_id="signed-key",
        signature="signed")
    return ProviderPlanningViewV3.from_offer(offer)


class Spec170LayerReuseFirstTest(unittest.TestCase):
    def test_one_role_cannot_pool_memory_from_two_devices(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        topology = DeviceTopologyProfile(
            "gpu-provider", ("cuda:0", "cuda:1"), "cuda")
        resources = tuple(
            DeviceResourceSnapshot(
                device, 12_000, 10_000,
                topology_digest=topology.digest())
            for device in topology.devices)
        offer = ProviderOfferV3(
            "req-1", 1, "/LLM/Qwen", "gpu-provider", M, G, True,
            ExecutionDisposition.ACCEPT_WITH_PREPARATION, True, topology,
            resources=resources, accepted_roles=("stage0",),
            backends=("cuda",), boot_epoch="boot-0001",
            captured_at_ms=1, expires_at_ms=100,
            signer_key_id="signed-key", signature="signed")
        role = RoleAssemblySpec(
            "stage0", 0, 0, 2, R, A, "cuda",
            required_device_memory_mb=16_000)
        with self.assertRaisesRegex(ValueError, "no distinct feasible Provider"):
            strategy.propose_v3(
                request_id="req-1", attempt=1, model_digest=M,
                graph_digest=G, roles=(role,),
                providers=(ProviderPlanningViewV3.from_offer(offer),),
                ack_closed_digest="sha256:" + "5" * 64)

    def test_exact_residency_wins_over_cold_provider(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        role = RoleAssemblySpec("stage0", 0, 0, 2, R, A, "cpu")
        decision = strategy.propose_v3(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            roles=(role,), providers=(provider("cold", exact=False, queue=0),
                                      provider("warm", exact=True, queue=5)),
            ack_closed_digest="sha256:" + "5" * 64)
        self.assertEqual(decision.provider_by_role["stage0"], "warm")

    def test_loaded_then_assembled_then_canonical_order_is_subordinate(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        role = RoleAssemblySpec("stage0", 0, 0, 2, R, A, "cpu")

        def view(name: str, kind: str):
            topology = DeviceTopologyProfile(name, (), "cpu")
            common = dict(
                artifact_digest=A, role="stage0", rank=0,
                device_set=(), boot_epoch="boot-0001",
                process_epoch="process-0001",
                topology_digest=topology.digest(), identity_digest=RI,
                model_manifest_digest=MI, artifact_profile_digest=PF,
                graph_digest=G, backend="cpu",
                protection_epoch="plaintext-v1")
            if kind == "loaded":
                proof = ResidencyProofV3(
                    tier=ResidencyTierV3.RAM,
                    residency_class=ResidencyClassV3.LOADED_RUNTIME,
                    assembly_spec_digest=R, runtime_generation=1,
                    fencing_token="fence-1", **common)
                disposition = ExecutionDisposition.ACCEPT_IF_EXACT_REUSE
                preparation = False
            elif kind == "assembled":
                proof = ResidencyProofV3(
                    tier=ResidencyTierV3.DISK,
                    residency_class=ResidencyClassV3.ASSEMBLED_FRAGMENT,
                    assembly_spec_digest=R, estimated_load_ms=5, **common)
                disposition = ExecutionDisposition.ACCEPT_IF_EXACT_REUSE
                preparation = False
            elif kind == "canonical":
                proof = ResidencyProofV3(
                    tier=ResidencyTierV3.CANONICAL,
                    residency_class=ResidencyClassV3.CANONICAL,
                    missing_verified_bytes=10, estimated_assembly_ms=10,
                    estimated_load_ms=5, **common)
                disposition = ExecutionDisposition.ACCEPT_WITH_PREPARATION
                preparation = True
            else:
                proof = None
                disposition = ExecutionDisposition.ACCEPT_WITH_PREPARATION
                preparation = True
            offer = ProviderOfferV3(
                "req-1", 1, "/LLM/Qwen", name, M, G, True,
                disposition, preparation, topology,
                residency=(() if proof is None else (proof,)),
                accepted_roles=("stage0",), backends=("cpu",),
                queue_depth=99 if kind == "loaded" else 0,
                boot_epoch="boot-0001", captured_at_ms=1,
                expires_at_ms=100, signer_key_id="signed-key",
                signature="signed")
            return ProviderPlanningViewV3.from_offer(offer)

        providers = tuple(view(name, kind) for name, kind in (
            ("loaded", "loaded"), ("assembled", "assembled"),
            ("canonical", "canonical"), ("cold", "cold")))
        for remaining, expected in (
            (providers, "loaded"), (providers[1:], "assembled"),
            (providers[2:], "canonical"), (providers[3:], "cold")):
            with self.subTest(expected=expected):
                decision = strategy.propose_v3(
                    request_id="req-1", attempt=1, model_digest=M,
                    graph_digest=G, roles=(role,), providers=remaining,
                    ack_closed_digest="sha256:" + "5" * 64)
                self.assertEqual(decision.provider_by_role["stage0"], expected)

    def test_exact_loaded_device_is_chosen_after_device_feasibility(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        topology = DeviceTopologyProfile(
            "gpu-provider", ("cuda:0", "cuda:1"), "cuda")
        resources = tuple(DeviceResourceSnapshot(
            device, 16_000, 12_000, topology_digest=topology.digest())
            for device in topology.devices)
        proof = ResidencyProofV3(
            A, "stage0", 0, ResidencyTierV3.GPU, ("cuda:1",),
            "boot-0001", "process-0001", topology.digest(),
            residency_class=ResidencyClassV3.LOADED_RUNTIME,
            identity_digest=RI, assembly_spec_digest=R,
            model_manifest_digest=MI, artifact_profile_digest=PF,
            graph_digest=G, backend="onnxruntime-cuda",
            protection_epoch="plaintext-v1", runtime_generation=1,
            fencing_token="fence-1")
        offer = ProviderOfferV3(
            "req-1", 1, "/LLM/Qwen", "gpu-provider", M, G, True,
            ExecutionDisposition.ACCEPT_IF_EXACT_REUSE, False, topology,
            resources=resources, residency=(proof,),
            accepted_roles=("stage0",), backends=("onnxruntime-cuda",),
            boot_epoch="boot-0001", captured_at_ms=1, expires_at_ms=100,
            signer_key_id="signed-key", signature="signed")
        decision = strategy.propose_v3(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            roles=(RoleAssemblySpec(
                "stage0", 0, 0, 2, R, A, "onnxruntime",
                required_device_memory_mb=1000),),
            providers=(ProviderPlanningViewV3.from_offer(offer),),
            ack_closed_digest="sha256:" + "5" * 64)
        self.assertEqual(decision.roles[0].device_set, ("cuda:1",))
        self.assertEqual(decision.roles[0].backend, "onnxruntime-cuda")

    def test_heterogeneous_roles_use_exact_rank_keys(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        roles = (
            RoleAssemblySpec("stage0", 0, 0, 1, R, A, "cpu"),
            RoleAssemblySpec("stage1", 0, 1, 2, R, A, "cpu",
                             role_kind="HYBRID_RANK"),
            RoleAssemblySpec("stage1", 1, 1, 2, R, A, "cpu",
                             role_kind="HYBRID_RANK"),
            RoleAssemblySpec("stage2", 0, 2, 3, R, A, "cpu"),
        )
        views = tuple(
            replace(
                provider(f"cold-{index}", exact=False, queue=index),
                accepted_roles=("stage0", "stage1", "stage2"),
            )
            for index in range(4)
        )
        decision = strategy.propose_v3(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            roles=roles, providers=views,
            ack_closed_digest="sha256:" + "5" * 64)
        self.assertEqual(set(decision.provider_by_role), {
            "stage0", "stage1#0", "stage1#1", "stage2"})
        self.assertEqual(len(set(decision.provider_by_role.values())), 4)

    def test_missing_hybrid_rank_fails_before_provider_selection(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        roles = (
            RoleAssemblySpec("stage1", 0, 1, 2, R, A, "cpu",
                             role_kind="HYBRID_RANK"),
            RoleAssemblySpec("stage1", 2, 1, 2, R, A, "cpu",
                             role_kind="HYBRID_RANK"),
        )
        with self.assertRaisesRegex(ValueError, "rank cover"):
            strategy.propose_v3(
                request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
                roles=roles, providers=(provider("cold", exact=False, queue=0),),
                ack_closed_digest="sha256:" + "5" * 64)


if __name__ == "__main__":
    unittest.main()
