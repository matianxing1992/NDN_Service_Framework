"""MiniNDN-facing preflight tests for the Spec191 five-node harness.

These checks intentionally stop before MiniNDN when native binaries or root
privileges are absent.  They validate that the launcher cannot silently turn a
five-node topology into a sixth Controller/forwarder node.
"""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.run_multicamera_tracking_demo import build_plan, preflight  # noqa: E402
from Experiments.UAV.tracking_topology import EXPECTED_NODES, Topology, TopologyError  # noqa: E402

ASSETS = Path("/home/tianxing/.cache/ndnsf/spec191-assets")


@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_preflight_keeps_controller_inside_ground_station_node():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True)
        report = preflight(plan, run=False, headless=True)
    assert report["ok"]
    assert set(report["nodes"]) == EXPECTED_NODES
    assert report["processCount"] == 7  # five network nodes + gs-local Controller + renderer


@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_process_plan_uses_real_tracking_ground_station_and_scoped_policy():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True)
    by_name = {item["name"]: item for item in plan["processes"]}
    assert "UavTrackingGroundStation" in by_name["ground-station"]["command"][0]
    controller = " ".join(by_name["controller"]["command"])
    assert "Experiments/UAV/spec191.policies" in controller
    assert by_name["controller"]["node"] == by_name["ground-station"]["node"] == "gs"


def test_extra_network_controller_is_rejected():
    with pytest.raises(TopologyError):
        Topology(frozenset((*EXPECTED_NODES, "controller")), frozenset()).validate()


@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_run_preflight_reports_environment_blockers_without_starting_network():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True)
        report = preflight(plan, run=True, headless=True)
    if report["ok"]:
        pytest.skip("native binaries and MiniNDN root privilege are available")
    assert any("native binary" in item or "root" in item for item in report["errors"])
