from __future__ import annotations

import copy
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


def load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text())


class AllocationTopologyTest(unittest.TestCase):
    def test_single_and_multi_node_process_maps(self) -> None:
        single = topology.validate_process_map(load("single-node.json"))
        multi = topology.validate_process_map(load("multi-node-tcp.json"))
        self.assertEqual(1, len(single["nodes"]))
        self.assertEqual(2, len(multi["nodes"]))
        self.assertEqual(2, sum(row["kind"] == "nfd" for row in multi["processes"]))
        self.assertEqual(3, sum(row["kind"] == "provider" for row in multi["processes"]))

    def test_udp_is_an_independent_selected_transport_variant(self) -> None:
        value = load("multi-node-tcp.json")
        value.update(load("variants.json")["udp"])
        result = topology.validate_process_map(value)
        self.assertEqual("udp", result["selectedTransport"])
        self.assertTrue(all(route["transport"] == "udp" for route in result["routes"]))

    def test_duplicate_identity_fails_closed(self) -> None:
        value = load("single-node.json")
        value["processes"][3]["identityRef"] = value["processes"][2]["identityRef"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_DUPLICATE_IDENTITY"):
            topology.validate_process_map(value)

    def test_partial_readiness_fails_closed(self) -> None:
        value = load("single-node.json")
        value["processes"][-1]["readinessInputs"] = ["provider-not-ready"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_READINESS_DEPENDENCY_INVALID"):
            topology.validate_process_map(value)

    def test_shell_injection_fails_before_render(self) -> None:
        value = load("single-node.json")
        value["processes"][-1]["command"] = ["python3", "user.py;touch", "/tmp/escaped"]
        value["processes"][-1]["commandDigest"] = topology.command_digest(value["processes"][-1]["command"])
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_COMMAND_UNSAFE"):
            topology.render_multiprog(value)

    def test_duplicate_nfd_fails_closed(self) -> None:
        value = load("single-node.json")
        duplicate = copy.deepcopy(value["processes"][0])
        duplicate.update(processId="nfd-extra", taskRank=6, readinessOutput="nfd-extra-ready", shutdownOrder=7)
        value["processes"].append(duplicate)
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_DUPLICATE_NFD"):
            topology.validate_process_map(value)

    def test_teardown_signal_and_audit_are_mandatory(self) -> None:
        for field, changed in (("signals", ["TERM"]), ("zeroSurvivorAudit", False)):
            value = load("single-node.json")
            value[field] = changed
            with self.subTest(field=field), self.assertRaisesRegex(
                topology.TopologyError, "TOPOLOGY_TEARDOWN_POLICY_INVALID"
            ):
                topology.validate_process_map(value)

    def test_nfd_and_multiprog_rendering_is_complete(self) -> None:
        value = load("single-node.json")
        template = (REPO / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/nfd.conf.in").read_text()
        rendered = topology.render_nfd_config(template, value["nodes"][0], "/tmp/ndnsf-di-job/nfd/0")
        self.assertNotIn("@@", rendered)
        self.assertIn("port 16363", rendered)
        multiprog = topology.render_multiprog(value)
        self.assertEqual(len(value["processes"]), len(multiprog.splitlines()))
        self.assertIn("0 nfd --config /tmp/spec110/nfd-0.conf", multiprog)


if __name__ == "__main__":
    unittest.main()
