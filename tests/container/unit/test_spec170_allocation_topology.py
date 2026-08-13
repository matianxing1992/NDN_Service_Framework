from __future__ import annotations

import sys
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
_LIB_INSERTED = str(LIB) not in sys.path
if _LIB_INSERTED:
    sys.path.insert(0, str(LIB))

import spec170_allocation_topology as topology

# ``lib`` also contains a deployment-helper named ``profile.py``.  Keep the
# import path scoped to this module so later tests can import Python's standard
# ``profile`` module (and packages that depend on it) without shadowing.
if _LIB_INSERTED:
    sys.path.remove(str(LIB))


def profile(gate: str) -> dict:
    contract = topology.GATE_CONTRACTS[gate]
    return {
        "schemaVersion": "spec170-allocation-topology-v1",
        "gate": gate,
        "sourceDigest": "sha256:" + "1" * 64,
        "ociDigest": "sha256:" + "2" * 64,
        "sifPath": "/project/tma1/ndnsf-di/releases/spec170.sif",
        "sifSha256": "sha256:" + "3" * 64,
        "nodeCount": contract["nodeCount"],
        "gpuCount": contract["gpuCount"],
        "providerCount": contract["providerCount"],
        "nv": contract["nv"],
        "providerPlacement": {
            "d0-cpu": "one-provider-cpu",
            "d1-single": "one-provider-one-gpu",
            "d2a-local-two-gpu": "one-provider-two-gpu",
            "d2b-cross-provider": "two-provider-one-gpu-each",
            "d2h-hybrid": "two-provider-hybrid",
        }[gate],
        "workload": "ndnsf-di-gate",
        "hiddenDefaults": False,
    }


class Spec170AllocationTopologyTest(unittest.TestCase):
    def test_all_gate_contracts_validate(self) -> None:
        for gate in topology.GATE_CONTRACTS:
            with self.subTest(gate=gate):
                self.assertEqual(gate, topology.validate_gate_profile(profile(gate))["gate"])

    def test_cpu_gate_has_no_nv_and_no_gpu(self) -> None:
        value = profile("d0-cpu")
        value["nv"] = True
        with self.assertRaisesRegex(topology.Spec170TopologyError, "ALLOCATION_CONTRACT_MISMATCH"):
            topology.validate_gate_profile(value)

    def test_cross_provider_cannot_be_relabelled_local_two_gpu(self) -> None:
        value = profile("d2b-cross-provider")
        value["providerPlacement"] = "one-provider-two-gpu"
        with self.assertRaisesRegex(topology.Spec170TopologyError, "PLACEMENT_MISMATCH"):
            topology.validate_gate_profile(value)

    def test_hidden_defaults_and_bad_digest_fail_closed(self) -> None:
        value = profile("d1-single")
        value["hiddenDefaults"] = True
        with self.assertRaisesRegex(topology.Spec170TopologyError, "HIDDEN_DEFAULTS"):
            topology.validate_gate_profile(value)
        value = profile("d1-single")
        value["ociDigest"] = "latest"
        with self.assertRaisesRegex(topology.Spec170TopologyError, "DIGEST_INVALID"):
            topology.validate_gate_profile(value)


if __name__ == "__main__":
    unittest.main()
