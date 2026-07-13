from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest


REPO = Path(__file__).resolve().parents[4]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
if str(LIB) not in sys.path:
    sys.path.insert(0, str(LIB))

import allocation_topology as topology


FIXTURES = REPO / "tests/container/itiger-qwen-live/fixtures/network"


class MultinodeProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.process_map = json.loads((FIXTURES / "multi-node-tcp.json").read_text())
        cls.variants = json.loads((FIXTURES / "variants.json").read_text())

    def test_selected_tcp_pass_is_not_blocked_by_udp_diagnostic_failure(self) -> None:
        result = topology.evaluate_transport_probe(
            self.process_map,
            self.variants["probeObservations"]["tcp-pass-udp-diagnostic-fail"],
        )
        self.assertEqual("PASS", result["status"])
        self.assertEqual("FAIL", result["diagnosticStatus"])

    def test_closed_selected_tcp_port_blocks(self) -> None:
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_SELECTED_TRANSPORT_FAILED:tcp"):
            topology.evaluate_transport_probe(
                self.process_map, self.variants["probeObservations"]["tcp-closed"]
            )

    def test_allocation_address_mismatch_prevents_false_multinode_pass(self) -> None:
        observation = json.loads(json.dumps(
            self.variants["probeObservations"]["tcp-pass-udp-diagnostic-fail"]
        ))
        observation["allocationAddresses"] = ["10.10.0.10", "192.0.2.99"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_PROBE_ADDRESS_MISMATCH"):
            topology.evaluate_transport_probe(self.process_map, observation)

    def test_udp_candidate_requires_udp_pass_even_if_tcp_passes(self) -> None:
        process_map = json.loads(json.dumps(self.process_map))
        process_map.update(self.variants["udp"])
        observation = self.variants["probeObservations"]["tcp-closed"]
        result = topology.evaluate_transport_probe(process_map, observation)
        self.assertEqual("udp", result["selectedTransport"])
        self.assertEqual("PASS", result["selectedStatus"])


if __name__ == "__main__":
    unittest.main()
