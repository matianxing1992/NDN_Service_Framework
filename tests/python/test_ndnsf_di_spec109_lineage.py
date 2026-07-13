#!/usr/bin/env python3
"""Spec 109 predecessor and authority-boundary tests."""

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

from spec109_predecessors import (  # noqa: E402
    PredecessorError,
    REQUIRED_TASK_IDS,
    assert_spec109_mutation_allowed,
    observe_predecessor_lock,
    validate_authority,
    verify_predecessor_gate,
)


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


class Spec109LineageTest(unittest.TestCase):
    def _valid_gate(self, root: Path) -> tuple[Path, dict[str, object]]:
        entries = {}
        for task_id in REQUIRED_TASK_IDS:
            spec, task = task_id.split(":")
            artifact = root / "predecessors" / spec / f"{task}.json"
            artifact.parent.mkdir(parents=True, exist_ok=True)
            artifact.write_text(json.dumps({"taskId": task_id}) + "\n", encoding="utf-8")
            entries[task_id] = {
                "requiredStatus": "PASS",
                "observedStatus": "PASS",
                "schemaVersion": "test-v1",
                "artifactPath": artifact.relative_to(root).as_posix(),
                "artifactDigest": digest(artifact),
                "identityDigest": "sha256:" + hashlib.sha256(task_id.encode()).hexdigest(),
            }
        payload: dict[str, object] = {
            "schemaVersion": "1.0",
            "requiredTaskIds": list(REQUIRED_TASK_IDS),
            "entries": entries,
        }
        canonical = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
        payload["gateDigest"] = "sha256:" + hashlib.sha256(canonical).hexdigest()
        path = root / "gate.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path, payload

    def test_exact_predecessor_gate_verifies_every_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path, _ = self._valid_gate(root)
            report = verify_predecessor_gate(path, repo_root=root)
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["verifiedTaskCount"], len(REQUIRED_TASK_IDS))

    def test_gate_rejects_missing_incomplete_tampered_and_digest_changes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _, payload = self._valid_gate(root)
            variants = []

            missing = copy.deepcopy(payload)
            del missing["entries"][REQUIRED_TASK_IDS[0]]  # type: ignore[index]
            variants.append((missing, "PREDECESSOR_TASK_SET_INVALID"))

            incomplete = copy.deepcopy(payload)
            incomplete["entries"][REQUIRED_TASK_IDS[0]]["observedStatus"] = "INCOMPLETE"  # type: ignore[index]
            variants.append((incomplete, "PREDECESSOR_STATUS_NOT_PASS"))

            bad_identity = copy.deepcopy(payload)
            bad_identity["entries"][REQUIRED_TASK_IDS[0]]["identityDigest"] = None  # type: ignore[index]
            variants.append((bad_identity, "PREDECESSOR_DIGEST_INVALID"))

            for index, (variant, reason) in enumerate(variants):
                if "gateDigest" in variant:
                    del variant["gateDigest"]
                variant["gateDigest"] = "sha256:" + hashlib.sha256(
                    json.dumps(variant, sort_keys=True, separators=(",", ":")).encode()
                ).hexdigest()
                candidate = root / f"bad-{index}.json"
                candidate.write_text(json.dumps(variant), encoding="utf-8")
                with self.subTest(reason=reason), self.assertRaisesRegex(PredecessorError, reason):
                    verify_predecessor_gate(candidate, repo_root=root)

            valid, payload = self._valid_gate(root)
            first = root / payload["entries"][REQUIRED_TASK_IDS[0]]["artifactPath"]  # type: ignore[index]
            first.write_text("tampered\n", encoding="utf-8")
            with self.assertRaisesRegex(PredecessorError, "PREDECESSOR_ARTIFACT_DIGEST_MISMATCH"):
                verify_predecessor_gate(valid, repo_root=root)

    def test_spec109_cannot_rewrite_predecessors_or_spec106_authority(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            denied = (
                "specs/106-ndnsf-di-physical-pilot/release-gate.json",
                "specs/107-ndnsf-di-minindn-gate-recovery/tasks.md",
                "specs/108-ndnsf-di-container-deployment/tasks.md",
                "results/spec107-candidate/manifest.json",
                "results/spec108-container/gpu/evidence.json",
            )
            for relative in denied:
                with self.subTest(relative=relative), self.assertRaisesRegex(
                        PredecessorError, "SPEC109_MUTATION_DENIED"):
                    assert_spec109_mutation_allowed(root / relative, repo_root=root)

            allowed = root / "results/spec109-itiger-qwen/discovery/report.json"
            self.assertEqual(assert_spec109_mutation_allowed(allowed, repo_root=root), allowed.resolve())

    def test_physical_production_is_forced_deferred(self) -> None:
        self.assertEqual(validate_authority({
            "substrate": "PASS", "candidate": "FAIL", "physicalProduction": "DEFERRED"
        })["physicalProduction"], "DEFERRED")
        with self.assertRaisesRegex(PredecessorError, "PHYSICAL_PRODUCTION_AUTHORITY_INVALID"):
            validate_authority({
                "substrate": "PASS", "candidate": "PASS", "physicalProduction": "PASS"
            })

    def test_incomplete_lock_produces_terminal_block_without_submission(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _, payload = self._valid_gate(root)
            payload["schemaVersion"] = "1.0-lock"
            payload["entries"][REQUIRED_TASK_IDS[0]]["observedStatus"] = "INCOMPLETE"  # type: ignore[index]
            payload["lockDigest"] = "sha256:" + "a" * 64
            payload["sourceSnapshotDigest"] = "sha256:" + "b" * 64
            path = root / "lock.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            report = observe_predecessor_lock(path)
            self.assertEqual(report["status"], "BLOCKED")
            self.assertFalse(report["jobSubmitted"])
            self.assertEqual(report["blockingTaskIds"], [REQUIRED_TASK_IDS[0]])


if __name__ == "__main__":
    unittest.main()
