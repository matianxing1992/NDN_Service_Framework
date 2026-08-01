import unittest

from Experiments.ndnsf_validation.fidelity import (
    FidelityError,
    FidelityTier,
    GatePolicy,
    aggregate_records,
    validate_fidelity_record,
)


def record(**updates):
    value = {
        "schemaVersion": 1,
        "caseId": "gate-b-minindn",
        "gateId": "B",
        "runId": "run-1",
        "startedAt": "2026-07-30T00:00:00Z",
        "completedAt": "2026-07-30T00:01:00Z",
        "status": "PASS",
        "failureReason": "",
        "exactCommand": "python3 gate.py",
        "sourceRevision": "source-1",
        "fidelityTier": "REAL_MININDN_MODEL",
        "realComponents": ["MiniNDN", "Qwen3"],
        "simulatedComponents": [],
        "networkMode": "minindn",
        "containerMode": "host",
        "modelIdentity": {
            "name": "Qwen/Qwen3-0.6B",
            "revision": "immutable",
            "contentDigest": "sha256:model",
        },
        "workloadDigest": "sha256:workload",
        "hardwareProfile": {"backend": "cpu"},
        "skipIsFailure": True,
        "evidencePaths": ["result.json"],
    }
    value.update(updates)
    return value


class FidelityContractTests(unittest.TestCase):
    def setUp(self):
        self.policy = GatePolicy(
            schema_version=1,
            run_id="run-1",
            source_revision="source-1",
            mandatory_cases={"gate-b-minindn": FidelityTier.REAL_MININDN_MODEL},
            model_identity_digest="sha256:model",
            workload_digest="sha256:workload",
        )

    def test_valid_record_passes(self):
        self.assertTrue(aggregate_records([record()], self.policy)["passed"])

    def test_missing_and_contradictory_fields_fail(self):
        missing = record()
        del missing["sourceRevision"]
        with self.assertRaises(FidelityError):
            validate_fidelity_record(missing)
        with self.assertRaises(FidelityError):
            validate_fidelity_record(
                record(simulatedComponents=["MiniNDN"])
            )

    def test_skip_fake_stale_and_cross_run_never_substitute(self):
        cases = (
            record(status="SKIP", failureReason="unavailable"),
            record(fidelityTier="FIXTURE"),
            record(sourceRevision="old-source"),
            record(runId="old-run"),
            record(workloadDigest="sha256:other"),
            record(
                modelIdentity={
                    "name": "Qwen/Qwen3-0.6B",
                    "revision": "immutable",
                    "contentDigest": "sha256:other",
                }
            ),
        )
        for candidate in cases:
            with self.subTest(candidate=candidate):
                self.assertFalse(
                    aggregate_records([candidate], self.policy)["passed"]
                )

    def test_duplicate_case_is_integrity_failure(self):
        verdict = aggregate_records([record(), record()], self.policy)
        self.assertFalse(verdict["passed"])
        self.assertIn("duplicate caseId", verdict["errors"][0])


if __name__ == "__main__":
    unittest.main()
