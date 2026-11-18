"""Regressions for qualification harness guards, not native tracking behavior."""

import copy
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


runner = load("spec226_acceptance_runner", ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py")
browser = load("spec226_acceptance_browser", ROOT / "Experiments/UAV/verify_installed_dashboard.py")


class PlanGuardsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "plan.json"
        placement = {"uav1-camera": "uav1", "uav2-camera": "uav2", "uav3-camera": "uav3",
                     "compute": "compute", "ground-station": "gs", "controller": "gs",
                     "browser-driver": "gs"}
        programs = {"controller": "/nix/store/test-native/bin/App_ServiceController",
                    "compute": "/nix/store/test-native/bin/UavTrackingNode",
                    "ground-station": "/nix/store/test-dashboard/bin/tracking-dashboard",
                    **{name: "/nix/store/test-camera/bin/tracking-camera"
                       for name in placement if name.endswith("-camera")}}
        self.plan = {"applicationMode": "installed-dashboard", "settings": {"fault": "none"},
                     "topology": {"nodes": ["uav1", "uav2", "uav3", "compute", "gs"]},
                     "installations": {"native": "/nix/store/test-native",
                                       "dashboard": "/nix/store/test-dashboard",
                                       "camera": "/nix/store/test-camera"},
                     "controllerCertificate": "/test/controller.cert", "assets": [],
                     "processes": [{"name": name, "node": node, "cwd": "/",
                                    "command": ([sys.executable, str(ROOT / "Experiments/UAV/verify_installed_dashboard.py")]
                                                if name == "browser-driver" else [programs[name]])}
                                   for name, node in placement.items()]}

    def test_all_roles_on_gs_are_rejected_before_network_or_credentials(self):
        for process in self.plan["processes"]:
            process["node"] = "gs"
        self.path.write_text(json.dumps(self.plan))
        with self.assertRaisesRegex(runner.RunnerError, "placement"):
            runner._read_installed_dashboard_plan(self.path)

    def test_old_installed_actor_cannot_mix_with_frozen_roots(self):
        process = self.plan["processes"][0]
        process["command"][0] = "/nix/store/old-camera/bin/tracking-camera"
        self.path.write_text(json.dumps(self.plan))
        with patch.object(runner.os, "access", return_value=True):
            with self.assertRaisesRegex(runner.RunnerError, "frozen Nix installation"):
                runner._read_installed_dashboard_plan(self.path)

    def test_unconfigured_label_cannot_qualify_a_configured_app(self):
        self.plan["settings"]["dashboardExpectation"] = "unconfigured"
        station = next(item for item in self.plan["processes"] if item["name"] == "ground-station")
        station["command"].extend(["--tracking-job-config", "/test/job.json"])
        browser_process = next(item for item in self.plan["processes"] if item["name"] == "browser-driver")
        browser_process["command"].extend(["--expect", "unconfigured"])
        self.path.write_text(json.dumps(self.plan))
        with self.assertRaisesRegex(runner.RunnerError, "omit tracking configuration"):
            runner._read_installed_dashboard_plan(self.path)

    def test_unconfigured_label_requires_the_unconfigured_browser_oracle(self):
        self.plan["settings"]["dashboardExpectation"] = "unconfigured"
        browser_process = next(item for item in self.plan["processes"] if item["name"] == "browser-driver")
        browser_process["command"].extend(["--expect", "completed"])
        self.path.write_text(json.dumps(self.plan))
        with self.assertRaisesRegex(runner.RunnerError, "browser oracle"):
            runner._read_installed_dashboard_plan(self.path)


class DomOracleTest(unittest.TestCase):
    def setUp(self):
        frame = {"width": 360, "height": 288,
                 "tracks": [{"globalId": 1, "box": [10, 20, 50, 120]}]}
        self.display = {"windowIndex": 2, "resultDigest": "sha256:known",
                        "resultJson": json.dumps({"frames": [dict(frame, cameraId="UAV" + str(i))
                                                             for i in range(1, 4)]})}
        self.cells = [{"camera": "UAV" + str(i), "window": "2", "digest": "sha256:known",
                       "ready": True, "bounds": {"x": 0, "y": 0, "width": 180, "height": 144},
                       "boxes": [{"id": "1", "bounds": {"x": 5, "y": 10,
                                                          "width": 20, "height": 50}}]}
                      for i in range(1, 4)]

    def page(self):
        cells = copy.deepcopy(self.cells)
        return type("Page", (), {"evaluate": lambda _self, _script: cells})()

    def test_known_equal_scale_boxes_pass(self):
        self.assertTrue(browser.check_dom(self.page(), self.display))

    def test_wrong_box_height_fails(self):
        self.cells[1]["boxes"][0]["bounds"]["height"] = 20
        with self.assertRaises(AssertionError):
            browser.check_dom(self.page(), self.display)

    def test_atomic_sample_of_a_newer_group_requests_retry(self):
        for cell in self.cells:
            cell["digest"] = "sha256:newer-group"
        self.assertFalse(browser.check_dom(self.page(), self.display))


if __name__ == "__main__":
    unittest.main()
