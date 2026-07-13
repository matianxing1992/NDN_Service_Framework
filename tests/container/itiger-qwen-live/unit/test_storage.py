from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from _support import FIXTURES, load_tool


storage = load_tool("spec110_storage")
CASES = json.loads((FIXTURES / "storage" / "cases.json").read_text())


class StorageTests(unittest.TestCase):
    def test_admission_uses_quota_and_reserve(self):
        passed = storage.evaluate_admission(CASES["admissionPass"])
        blocked = storage.evaluate_admission(CASES["quotaFull"])
        self.assertEqual(passed["status"], "PASS")
        self.assertEqual(blocked["reasonCode"], "QUOTA_RESERVE_INSUFFICIENT")

    def test_home_bulk_is_rejected(self):
        with self.assertRaisesRegex(storage.StorageError, "STORAGE_HOME_BULK_FORBIDDEN"):
            storage.evaluate_admission(CASES["homeBulkInvalid"])

    def test_partial_copy_never_promotes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage = root / "stage"
            stage.mkdir()
            (stage / "stdout.log").write_bytes(b"not-the-registered-content")
            with self.assertRaisesRegex(storage.StorageError, "PROMOTION_PARTIAL_COPY"):
                storage.atomic_promote(
                    stage, root / "project" / "evidence" / "run",
                    CASES["partialCopy"]["manifest"], allow_test_root=True,
                )
            self.assertTrue(stage.exists())

    def test_checksum_verified_atomic_promotion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage = root / "stage"
            stage.mkdir()
            payloads = {"stdout.log": b"stdout", "evidence.json": b"{}"}
            for name, payload in payloads.items():
                (stage / name).write_bytes(payload)
            manifest = {
                name: "sha256:" + hashlib.sha256(payload).hexdigest()
                for name, payload in payloads.items()
            }
            target = root / "project" / "evidence" / "run"
            result = storage.atomic_promote(stage, target, manifest, allow_test_root=True)
            self.assertTrue(result["complete"])
            self.assertFalse(stage.exists())
            self.assertTrue(target.is_dir())

    def test_cleanup_defaults_dry_run_and_protects_every_class(self):
        protected = CASES["protectedCleanup"]
        candidates = [item for values in protected.values() for item in values]
        candidates += ["models/.partial/orphan", "evidence/debug-orphan"]
        result = storage.plan_cleanup(candidates, protected)
        self.assertTrue(result["dryRun"])
        self.assertFalse(result["executed"])
        self.assertEqual(set(result["deleteCandidates"]), {"models/.partial/orphan", "evidence/debug-orphan"})
        self.assertEqual(set(result["protected"]), set(candidates) - set(result["deleteCandidates"]))


if __name__ == "__main__":
    unittest.main()
