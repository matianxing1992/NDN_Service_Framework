from __future__ import annotations

import hashlib
import json
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
FEATURE = REPO / "specs/111-ndnsf-di-core-app-separation"
BASELINE = FEATURE / "evidence/historical-evidence-baseline.json"
FIXTURE = REPO / "tests/fixtures/ndnsf-di-core-app-separation/historical-evidence-baseline.json"


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
    def test_frozen_historical_files_match(self) -> None:
        baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
        for item in baseline["files"]:
            path = REPO / item["path"]
            self.assertTrue(path.is_file(), item["path"])
            self.assertEqual(sha256(path), item["sha256"], item["path"])

    def test_fixture_references_exact_baseline(self) -> None:
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(fixture["baselinePath"], BASELINE.relative_to(REPO).as_posix())
        self.assertEqual(fixture["baselineSha256"], sha256(BASELINE))

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
