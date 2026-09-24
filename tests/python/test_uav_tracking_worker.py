from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
from tracking_worker import CrossCameraAssociator, TrackingEngine  # noqa: E402


class TrackingWorkerTest(unittest.TestCase):
    def test_functional_worker_preserves_source_time_and_ids(self) -> None:
        worker = TrackingEngine(functional=True)
        image = np.zeros((32, 48, 3), dtype=np.uint8)
        first = worker.process_frame("UAV1", 0, 1234, image)
        second = worker.process_frame("UAV1", 1, 501234, image)
        self.assertEqual(first["ptsUs"], 1234)
        self.assertEqual(first["tracks"][0]["localId"], second["tracks"][0]["localId"])
        self.assertEqual(first["tracks"][0]["globalId"], second["tracks"][0]["globalId"])
        self.assertEqual(first["algorithmProfile"], "functional-test-v1")

    def test_real_worker_requires_explicit_checkpoint(self) -> None:
        with self.assertRaises(FileNotFoundError):
            TrackingEngine(Path("/does/not/exist.pt"))

    def test_functional_worker_processes_canonical_multi_view_window(self) -> None:
        worker = TrackingEngine(functional=True)
        image = np.zeros((32, 48, 3), dtype=np.uint8)
        ok, encoded = cv2.imencode(".jpg", image)
        self.assertTrue(ok)
        payload = bytes(encoded)
        result = worker.process_window(
            {"runId": "r", "missionId": "m", "windowId": "w", "sequence": 1},
            [({"cameraId": camera, "sequence": 1, "ptsUs": index * 1000}, payload)
             for index, camera in enumerate(("UAV1", "UAV2", "UAV3"))])
        self.assertEqual(result["schema"], "spec191-tracking-result-v1")
        self.assertEqual([item["cameraId"] for item in result["frames"]],
                         ["UAV1", "UAV2", "UAV3"])
        self.assertTrue(all(item["inputDigest"].startswith("sha256:")
                            for item in result["frames"]))
        self.assertTrue(all(item["imageJpeg"] for item in result["frames"]))

    def test_session_state_survives_worker_process_boundary(self) -> None:
        image = np.zeros((32, 48, 3), dtype=np.uint8)
        ok, encoded = cv2.imencode(".jpg", image)
        self.assertTrue(ok)
        payload = bytes(encoded)
        frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
                  for camera in ("UAV1", "UAV2", "UAV3")]
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "tracking-state.json"
            first_worker = TrackingEngine(functional=True)
            first = first_worker.process_window(
                {"runId": "r", "missionId": "m", "windowId": "w0", "sequence": 1},
                frames)
            first_worker.save_state(state)
            second_worker = TrackingEngine(functional=True)
            second_worker.restore_state(state)
            second_frames = [({"cameraId": camera, "sequence": 2, "ptsUs": 1_000_000}, payload)
                             for camera in ("UAV1", "UAV2", "UAV3")]
            second = second_worker.process_window(
                {"runId": "r", "missionId": "m", "windowId": "w1", "sequence": 2},
                second_frames)
        self.assertEqual(
            [item["tracks"][0]["localId"] for item in first["frames"]],
            [item["tracks"][0]["localId"] for item in second["frames"]])
        self.assertEqual(
            [item["tracks"][0]["globalId"] for item in first["frames"]],
            [item["tracks"][0]["globalId"] for item in second["frames"]])

    def test_cross_camera_association_uses_overlap_lane_and_source_pts(self) -> None:
        associator = CrossCameraAssociator(max_gap_us=1_000_000)
        first, first_event = associator.observe("UAV1", 7, "car", (80, 40, 100, 60),
                                                1_000_000, (100, 100))
        second, second_event = associator.observe("UAV2", 3, "car", (0, 40, 20, 60),
                                                 1_500_000, (100, 100))
        self.assertIsNone(first_event)
        self.assertIsNotNone(second_event)
        self.assertEqual(second, first)
        self.assertEqual(second_event["reason"], "overlap-lane-pts-center")

    def test_cross_camera_association_rejects_stale_candidate(self) -> None:
        associator = CrossCameraAssociator(max_gap_us=100)
        first, _ = associator.observe("UAV1", 7, "car", (80, 40, 100, 60),
                                      1_000_000, (100, 100))
        second, event = associator.observe("UAV2", 3, "car", (0, 40, 20, 60),
                                           1_000_500, (100, 100))
        self.assertIsNone(event)
        self.assertNotEqual(second, first)

    def test_cross_camera_association_rejects_candidate_outside_entry_gate(self) -> None:
        associator = CrossCameraAssociator(max_gap_us=1_000_000)
        first, _ = associator.observe("UAV1", 7, "car", (80, 40, 100, 60),
                                      1_000_000, (100, 100))
        # Same class, lane, and source-time window, but not in UAV2's
        # registered entry rectangle.  This must not consume the pending ID.
        second, event = associator.observe("UAV2", 3, "car", (0, 90, 20, 100),
                                           1_500_000, (100, 100))
        self.assertIsNone(event)
        self.assertNotEqual(second, first)

    def test_cross_camera_association_uses_route_specific_lane_thresholds(self) -> None:
        associator = CrossCameraAssociator(max_gap_us=5_000_000)
        # These are normalized equivalents of the recorded UAV2->UAV3
        # handoff: the source is above UAV2's right-side lane split, while the
        # destination is above UAV3's shifted split.
        first, _ = associator.observe("UAV2", 8, "car", (3034, 1248, 3140, 1303),
                                      28_200_000, (3840, 2160))
        second, event = associator.observe("UAV3", 13, "car", (768, 863, 882, 926),
                                           31_500_000, (3840, 2160))
        self.assertIsNotNone(event)
        self.assertEqual(second, first)
        self.assertEqual(event["sourceCamera"], "UAV2")
        self.assertEqual(event["targetCamera"], "UAV3")


if __name__ == "__main__":
    unittest.main()
