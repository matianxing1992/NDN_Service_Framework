from __future__ import annotations

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceResourceSnapshot,
    DeviceTopologyProfile,
    ExecutionDisposition,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    ResidencyProofV3,
    ResidencyTierV3,
    decode_placement_wire,
)


MODEL = "sha256:" + "1" * 64
GRAPH = "sha256:" + "2" * 64


def offer(*, disposition=ExecutionDisposition.ACCEPT_IF_EXACT_REUSE,
          status=True, preparation=False, devices=(), residency=True):
    topology = DeviceTopologyProfile("p0", devices, "cpu")
    proof = ()
    if residency:
        proof = (ResidencyProofV3(
            artifact_digest="sha256:" + "3" * 64, role="stage0", rank=0,
            tier=ResidencyTierV3.DISK, device_set=(), boot_epoch="boot-0001",
            process_epoch="process-0001", topology_digest=topology.digest()),)
    return ProviderOfferV3(
        request_id="request-1", attempt=1, service="/LLM/Qwen",
        provider="p0", model_digest=MODEL, graph_digest=GRAPH, status=status,
        execution_disposition=disposition, preparation_accepted=preparation,
        topology=topology, resources=tuple(DeviceResourceSnapshot(
            d, 0, 0, topology_digest=topology.digest()) for d in devices),
        residency=proof, accepted_roles=("stage0",), backends=("cpu",),
        boot_epoch="boot-0001", captured_at_ms=10, expires_at_ms=100,
        signer_key_id="signed-key", signature="signed")


class Spec170PlacementV3Test(unittest.TestCase):
    def test_exact_reuse_is_positive_but_preparation_false(self):
        value = offer()
        self.assertTrue(value.status)
        self.assertFalse(value.preparation_accepted)
        self.assertFalse(value.ack_reservation)
        self.assertIsInstance(
            ProviderPlanningViewV3.from_offer(value), ProviderPlanningViewV3)

    def test_preparation_and_reject_tuples(self):
        prepared = offer(
            disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation=True, residency=False)
        rejected = offer(
            disposition=ExecutionDisposition.REJECT, status=False,
            preparation=False, residency=False)
        self.assertTrue(prepared.status)
        self.assertFalse(rejected.status)
        with self.assertRaises(ValueError):
            ProviderPlanningViewV3.from_offer(rejected)

    def test_contradictory_tuple_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "disposition tuple"):
            offer(preparation=True)
        with self.assertRaisesRegex(ValueError, "disposition tuple"):
            offer(disposition=ExecutionDisposition.REJECT, status=False,
                  preparation=True, residency=False)

    def test_zero_one_many_devices_are_truthful(self):
        for devices in ((), ("cuda:0",), ("cuda:0", "cuda:1")):
            value = offer(devices=devices, residency=False,
                          disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
                          preparation=True)
            self.assertEqual(tuple(value.topology.devices), devices)

    def test_canonical_v3_round_trip_and_dispatch(self):
        value = offer()
        decoded = decode_placement_wire(value.to_bytes())
        self.assertEqual(decoded.digest(), value.digest())
        self.assertEqual(decoded.to_bytes(), value.to_bytes())
        with self.assertRaisesRegex(ValueError, "unknown placement schema"):
            decode_placement_wire(b'{"schema":"DI_PLACEMENT_V3_OLD"}')


if __name__ == "__main__":
    unittest.main()
