from __future__ import annotations

import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.tracking_report import FetchedProduct, ResultError, collect_result, digest


class TrackingReportTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
