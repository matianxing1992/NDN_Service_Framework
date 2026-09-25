from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.run_multicamera_tracking_demo import (
    ProcessSupervisor,
    RunnerError,
    _child_pythonpath,
    build_plan,
    preflight,
)


ASSETS = Path("/home/tianxing/.cache/ndnsf/spec191-assets")


class TrackingRunnerTest(unittest.TestCase):
    def test_child_pythonpath_preserves_launcher_import_roots(self) -> None:
        paths = _child_pythonpath().split(":")
        self.assertIn(str(ROOT), paths)
        self.assertTrue(any(path.endswith("site-packages") for path in paths))

    def test_plan_has_exact_five_nodes_and_gs_local_controller(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            plan = build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                              output=Path(temporary), license_text="local-test-accepted",
                              headless=True)
        self.assertEqual(plan["topology"]["nodes"], ["compute", "gs", "uav1", "uav2", "uav3"])
        self.assertEqual({item["node"] for item in plan["processes"]},
                         {"compute", "gs", "uav1", "uav2", "uav3"})
        self.assertEqual([item for item in plan["processes"] if item["name"] == "controller"][0]["node"], "gs")
        self.assertNotIn("controller", plan["topology"]["nodes"])
        identities = {item["name"]: item["identity"] for item in plan["processes"]}
        self.assertEqual(identities["uav1-camera"], "/example/uav/drone/UAV1")
        self.assertEqual(identities["uav2-camera"], "/example/uav/drone/UAV2")
        self.assertEqual(identities["uav3-camera"], "/example/uav/drone/UAV3")
        self.assertEqual(plan["settings"]["windowCount"], 3)
        for camera in ("UAV1", "UAV2", "UAV3"):
            process = plan["processes"][[item["name"] for item in plan["processes"]].index(
                f"uav{camera[-1]}-camera")]
            self.assertIn("--frame-dir", process["command"])
        self.assertIn("--window-count", plan["processes"][-2]["command"])

    def test_plan_rejects_unbounded_or_empty_replay(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            for count in (0, 61):
                with self.assertRaises(RunnerError):
                    build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                               output=Path(temporary), license_text="local-test-accepted",
                               headless=True, window_count=count)

    def test_late_response_loss_is_an_explicit_bounded_fault(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            plan = build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                              output=Path(temporary), license_text="local-test-accepted",
                              headless=True, window_count=3,
                              fault="late-response-loss")
        self.assertEqual(plan["settings"]["fault"], "late-response-loss")
        compute = next(item for item in plan["processes"] if item["name"] == "compute")
        self.assertIn("late-response-loss", compute["command"])

    def test_run_preflight_reports_missing_native_and_root_without_starting(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            plan = build_plan(source=ASSETS, model=ASSETS / "3UAVs.pt",
                              output=Path(temporary), license_text="local-test-accepted",
                              headless=True)
            report = preflight(plan, run=True, headless=True)
        self.assertFalse(report["ok"])
        self.assertTrue(any("native binary" in error or "root" in error
                            for error in report["errors"]))

    def test_supervisor_rejects_missing_executable_before_child_start(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            supervisor = ProcessSupervisor(Path(temporary))
            from Experiments.UAV.run_multicamera_tracking_demo import ProcessSpec
            spec = ProcessSpec("missing", "gs", "/id", Path(temporary),
                               ("does-not-exist-spec191",), Path(temporary) / "private")
            with self.assertRaises(RunnerError):
                supervisor.start(spec, {"PATH": "/usr/bin"})


if __name__ == "__main__":
    unittest.main()
