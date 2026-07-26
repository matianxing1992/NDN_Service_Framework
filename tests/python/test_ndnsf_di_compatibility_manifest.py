from __future__ import annotations

import json
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
MANIFEST = (
    REPO
    / "NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json"
)


class CompatibilityManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.value = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def test_manifest_has_unique_complete_entries(self) -> None:
        self.assertEqual(self.value["schemaVersion"], 1)
        entries = self.value["entries"]
        self.assertGreater(len(entries), 0)
        identities = [entry["surfaceId"] for entry in entries]
        self.assertEqual(len(identities), len(set(identities)))
        required = {
            "surfaceId",
            "surfaceKind",
            "currentOwner",
            "callers",
            "rollbackRelease",
            "proposedCanonicalOwner",
            "status",
        }
        for entry in entries:
            self.assertTrue(required.issubset(entry), entry)
            self.assertIsInstance(entry["callers"], list, entry)
            for field in required - {"callers"}:
                self.assertIsInstance(entry[field], str, entry)
                self.assertTrue(entry[field], entry)

    def test_every_initial_surface_kind_is_represented(self) -> None:
        kinds = {entry["surfaceKind"] for entry in self.value["entries"]}
        self.assertTrue(
            {"python-export", "console-script", "cpp-header", "cpp-target", "deployment-caller"}.issubset(kinds)
        )


if __name__ == "__main__":
    unittest.main()
