from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceTopologyProfile, ExecutionDisposition, PlanSealerV3,
    PlacementProposalV3, ProviderOfferV3, ProviderPlanningViewV3,
    RoleAssemblySpec, ResidencyProofV3, ResidencyTierV3,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64
S = "sha256:" + "5" * 64


def view(provider: str, *, preparation: bool = False) -> ProviderPlanningViewV3:
    topology = DeviceTopologyProfile(provider, (), "cpu")
    residency = () if preparation else (ResidencyProofV3(
        artifact_digest=A, role="stage0", rank=0, tier=ResidencyTierV3.DISK,
        boot_epoch="boot-0001", process_epoch="process-0001",
        topology_digest=topology.digest()),)
    offer = ProviderOfferV3(
        request_id="req-1", attempt=1, service="/LLM/Qwen", provider=provider,
        model_digest=M, graph_digest=G, status=True,
        execution_disposition=(ExecutionDisposition.ACCEPT_WITH_PREPARATION
                                if preparation
                                else ExecutionDisposition.ACCEPT_IF_EXACT_REUSE),
        preparation_accepted=preparation, topology=topology, residency=residency,
        accepted_roles=("stage0",), backends=("cpu",), boot_epoch="boot-0001",
        captured_at_ms=1, expires_at_ms=100, signer_key_id="signed-key",
        signature="signed")
    return ProviderPlanningViewV3.from_offer(offer)


def proposal(provider: str = "p0") -> PlacementProposalV3:
    role = RoleAssemblySpec(
        role="stage0", rank=0, layer_begin=0, layer_end=2,
        recipe_digest=R, artifact_digest=A, backend="cpu")
    return PlacementProposalV3(
        request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
        roles=(role,), provider_by_role={"stage0": provider},
        dependencies=(), strategy_name="layer-reuse-first", strategy_version="3",
        strategy_state_digest=S)


class Spec170PlanSealerTest(unittest.TestCase):
    def test_exact_reuse_requires_exact_residency(self):
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
            {"p0": view("p0")})
        self.assertTrue(core.plan_core_digest.startswith("sha256:"))
        with self.assertRaises(ValueError):
            missing = view("p0")
            missing = ProviderPlanningViewV3(
                provider=missing.provider, offer_digest=missing.offer_digest,
                request_id=missing.request_id, attempt=missing.attempt,
                topology=missing.topology, resources=missing.resources,
                residency=(), accepted_roles=missing.accepted_roles,
                backends=missing.backends,
                execution_disposition=missing.execution_disposition,
                preparation_accepted=missing.preparation_accepted,
                queue_depth=missing.queue_depth,
                estimated_wait_ms=missing.estimated_wait_ms,
                rtt_ms=missing.rtt_ms, bandwidth_mbps=missing.bandwidth_mbps)
            PlanSealerV3.seal_core(
                {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
                {"p0": missing})

    def test_preparation_offer_is_required_for_cold_role(self):
        prepared = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
            {"p0": view("p0", preparation=True)})
        self.assertEqual(prepared.provider_by_role["stage0"], "p0")

    def test_two_stage_digest_has_no_grant_cycle(self):
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
            {"p0": view("p0")})
        grant = PlanSealerV3.grant_view(core, "p0", view("p0"), S)
        final = PlanSealerV3.finalize_security(core, (grant,), S)
        self.assertNotEqual(final, core.plan_core_digest)
        self.assertEqual(
            final,
            PlanSealerV3.finalize_security(core, (grant,), S))

    def test_incomplete_or_substituted_grants_fail(self):
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
            {"p0": view("p0")})
        with self.assertRaises(ValueError):
            PlanSealerV3.finalize_security(core, (), S)
        with self.assertRaises(ValueError):
            PlanSealerV3.finalize_security(
                core, (PlanSealerV3.grant_view(core, "p0", view("p0"), R),), S)

    def test_multi_rank_role_keys_are_explicit_and_sealed(self):
        topology = DeviceTopologyProfile("p0", ("cuda:0", "cuda:1"), "cuda")
        proofs = tuple(
            ResidencyProofV3(
                artifact_digest=A, role="tensor", rank=rank,
                tier=ResidencyTierV3.GPU,
                device_set=(f"cuda:{rank}",), boot_epoch="boot-0001",
                process_epoch="process-0001", topology_digest=topology.digest(),
            ) for rank in (0, 1)
        )
        offer = ProviderOfferV3(
            request_id="req-1", attempt=1, service="/LLM/Qwen", provider="p0",
            model_digest=M, graph_digest=G, status=True,
            execution_disposition=ExecutionDisposition.ACCEPT_IF_EXACT_REUSE,
            preparation_accepted=False, topology=topology, residency=proofs,
            accepted_roles=("tensor",), backends=("cuda",),
            boot_epoch="boot-0001", captured_at_ms=1, expires_at_ms=100,
            signer_key_id="signed-key", signature="signed",
        )
        view_v3 = ProviderPlanningViewV3.from_offer(offer)
        roles = tuple(
            RoleAssemblySpec(
                role="tensor", rank=rank, layer_begin=rank,
                layer_end=rank + 1, recipe_digest=R, artifact_digest=A,
                backend="cuda", device_set=(f"cuda:{rank}",),
            ) for rank in (0, 1)
        )
        proposal_v3 = PlacementProposalV3(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            roles=roles, provider_by_role={"tensor#0": "p0", "tensor#1": "p0"},
            strategy_name="layer-reuse-first", strategy_version="3",
            strategy_state_digest=S,
        )
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "attempt": 1, "ack_closed_digest": S},
            proposal_v3, {"p0": view_v3})
        grant = PlanSealerV3.grant_view(core, "p0", view_v3, S)
        self.assertEqual(len(grant.role_digests), 2)


if __name__ == "__main__":
    unittest.main()
