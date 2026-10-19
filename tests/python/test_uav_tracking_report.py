from __future__ import annotations

import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.tracking_report import (FetchedProduct, ResultError, collect_result,
                                             digest, summarize_motion_result)


class TrackingReportTest(unittest.TestCase):
    def test_legacy_report_preserves_explicit_camera_identity(self) -> None:
        estimates = [
            {"cameraId": "UAV1", "globalId": 7, "status": "ok",
             "vehicleSpeedMmps": 5000},
            {"cameraId": "UAV2", "globalId": 7, "status": "ok",
             "vehicleSpeedMmps": 12000},
            {"globalId": 7, "status": "ok", "vehicleSpeedMmps": 9000},
        ]
        summary = summarize_motion_result({"motion": {
            "schema": "spec191-motion-result-v1", "estimates": estimates,
        }})
        rows = summary["estimates"]
        self.assertEqual(
            [(row.get("cameraId"), row["globalId"], row["vehicleSpeedMmps"])
             for row in rows],
            [("UAV1", 7, 5000), ("UAV2", 7, 12000), (None, 7, 9000)])
        self.assertNotIn("cameraId", rows[2])

    def manifest(self, payload: bytes = b"tracks") -> dict[str, object]:
        return {
            "schema": "spec191-uav-result/v1",
            "runId": "run-1",
            "sessionId": "session-1",
            "providerIdentity": "/example/uav/compute",
            "terminal": True,
            "inputs": [{"name": "/uav/UAV1/window/0", "digest": "sha256:input"}],
            "products": [{"name": "/compute/result/0", "digest": digest(payload)}],
        }

    def test_collect_requires_verified_provider_owned_bytes(self) -> None:
        manifest = self.manifest()
        products = collect_result(
            manifest, run_id="run-1", session_id="session-1",
            expected_provider="/example/uav/compute",
            fetch=lambda name: FetchedProduct(name, b"tracks", "/example/uav/compute", True))
        self.assertEqual(products["/compute/result/0"], b"tracks")

    def test_collect_rejects_unverified_or_tampered_product(self) -> None:
        manifest = self.manifest()
        with self.assertRaises(ResultError):
            collect_result(
                manifest, run_id="run-1", session_id="session-1",
                expected_provider="/example/uav/compute",
                fetch=lambda name: FetchedProduct(name, b"tracks", "/example/uav/compute", False))
        with self.assertRaises(ResultError):
            collect_result(
                manifest, run_id="run-1", session_id="session-1",
                expected_provider="/example/uav/compute",
                fetch=lambda name: FetchedProduct(name, b"tampered", "/example/uav/compute", True))

    def test_spec210_report_preserves_camera_track_and_unknown_values(self) -> None:
        measurement = {
            "schema": "uav-motion-result-v2",
            "trackKey": {"sourceId": "src", "cameraId": "UAV2", "sourceEpoch": 5,
                         "localTrackId": 3, "trackGeneration": 3},
            "frameKey": {"sourceId": "src", "cameraId": "UAV2", "sourceEpoch": 5,
                         "sequence": 11},
            "observationRef": {"name": "/src/observation/11", "digest": "sha256:frame"},
            "speedMps": None, "courseDeg": None, "quality": "warming-up",
            "reasonCodes": ["TRACK_WARMUP"],
        }
        summary = summarize_motion_result({"motion": {
            "schema": "uav-motion-result-v2", "sourceEpoch": 5,
            "inputProvenance": "measured", "accuracyQualification": "unvalidated",
            "measurements": [measurement],
        }})
        self.assertEqual(summary["status"], "warming-up")
        self.assertEqual(summary["inputProvenance"], "measured")
        self.assertEqual(summary["measurements"][0]["trackKey"], measurement["trackKey"])
        self.assertIsNone(summary["measurements"][0]["speedMps"])


if __name__ == "__main__":
    unittest.main()
