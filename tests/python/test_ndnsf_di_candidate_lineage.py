from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/fixtures/ndnsf-di-core-app-separation/historical-evidence-baseline.json"
SPEC = importlib.util.spec_from_file_location(
    "baseline_inventory", REPO / "tools/spec111/generate_phase1_inventories.py")
inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inventory)


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def validate_candidate_evidence(value: dict) -> None:
    source_spec = str(value.get("sourceSpec", ""))
    evidence_path = str(value.get("evidencePath", ""))
    promoted_from = str(value.get("promotedFrom", ""))
    if source_spec == "111" and evidence_path.startswith("specs/110-"):
        raise ValueError("SPEC111_MAY_NOT_WRITE_SPEC110_EVIDENCE")
    if source_spec == "111" and ("spec109" in promoted_from or "spec110" in promoted_from):
        raise ValueError("HISTORICAL_EVIDENCE_PROMOTION_FORBIDDEN")


class CandidateLineageTest(unittest.TestCase):
    def test_explicit_archive_verifier_checks_frozen_files_and_metadata(self) -> None:
        # Synthetic archive: this is not qualification of the original 100 files.
        with tempfile.TemporaryDirectory(prefix="ndnsf-baseline-test-") as directory:
            root = Path(directory)
            archived = root / 'archive.json'
            archived.write_text('{"frozen":true}\n', encoding='utf-8')
            baseline = root / 'baseline.json'
            baseline.write_text(json.dumps({
                'sourceCommit': 'a' * 40,
                'files': [{'path': 'archive.json', 'sha256': sha256(archived)}],
            }), encoding='utf-8')
            reference = root / 'reference.json'
            reference.write_text(json.dumps({
                'sourceCommit': 'a' * 40, 'frozenFileCount': 1,
                'baselineSha256': sha256(baseline),
            }), encoding='utf-8')
            self.assertEqual(inventory.verify_historical_baseline(
                root, baseline, reference)['verifiedFileCount'], 1)
            archived.write_text('changed', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'file-digest-mismatch'):
                inventory.verify_historical_baseline(root, baseline, reference)
            archived.unlink()
            with self.assertRaisesRegex(ValueError, 'file-missing-or-duplicate'):
                inventory.verify_historical_baseline(root, baseline, reference)
            baseline.write_text('{}', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'baseline-digest-mismatch'):
                inventory.verify_historical_baseline(root, baseline, reference)

    def test_original_archive_reference_retains_its_provenance(self) -> None:
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(fixture['baselinePath'],
            'specs/111-ndnsf-di-core-app-separation/evidence/historical-evidence-baseline.json')
        self.assertEqual(fixture['sourceCommit'], '4d695ce8b7ffe2c79465dc1f3db649a5a65806a6')
        self.assertEqual(fixture['frozenFileCount'], 100)
        self.assertRegex(fixture['baselineSha256'], r'^sha256:[0-9a-f]{64}$')

    def test_spec111_cannot_promote_into_spec110(self) -> None:
        with self.assertRaisesRegex(ValueError, "SPEC111_MAY_NOT_WRITE_SPEC110_EVIDENCE"):
            validate_candidate_evidence(
                {"sourceSpec": "111", "evidencePath": "specs/110-itiger/evidence/result.json"}
            )
        with self.assertRaisesRegex(ValueError, "HISTORICAL_EVIDENCE_PROMOTION_FORBIDDEN"):
            validate_candidate_evidence(
                {"sourceSpec": "111", "evidencePath": "specs/111/evidence/new.json", "promotedFrom": "spec110-job"}
            )

    def test_new_spec111_evidence_is_allowed(self) -> None:
        validate_candidate_evidence(
            {"sourceSpec": "111", "evidencePath": "specs/111-ndnsf-di-core-app-separation/evidence/new.json"}
        )


if __name__ == "__main__":
    unittest.main()
