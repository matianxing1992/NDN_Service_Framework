#!/usr/bin/env python3
"""Spec 107 frozen-lineage contract tests."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools" / "ndnsf-di"
sys.path.insert(0, str(TOOLS))

from spec107_lineage import (  # noqa: E402
    LineageError,
    assert_mutation_allowed,
    load_lineage_lock,
    verify_lineage_lock,
)


class Spec107LineageTest(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[Path, dict[str, object]]:
        rows = []
        entries = (
            ("task-closure", "specs/105-ndnsf-di-deployment-readiness/tasks.md"),
            ("release-decision", "specs/105-ndnsf-di-deployment-readiness/release-gate.json"),
            ("performance-negative-evidence", "specs/105-ndnsf-di-deployment-readiness/evidence/performance.md"),
            ("recovery-negative-evidence", "specs/105-ndnsf-di-deployment-readiness/evidence/recovery.md"),
        )
        for classification, relative in entries:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(classification + "\n", encoding="utf-8")
            rows.append({
                "classification": classification,
                "path": relative,
                "sha256": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
            })
        payload: dict[str, object] = {
            "schema": "ndnsf-di-lineage-lock-v1",
            "predecessorSpec": "specs/105-ndnsf-di-deployment-readiness",
            "frozenCommit": "a" * 40,
            "files": rows,
            "predecessorReleaseId": "spec105-local-minindn-candidate-r2",
            "predecessorMiniNdnVerdict": "BLOCK",
            "predecessorPhysicalVerdict": "DEFERRED",
        }
        lock = root / "lineage-lock.json"
        lock.write_text(json.dumps(payload), encoding="utf-8")
        return lock, payload

    def test_verifies_all_four_files_and_frozen_commit_identifier(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, _ = self._fixture(root)
            result = verify_lineage_lock(lock, repo_root=root, verify_commit=False)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["verifiedFileCount"], 4)
            self.assertEqual(result["verifiedIdentifierCount"], 5)
            self.assertEqual(result["frozenCommit"], "a" * 40)
            self.assertEqual(len(result["files"]), 4)

    def test_rejects_tampering_without_writing_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, payload = self._fixture(root)
            target = root / payload["files"][0]["path"]  # type: ignore[index]
            target.write_text("tampered\n", encoding="utf-8")
            sentinel = root / "must-not-exist.json"
            with self.assertRaisesRegex(LineageError, "LINEAGE_DIGEST_MISMATCH"):
                verify_lineage_lock(lock, repo_root=root, verify_commit=False)
            self.assertFalse(sentinel.exists())

    def test_rejects_missing_duplicate_unknown_and_escaping_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, payload = self._fixture(root)
            variants = []

            missing = copy.deepcopy(payload)
            missing["files"] = missing["files"][:-1]  # type: ignore[index]
            variants.append((missing, "LINEAGE_CLASSIFICATIONS_INVALID"))

            duplicate = copy.deepcopy(payload)
            duplicate["files"][1]["path"] = duplicate["files"][0]["path"]  # type: ignore[index]
            variants.append((duplicate, "LINEAGE_PATH_DUPLICATE"))

            unknown = copy.deepcopy(payload)
            unknown["files"][0]["classification"] = "unknown"  # type: ignore[index]
            variants.append((unknown, "LINEAGE_CLASSIFICATIONS_INVALID"))

            escaping = copy.deepcopy(payload)
            escaping["files"][0]["path"] = "../outside"  # type: ignore[index]
            variants.append((escaping, "LINEAGE_PATH_INVALID"))

            absolute = copy.deepcopy(payload)
            absolute["files"][0]["path"] = "/tmp/outside"  # type: ignore[index]
            variants.append((absolute, "LINEAGE_PATH_INVALID"))

            for index, (variant, reason) in enumerate(variants):
                with self.subTest(reason=reason):
                    candidate = root / f"invalid-{index}.json"
                    candidate.write_text(json.dumps(variant), encoding="utf-8")
                    with self.assertRaisesRegex(LineageError, reason):
                        load_lineage_lock(candidate, repo_root=root)

    def test_rejects_wrong_predecessor_verdict_or_commit_shape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _, payload = self._fixture(root)
            cases = (
                ("predecessorMiniNdnVerdict", "PASS", "LINEAGE_MININDN_VERDICT_INVALID"),
                ("predecessorPhysicalVerdict", "PASS", "LINEAGE_PHYSICAL_VERDICT_INVALID"),
                ("frozenCommit", "short", "LINEAGE_FROZEN_COMMIT_INVALID"),
            )
            for index, (field, value, reason) in enumerate(cases):
                with self.subTest(field=field):
                    candidate_payload = copy.deepcopy(payload)
                    candidate_payload[field] = value
                    candidate = root / f"bad-{index}.json"
                    candidate.write_text(json.dumps(candidate_payload), encoding="utf-8")
                    with self.assertRaisesRegex(LineageError, reason):
                        load_lineage_lock(candidate, repo_root=root)

    def test_denies_spec105_document_and_result_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            denied = (
                root / "specs/105-ndnsf-di-deployment-readiness/tasks.md",
                root / "specs/105-ndnsf-di-deployment-readiness-new/file.md",
                root / "results/spec105-qwen-pilot-run1/summary.json",
                root / "results/spec105-anything",
            )
            for path in denied:
                with self.subTest(path=path):
                    with self.assertRaisesRegex(LineageError, "SPEC105_MUTATION_DENIED"):
                        assert_mutation_allowed(path, repo_root=root)

            allowed = root / "results/spec107-c1-diagnostic/summary.json"
            self.assertEqual(assert_mutation_allowed(allowed, repo_root=root), allowed.resolve())

    def test_symlink_cannot_escape_mutation_guard(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            frozen = root / "specs/105-ndnsf-di-deployment-readiness"
            frozen.mkdir(parents=True)
            alias = root / "alias"
            alias.symlink_to(frozen, target_is_directory=True)
            with self.assertRaisesRegex(LineageError, "SPEC105_MUTATION_DENIED"):
                assert_mutation_allowed(alias / "tasks.md", repo_root=root)


if __name__ == "__main__":
    unittest.main()
