#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = (
    ROOT
    / "packaging/ndnsf-di-container/oci/layered/scripts/verify-app-reuse.py"
)


def manifest(app_id: str, *, parent_suffix: str = "") -> dict[str, object]:
    images = {
        product: {"imageId": f"sha256:{product}{parent_suffix}", "tag": product}
        for product in ("ml-devel", "ml-runtime", "ndn-devel", "ndn-runtime")
    }
    images["app-runtime"] = {"imageId": f"sha256:{app_id}", "tag": app_id}
    return {
        "schemaVersion": "spec158-layered-build-v1",
        "status": "PASS",
        "images": images,
        "executedProducts": ["app-runtime"],
        "contentScan": {"status": "PASS"},
        "staticProbe": {"status": "PASS"},
    }


class AppReuseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("verify_app_reuse", SCRIPT)
        cls.module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        sys.modules[spec.name] = cls.module
        spec.loader.exec_module(cls.module)

    def write_manifest(self, value: dict[str, object], name: str) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="spec158-reuse-test-"))
        path = directory / name
        path.write_text(json.dumps(value))
        return path

    def test_matching_foundations_pass(self):
        first = self.write_manifest(manifest("app-one"), "first.json")
        second = self.write_manifest(manifest("app-two"), "second.json")
        report = self.module.verify(first, second)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["secondExecutedProducts"], ["app-runtime"])

    def test_parent_drift_is_rejected(self):
        first = self.write_manifest(manifest("app-one"), "first.json")
        second = self.write_manifest(
            manifest("app-two", parent_suffix="-changed"), "second.json"
        )
        with self.assertRaisesRegex(ValueError, "FOUNDATION_IMAGE_DRIFT"):
            self.module.verify(first, second)


if __name__ == "__main__":
    unittest.main()
