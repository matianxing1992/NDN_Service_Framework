from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceBinding, DeviceBindingMode, DeviceTopologyProfile, ExecutionDisposition,
    ExecutionRole, GrantBindingV1, PlanSealerV3,
    PlacementProposalV3, ProviderOfferV3, ProviderPlanningViewV3,
    ProviderSelectionProjectionV3, RoleAssemblySpec, RoleDataflowContract,
    ResidencyClassV3, ResidencyProofV3, ResidencyTierV3, canonical_digest,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64
S = "sha256:" + "5" * 64
MI = "sha256:" + "6" * 64
PF = "sha256:" + "7" * 64
RI = "sha256:" + "8" * 64


def view(provider: str, *, preparation: bool = False) -> ProviderPlanningViewV3:
    topology = DeviceTopologyProfile(provider, (), "cpu")
    residency = () if preparation else (ResidencyProofV3(
        artifact_digest=A, role="stage0", rank=0, tier=ResidencyTierV3.DISK,
        boot_epoch="boot-0001", process_epoch="process-0001",
        topology_digest=topology.digest(),
        residency_class=ResidencyClassV3.ASSEMBLED_FRAGMENT,
        identity_digest=RI, assembly_spec_digest=R,
        model_manifest_digest=MI, artifact_profile_digest=PF,
        graph_digest=G, backend="cpu",
        protection_epoch="plaintext-v1"),)
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
    def test_security_finalization_requires_real_grants_only_for_protected_roles(self):
        provider_view = view("p0")
        plaintext_core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
            {"p0": provider_view})
        plaintext_plan = PlanSealerV3.finalize_security(
            plaintext_core, (), S)
        self.assertTrue(plaintext_plan.startswith("sha256:"))

        protected_proposal = proposal()
        protected_role = RoleAssemblySpec(
            **{
                **protected_proposal.roles[0].__dict__,
                "protection_epoch": "policy-epoch-7",
            }
        )
        protected_proposal = PlacementProposalV3(
            **{
                **protected_proposal.__dict__,
                "roles": (protected_role,),
            }
        )
        protected_core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S},
            protected_proposal, {"p0": view("p0", preparation=True)})
        with self.assertRaisesRegex(ValueError, "grant cover"):
            PlanSealerV3.finalize_security(protected_core, (), S)
        binding = GrantBindingV1(
            provider="p0",
            grant_name="/authority/grants/req-1/p0",
            grant_digest="sha256:" + "9" * 64,
            request_id="req-1", attempt=1,
            plan_core_digest=protected_core.plan_core_digest,
            security_policy_snapshot_digest=S,
            protection_epoch="policy-epoch-7",
        )
        self.assertTrue(PlanSealerV3.finalize_security(
            protected_core, (binding,), S).startswith("sha256:"))

    def test_protected_selection_carries_exact_provider_grant_binding(self):
        base = proposal()
        protected_role = RoleAssemblySpec(
            **{**base.roles[0].__dict__, "protection_epoch": "policy-epoch-7"})
        protected = PlacementProposalV3(
            **{**base.__dict__, "roles": (protected_role,)})
        provider_view = view("p0", preparation=True)
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, protected,
            {"p0": provider_view})
        binding = GrantBindingV1(
            provider="p0", grant_name="/authority/grants/req-1/p0",
            grant_digest="sha256:" + "9" * 64,
            request_id="req-1", attempt=1,
            plan_core_digest=core.plan_core_digest,
            security_policy_snapshot_digest=S,
            protection_epoch="policy-epoch-7")
        final = PlanSealerV3.finalize_security(core, (binding,), S)

        projection = PlanSealerV3.project(
            core, plan_digest=final, provider="p0", offer=provider_view,
            security_policy_snapshot_digest=S,
            execution_role=ExecutionRole(
                "stage0", "stage0", 0, 0, 2, "cpu"),
            assembly=protected_role,
            dataflow=RoleDataflowContract(
                "req-1", 1, final, "stage0", terminal_response_owner=True),
            device_binding=DeviceBinding(
                DeviceBindingMode.CPU, "p0", "stage0",
                provider_view.offer_digest, provider_view.topology.digest(),
                canonical_digest(provider_view.resources), 1),
            deadline_ms=100, grant_binding=binding)
        decoded = ProviderSelectionProjectionV3.from_bytes(
            projection.to_bytes())
        self.assertEqual(decoded.grant_binding, binding)

        with self.assertRaisesRegex(ValueError, "grant binding"):
            PlanSealerV3.project(
                core, plan_digest=final, provider="p0", offer=provider_view,
                security_policy_snapshot_digest=S,
                execution_role=projection.execution_role,
                assembly=protected_role, dataflow=projection.dataflow,
                device_binding=projection.device_binding, deadline_ms=100)

    def test_projection_carries_complete_offer_and_security_binding(self):
        provider_view = view("p0")
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, proposal(),
            {"p0": provider_view})
        final = PlanSealerV3.finalize_security(core, (), S)
        role = core.roles[0]
        projection = PlanSealerV3.project(
            core, plan_digest=final, provider="p0", offer=provider_view,
            security_policy_snapshot_digest=S,
            execution_role=ExecutionRole(
                "stage0", "stage0", 0, 0, 2, "cpu"),
            assembly=role,
            dataflow=RoleDataflowContract(
                "req-1", 1, final, "stage0", terminal_response_owner=True),
            device_binding=DeviceBinding(
                DeviceBindingMode.CPU, "p0", "stage0",
                provider_view.offer_digest, provider_view.topology.digest(),
                canonical_digest(provider_view.resources), 1),
            deadline_ms=100,
        )
        decoded = ProviderSelectionProjectionV3.from_bytes(
            projection.to_bytes())
        self.assertEqual(decoded.ack_closed_digest, S)
        self.assertEqual(decoded.offer_digest, provider_view.offer_digest)
        self.assertEqual(decoded.security_policy_snapshot_digest, S)
        self.assertEqual(decoded.device_binding.offer_digest,
                         decoded.offer_digest)

    def test_external_proposal_rejects_opaque_runtime_content(self):
        base = proposal()
        with self.assertRaisesRegex(ValueError, "executable/runtime"):
            PlacementProposalV3(
                request_id=base.request_id, attempt=base.attempt,
                model_digest=base.model_digest, graph_digest=base.graph_digest,
                roles=base.roles, provider_by_role=base.provider_by_role,
                dependencies=({"runtime_object": object()},),
                strategy_name="custom-v3", strategy_version="1",
                strategy_state_digest=S)

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
        final = PlanSealerV3.finalize_security(core, (), S)
        self.assertNotEqual(final, core.plan_core_digest)
        self.assertEqual(
            final,
            PlanSealerV3.finalize_security(core, (), S))

    def test_incomplete_or_substituted_grants_fail(self):
        base = proposal()
        protected_role = RoleAssemblySpec(
            **{**base.roles[0].__dict__, "protection_epoch": "policy-epoch-7"})
        protected = PlacementProposalV3(
            **{**base.__dict__, "roles": (protected_role,)})
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "ack_closed_digest": S}, protected,
            {"p0": view("p0", preparation=True)})
        with self.assertRaises(ValueError):
            PlanSealerV3.finalize_security(core, (), S)
        binding = GrantBindingV1(
            provider="p0", grant_name="/authority/grants/req-1/p0",
            grant_digest="sha256:" + "9" * 64,
            request_id="req-1", attempt=1,
            plan_core_digest=core.plan_core_digest,
            security_policy_snapshot_digest=R,
            protection_epoch="policy-epoch-7")
        with self.assertRaises(ValueError):
            PlanSealerV3.finalize_security(core, (binding,), S)

    def test_multi_rank_roles_require_distinct_providers(self):
        roles = tuple(
            RoleAssemblySpec(
                role="tensor", rank=rank, layer_begin=rank,
                layer_end=rank + 1, recipe_digest=R, artifact_digest=A,
                backend="cuda", device_set=("cuda:0",),
            ) for rank in (0, 1)
        )

        with self.assertRaisesRegex(ValueError, "one-to-one"):
            PlacementProposalV3(
                request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
                roles=roles,
                provider_by_role={"tensor#0": "p0", "tensor#1": "p0"},
                strategy_name="pre-split-first", strategy_version="3",
                strategy_state_digest=S,
            )

        views = {}
        for rank, provider in enumerate(("p0", "p1")):
            topology = DeviceTopologyProfile(provider, ("cuda:0",), "cuda")
            proof = ResidencyProofV3(
                artifact_digest=A, role="tensor", rank=rank,
                tier=ResidencyTierV3.GPU, device_set=("cuda:0",),
                boot_epoch="boot-0001", process_epoch="process-0001",
                topology_digest=topology.digest(),
                residency_class=ResidencyClassV3.LOADED_RUNTIME,
                identity_digest=RI, assembly_spec_digest=R,
                model_manifest_digest=MI, artifact_profile_digest=PF,
                graph_digest=G, backend="cuda",
                protection_epoch="plaintext-v1", runtime_generation=1,
                fencing_token="fence-1",
            )
            offer = ProviderOfferV3(
                request_id="req-1", attempt=1, service="/LLM/Qwen",
                provider=provider, model_digest=M, graph_digest=G, status=True,
                execution_disposition=ExecutionDisposition.ACCEPT_IF_EXACT_REUSE,
                preparation_accepted=False, topology=topology,
                residency=(proof,), accepted_roles=("tensor",),
                backends=("cuda",), boot_epoch="boot-0001",
                captured_at_ms=1, expires_at_ms=100,
                signer_key_id="signed-key", signature="signed",
            )
            views[provider] = ProviderPlanningViewV3.from_offer(offer)

        proposal_v3 = PlacementProposalV3(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            roles=roles,
            provider_by_role={"tensor#0": "p0", "tensor#1": "p1"},
            strategy_name="pre-split-first", strategy_version="3",
            strategy_state_digest=S,
        )
        core = PlanSealerV3.seal_core(
            {"request_id": "req-1", "attempt": 1, "ack_closed_digest": S},
            proposal_v3, views)
        grants = tuple(
            PlanSealerV3.grant_view(core, provider, views[provider], S)
            for provider in ("p0", "p1")
        )
        self.assertEqual([len(grant.role_digests) for grant in grants], [1, 1])


if __name__ == "__main__":
    unittest.main()
