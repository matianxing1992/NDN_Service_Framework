from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.tracking_topology import (
    EXPECTED_LINKS,
    EXPECTED_NODES,
    Topology,
    TopologyError,
    load_topology,
)


def test_spec191_topology_is_exactly_five_nodes_and_four_links():
    topology = load_topology(ROOT / "Experiments/UAV/topology.conf")
    assert topology.nodes == EXPECTED_NODES
    assert topology.links == EXPECTED_LINKS
    assert topology.controller_host == "gs"
    assert topology.compute_host == "compute"
    assert topology.link_attributes is not None
    assert all(attributes == {"delay": "2ms", "bw": "100", "loss": "0"}
               for attributes in topology.link_attributes.values())


def test_extra_controller_or_forwarder_is_rejected():
    with pytest.raises(TopologyError):
        Topology(
            frozenset((*EXPECTED_NODES, "controller")),
            EXPECTED_LINKS,
        ).validate()


def test_wrong_star_link_is_rejected():
    with pytest.raises(TopologyError):
        Topology(
            EXPECTED_NODES,
            frozenset((*EXPECTED_LINKS, ("gs", "uav1"))),
        ).validate()
