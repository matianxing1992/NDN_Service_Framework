from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ndnsf-di"))
from spec170_evidence import (  # noqa: E402
    CandidateIdentity,
    EvidenceBundle,
    EvidenceRow,
    RunIdentity,
    invalid_candidate_identity,
)


class Spec170EvidenceBundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = ROOT / "tests" / "fixtures" / "spec170" / "candidate.json"
        cls.candidate = CandidateIdentity.from_dict(
            json.loads(fixture.read_text(encoding="utf-8")))

    def make_bundle(self) -> EvidenceBundle:
        run = RunIdentity(
            candidate_id=self.candidate.candidate_id,
            gate="A", run_id="run-001", started_at="2026-08-04T20:00:00Z",
            topology={"providers": 3, "repo": "repo0"},
        )
        bundle = EvidenceBundle.create(self.candidate, run)
        bundle.add_complete(EvidenceRow("row-1", "COMPLETE", {"tokens": 3}))
        bundle.add_negative(EvidenceRow(
            "row-neg-1", "NEGATIVE", {"error": "stale-offer"},
            reason="negative control"))
        return bundle

    def test_round_trip_is_deterministic(self):
        bundle = self.make_bundle()
        digest = bundle.freeze()
        encoded = bundle.to_json()
        restored = EvidenceBundle.from_dict(json.loads(encoded))
        self.assertEqual(restored.digest, digest)
        self.assertEqual(restored.to_json(), encoded)
        restored.assert_integrity()

    def test_post_freeze_mutation_is_rejected(self):
        bundle = self.make_bundle()
        bundle.freeze()
        with self.assertRaises(RuntimeError):
            bundle.add_complete(EvidenceRow("row-2", "COMPLETE"))
        # Detect direct mutation too: the digest is an evidence gate, not a
        # convention that callers may bypass by modifying the list in place.
        bundle.complete_rows.append(EvidenceRow("row-evil", "COMPLETE"))
        with self.assertRaisesRegex(ValueError, "INVALID_CANDIDATE"):
            bundle.assert_integrity()

    def test_invalid_candidate_identity_is_stable(self):
        first = invalid_candidate_identity("sha256:a", "sha256:b")
        second = invalid_candidate_identity("sha256:a", "sha256:b")
        self.assertEqual(first, second)
        self.assertTrue(first.startswith("INVALID_CANDIDATE:"))


if __name__ == "__main__":
    unittest.main()
