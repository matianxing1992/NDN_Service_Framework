from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/spec111/finalize_compatibility_exit.py"


def load_module():
    spec = importlib.util.spec_from_file_location("spec111_compat_exit", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load compatibility exit evaluator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture() -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    manifest = {
        "schemaVersion": 1,
        "entries": [{
            "surfaceId": "python-export:Legacy",
            "surfaceKind": "python-export",
            "currentOwner": "canonical.owner",
            "callers": [],
            "rollbackRelease": "pre-spec111",
            "proposedCanonicalOwner": "canonical.owner",
            "status": "compatibility-reexport",
        }],
    }
    snapshot = {
        "snapshotId": "snapshot-1",
        "productionCallerCount": 0,
        "verdict": "PASS",
    }
    performance = {"gate": {"pass": True}}
    return manifest, snapshot, performance


class CompatibilityExitTest(unittest.TestCase):
    def test_unknown_external_use_retains_adapter_despite_zero_callers(self):
        module = load_module()
        manifest, snapshot, performance = fixture()
        snapshot_two = dict(snapshot, snapshotId="snapshot-2")

        updated, gate = module.evaluate(
            manifest, snapshot, snapshot_two, performance)

        self.assertEqual(gate["verdict"], "RETAIN_ALL")
        self.assertEqual(gate["eligibleCount"], 0)
        entry = updated["entries"][0]
        self.assertEqual(entry["externalUseStatus"], "external_use_unknown")
        self.assertFalse(entry["removalEligible"])
        self.assertIn("external migration evidence", entry["remainingExitCondition"])

    def test_confirmed_external_migration_allows_bounded_removal(self):
        module = load_module()
        manifest, snapshot, performance = fixture()
        manifest["entries"][0]["externalMigrationEvidence"] = "ticket-111"
        snapshot_two = dict(snapshot, snapshotId="snapshot-2")

        updated, gate = module.evaluate(
            manifest, snapshot, snapshot_two, performance)

        self.assertEqual(gate["verdict"], "ALLOW_BOUNDED_REMOVAL")
        self.assertEqual(gate["eligibleCount"], 1)
        self.assertTrue(updated["entries"][0]["removalEligible"])
        self.assertEqual(updated["entries"][0]["remainingExitCondition"], "none")


if __name__ == "__main__":
    unittest.main()
