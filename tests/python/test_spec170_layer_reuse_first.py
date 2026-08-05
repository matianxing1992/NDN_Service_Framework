from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.planner.layer_reuse_first import LayerReuseFirstStrategy  # noqa: E402
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceTopologyProfile, ExecutionDisposition, ProviderOfferV3,
    ProviderPlanningViewV3, ResidencyProofV3, ResidencyTierV3, RoleAssemblySpec,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
A = "sha256:" + "3" * 64
R = "sha256:" + "4" * 64


def provider(name: str, *, exact: bool, queue: int) -> ProviderPlanningViewV3:
    topology = DeviceTopologyProfile(name, (), "cpu")
    proof = (ResidencyProofV3(
        A, "stage0", 0, ResidencyTierV3.DISK, (), "boot-0001",
        "process-0001", topology.digest()),) if exact else ()
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
    def test_exact_residency_wins_over_cold_provider(self):
        strategy = LayerReuseFirstStrategy(at_ms=1)
        role = RoleAssemblySpec("stage0", 0, 0, 2, R, A, "cpu")
        decision = strategy.propose_v3(
            request_id="req-1", attempt=1, model_digest=M, graph_digest=G,
            roles=(role,), providers=(provider("cold", exact=False, queue=0),
                                      provider("warm", exact=True, queue=5)),
            ack_closed_digest="sha256:" + "5" * 64)
        self.assertEqual(decision.provider_by_role["stage0"], "warm")


if __name__ == "__main__":
    unittest.main()
