from __future__ import annotations

import copy
import unittest

from _support import load_tool


candidate = load_tool("spec110_candidate")


def digest(character: str) -> str:
    return "sha256:" + character * 64


def candidate_bindings() -> dict[str, str]:
    return {
        "sourceDigest": digest("1"),
        "runtimeReleaseDigest": digest("2"),
        "modelArtifactSetDigest": digest("3"),
        "identitySetDigest": digest("4"),
        "topologyPlacementDigest": digest("5"),
        "workloadDigest": digest("6"),
    }


def campaign_bindings() -> dict[str, str]:
    return {
        "sourceBaselineDigest": digest("a"),
        "modelLadderDigest": digest("b"),
        "workloadDigest": digest("c"),
        "identityContractDigest": digest("d"),
        "clusterContractDigest": digest("e"),
        "evidenceContractDigest": digest("f"),
    }


class CandidateIdentityTests(unittest.TestCase):
    def test_candidate_is_stable_and_uses_six_binding_fragments(self):
        first = candidate.freeze_candidate({"bindingDigests": candidate_bindings()})
        second = candidate.freeze_candidate({"bindingDigests": dict(reversed(list(candidate_bindings().items())))})
        self.assertEqual(first, second)
        self.assertRegex(first["candidateId"], r"^spec110-c1(?:-[0-9a-f]{12}){6}$")
        self.assertEqual(first["state"], "FROZEN")

    def test_every_changed_binding_changes_candidate_identity(self):
        baseline = candidate.freeze_candidate({"bindingDigests": candidate_bindings()})
        for key in candidate.CANDIDATE_BINDING_FIELDS:
            changed = candidate_bindings()
            changed[key] = digest("9")
            self.assertNotEqual(
                baseline["candidateId"],
                candidate.freeze_candidate({"bindingDigests": changed})["candidateId"],
                key,
            )

    def test_short_prefix_collision_is_rejected_by_full_digest(self):
        first = candidate.freeze_candidate({"bindingDigests": candidate_bindings()})
        colliding = candidate_bindings()
        colliding["workloadDigest"] = "sha256:" + "6" * 12 + "7" * 52
        second = candidate.freeze_candidate({"bindingDigests": colliding})
        self.assertEqual(first["candidateId"], second["candidateId"])
        self.assertNotEqual(first["candidateDigest"], second["candidateDigest"])
        with self.assertRaisesRegex(candidate.IdentityError, "IDENTITY_PREFIX_COLLISION"):
            candidate.assert_no_identity_collision([first], second)

    def test_spec109_identity_is_never_accepted(self):
        for legacy in (
            "spec109-candidate-a", "spec109-campaign-a", "spec109-cell-a",
            "spec109-run-a", "spec109-submission-a",
        ):
            with self.assertRaisesRegex(candidate.IdentityError, "LEGACY_IDENTITY_FORBIDDEN"):
                candidate.reject_legacy_identity(legacy)
        with self.assertRaisesRegex(candidate.IdentityError, "LEGACY_IDENTITY_FORBIDDEN"):
            candidate.derive_cell_identity("spec109-candidate-a", "DISTRIBUTED_CANDIDATE", 1, 0, "single")

    def test_frozen_candidate_rejects_mutation(self):
        frozen = candidate.freeze_candidate({"bindingDigests": candidate_bindings()})
        candidate.validate_frozen_candidate(frozen)
        mutated = copy.deepcopy(frozen)
        mutated["bindingDigests"]["sourceDigest"] = digest("9")
        with self.assertRaisesRegex(candidate.IdentityError, "FROZEN_CANDIDATE_MUTATED"):
            candidate.validate_frozen_candidate(mutated)

    def test_campaign_cell_run_submission_and_replacement_links(self):
        campaign = candidate.freeze_campaign({"bindingDigests": campaign_bindings()})
        self.assertRegex(campaign["campaignId"], r"^spec110-campaign-v1-[0-9a-f]{20}$")
        self.assertEqual(campaign["protocol"], candidate.DEFAULT_CAMPAIGN_PROTOCOL)
        self.assertEqual(campaign["protocol"]["submissionPolicy"], "at-most-once-no-auto-resubmit")
        frozen = candidate.freeze_candidate({"bindingDigests": candidate_bindings()})
        cell = candidate.derive_cell_identity(
            frozen["candidateId"], "DISTRIBUTED_CANDIDATE", 32, 0,
            "single-node-multi-gpu",
        )
        original = candidate.derive_run_identity(cell, 1)
        submission = candidate.derive_submission_identity(original["runId"], digest("8"))
        self.assertRegex(cell, r"^spec110-cell-[0-9a-f]{20}$")
        self.assertRegex(original["runId"], r"^spec110-run-[0-9a-f]{20}$")
        self.assertRegex(submission, r"^spec110-submission-[0-9a-f]{20}$")
        with self.assertRaisesRegex(candidate.IdentityError, "REPLACEMENT_AUTHORIZATION_REQUIRED"):
            candidate.derive_run_identity(cell, 2, replaces_run_id=original["runId"])
        replacement = candidate.derive_run_identity(
            cell, 2, replaces_run_id=original["runId"],
            replacement_authorization_digest=digest("9"),
        )
        self.assertNotEqual(original["runId"], replacement["runId"])
        self.assertEqual(replacement["replacesRunId"], original["runId"])


if __name__ == "__main__":
    unittest.main()
