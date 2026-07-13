#!/usr/bin/env python3
"""Spec 107 candidate and campaign identity contract tests."""

from __future__ import annotations

import hashlib
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from spec107_identity import (  # noqa: E402
    IdentityError,
    build_candidate_identity,
    build_campaign_identity,
    committed_source_digest,
    digest_object,
    validate_campaign_set,
    validate_candidate_identity,
)


def digest(character: str) -> str:
    return "sha256:" + character * 64


def candidate_inputs() -> dict[str, str]:
    return {
        "source": digest("1"),
        "profile": digest("2"),
        "model": digest("3"),
        "plan": digest("4"),
        "artifact": digest("5"),
        "lineage": digest("6"),
        "workload": digest("7"),
        "tokenizer": digest("8"),
        "trustPolicy": digest("9"),
        "command": digest("a"),
    }


class Spec107IdentityTest(unittest.TestCase):
    def test_builds_canonical_six_digest_candidate_id(self) -> None:
        inputs = candidate_inputs()
        value = build_candidate_identity(
            inputs,
            created_at="2026-07-12T00:00:00Z",
            generator_version="spec107-tools-v1",
        )
        self.assertEqual(
            value["candidateId"],
            "spec107-c1-111111111111-222222222222-333333333333-"
            "444444444444-555555555555-666666666666",
        )
        self.assertEqual(value["namespace"], "spec107-c1")
        self.assertEqual(value["state"], "FROZEN")
        self.assertEqual(value["digests"], inputs)
        self.assertEqual(value["createdAt"], "2026-07-12T00:00:00Z")

    def test_candidate_requires_all_full_lowercase_sha256_digests(self) -> None:
        inputs = candidate_inputs()
        for key, invalid in (
            ("source", "sha256:short"),
            ("profile", "SHA256:" + "2" * 64),
            ("model", "sha256:" + "G" * 64),
        ):
            with self.subTest(key=key):
                values = dict(inputs)
                values[key] = invalid
                with self.assertRaisesRegex(IdentityError, "CANDIDATE_DIGEST_INVALID"):
                    build_candidate_identity(values)
        values = dict(inputs)
        del values["lineage"]
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_DIGEST_MISSING"):
            build_candidate_identity(values)

    def test_candidate_rejects_unknown_fields_and_spec105_values(self) -> None:
        unknown = candidate_inputs()
        unknown["extra"] = digest("b")
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_DIGEST_UNKNOWN"):
            build_candidate_identity(unknown)

        with self.assertRaisesRegex(IdentityError, "SPEC105_IDENTITY_REJECTED"):
            build_candidate_identity(
                candidate_inputs(), namespace="spec105-local-minindn-candidate-r2")

    def test_candidate_validation_requires_exact_frozen_metadata_schema(self) -> None:
        candidate = build_candidate_identity(
            candidate_inputs(), created_at="2026-07-12T00:00:00Z")
        unknown = dict(candidate)
        unknown["extra"] = "not-authority"
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_FIELD_UNKNOWN"):
            validate_candidate_identity(unknown)

        draft = dict(candidate)
        draft["state"] = "DRAFT"
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_STATE_INVALID"):
            validate_candidate_identity(draft)

        bad_timestamp = dict(candidate)
        bad_timestamp["createdAt"] = "2026-07-12"
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_TIMESTAMP_INVALID"):
            validate_candidate_identity(bad_timestamp)

        bad_generator = dict(candidate)
        bad_generator["generatorVersion"] = ""
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_GENERATOR_INVALID"):
            validate_candidate_identity(bad_generator)

    def test_campaign_kinds_have_disjoint_ids_roots_and_eligibility(self) -> None:
        candidate = build_candidate_identity(candidate_inputs())
        kinds = (
            "diagnostic", "correctness", "performance", "fault",
            "canary", "operations", "soak", "release-gate",
        )
        campaigns = [
            build_campaign_identity(
                candidate,
                kind=kind,
                ordinal=1,
                command_digest=digest(format(index + 1, "x")[-1]),
                output_root=f"results/spec107-c1-{kind}-r1",
            )
            for index, kind in enumerate(kinds)
        ]
        candidate_digest = digest_object(candidate)
        validate_campaign_set(
            campaigns, candidate_id=candidate["candidateId"],
            candidate_digest=candidate_digest)
        self.assertTrue(all(
            row["candidateDigest"] == candidate_digest for row in campaigns))
        self.assertEqual(len({row["campaignId"] for row in campaigns}), len(kinds))
        self.assertEqual(len({row["outputRoot"] for row in campaigns}), len(kinds))
        diagnostic = campaigns[0]
        self.assertEqual(diagnostic["eligibility"], "DIAGNOSTIC_INELIGIBLE")
        self.assertFalse(diagnostic["releaseEligible"])
        for row in campaigns[1:]:
            self.assertEqual(row["eligibility"], "EVIDENCE_ELIGIBLE")

        tampered = json.loads(json.dumps(candidate))
        tampered["digests"]["workload"] = digest("b")
        self.assertEqual(tampered["candidateId"], candidate["candidateId"])
        with self.assertRaisesRegex(
                IdentityError, "CAMPAIGN_CANDIDATE_DIGEST_MISMATCH"):
            validate_campaign_set(
                campaigns, candidate_id=candidate["candidateId"],
                candidate_digest=digest_object(tampered))

    def test_campaign_rejects_spec105_paths_and_candidate_mismatch(self) -> None:
        candidate = build_candidate_identity(candidate_inputs())
        with self.assertRaisesRegex(IdentityError, "SPEC105_IDENTITY_REJECTED"):
            build_campaign_identity(
                candidate,
                kind="performance",
                ordinal=1,
                command_digest=digest("b"),
                output_root="results/spec105-qwen-pilot-r1",
            )
        altered = dict(candidate)
        altered["candidateId"] = "spec107-c1-invalid"
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_ID_MISMATCH"):
            build_campaign_identity(
                altered,
                kind="performance",
                ordinal=1,
                command_digest=digest("b"),
                output_root="results/spec107-c1-performance-r1",
            )

    def test_campaign_set_rejects_duplicate_id_or_output(self) -> None:
        candidate = build_candidate_identity(candidate_inputs())
        first = build_campaign_identity(
            candidate, kind="performance", ordinal=1,
            command_digest=digest("b"),
            output_root="results/spec107-c1-performance-r1")
        duplicate_id = dict(first)
        with self.assertRaisesRegex(IdentityError, "CAMPAIGN_ID_DUPLICATE"):
            validate_campaign_set(
                [first, duplicate_id], candidate_id=candidate["candidateId"],
                candidate_digest=digest_object(candidate))

        second = build_campaign_identity(
            candidate, kind="performance", ordinal=2,
            command_digest=digest("c"),
            output_root=first["outputRoot"])
        with self.assertRaisesRegex(IdentityError, "CAMPAIGN_OUTPUT_DUPLICATE"):
            validate_campaign_set(
                [first, second], candidate_id=candidate["candidateId"],
                candidate_digest=digest_object(candidate))

    def test_campaign_validation_recomputes_id_and_eligibility(self) -> None:
        candidate = build_candidate_identity(candidate_inputs())
        campaign = build_campaign_identity(
            candidate, kind="diagnostic", ordinal=1,
            command_digest=digest("b"),
            output_root="results/spec107-attribution-c1/warm-single")
        arguments = {
            "candidate_id": candidate["candidateId"],
            "candidate_digest": digest_object(candidate),
        }
        altered_ordinal = dict(campaign)
        altered_ordinal["ordinal"] = 2
        with self.assertRaisesRegex(IdentityError, "CAMPAIGN_ID_MISMATCH"):
            validate_campaign_set([altered_ordinal], **arguments)

        altered_eligibility = dict(campaign)
        altered_eligibility["eligibility"] = "EVIDENCE_ELIGIBLE"
        altered_eligibility["releaseEligible"] = True
        with self.assertRaisesRegex(
                IdentityError, "CAMPAIGN_ELIGIBILITY_INVALID"):
            validate_campaign_set([altered_eligibility], **arguments)

    def test_campaign_validation_requires_exact_top_level_schema(self) -> None:
        candidate = build_candidate_identity(candidate_inputs())
        campaign = build_campaign_identity(
            candidate, kind="diagnostic", ordinal=1,
            command_digest=digest("b"),
            output_root="results/spec107-attribution-c1/warm-single")
        arguments = {
            "candidate_id": candidate["candidateId"],
            "candidate_digest": digest_object(candidate),
        }

        unknown = dict(campaign)
        unknown["extra"] = "not-authority"
        with self.assertRaisesRegex(IdentityError, "CAMPAIGN_FIELD_UNKNOWN"):
            validate_campaign_set([unknown], **arguments)

        missing = dict(campaign)
        del missing["eligibility"]
        with self.assertRaisesRegex(IdentityError, "CAMPAIGN_FIELD_MISSING"):
            validate_campaign_set([missing], **arguments)

    def test_manifest_validation_rejects_non_object_values(self) -> None:
        with self.assertRaisesRegex(IdentityError, "CANDIDATE_OBJECT_INVALID"):
            validate_candidate_identity(7)  # type: ignore[arg-type]

        candidate = build_candidate_identity(candidate_inputs())
        with self.assertRaisesRegex(IdentityError, "CAMPAIGN_OBJECT_INVALID"):
            validate_campaign_set(
                [7],  # type: ignore[list-item]
                candidate_id=candidate["candidateId"],
                candidate_digest=digest_object(candidate),
            )

    def test_cli_exposes_lineage_artifact_candidate_campaign_and_fail_closed_gate(self) -> None:
        cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
        help_result = subprocess.run(
            [sys.executable, str(cli), "--help"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        for command in ("lineage", "artifact", "candidate", "campaign", "gate"):
            self.assertIn(command, help_result.stdout)

        lineage = subprocess.run([
            sys.executable, str(cli), "lineage", "verify", "--lock",
            str(REPO / "specs/107-ndnsf-di-minindn-gate-recovery/lineage-lock.json"),
        ], cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False)
        self.assertEqual(lineage.returncode, 0, lineage.stderr)
        self.assertEqual(json.loads(lineage.stdout)["verifiedIdentifierCount"], 5)

        gate = subprocess.run([
            sys.executable, str(cli), "gate", "generate",
            "--feature", "specs/107-ndnsf-di-minindn-gate-recovery",
            "--output", "specs/107-ndnsf-di-minindn-gate-recovery/release-gate.json",
        ], cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False)
        self.assertEqual(gate.returncode, 2)
        self.assertIn("SPEC107_RELEASE_INPUT_INVALID", gate.stderr)

    def test_cli_creates_exclusive_candidate_and_campaign_manifests(self) -> None:
        cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "spec107@test.invalid"],
                           cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Spec 107 Test"],
                           cwd=root, check=True)
            (root / "tracked.txt").write_text("frozen\n", encoding="utf-8")
            subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)
            values = candidate_inputs()
            values["source"] = committed_source_digest(root)
            digests = root / "digests.json"
            digests.write_text(json.dumps(values), encoding="utf-8")
            candidate_path = root / "results/spec107-candidate.json"
            create = subprocess.run([
                sys.executable, str(cli), "--repo-root", str(root),
                "candidate", "create", "--digests", str(digests),
                "--created-at", "2026-07-12T00:00:00Z",
                "--output", str(candidate_path),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(create.returncode, 0, create.stderr)
            self.assertTrue(candidate_path.is_file())

            campaign_path = root / "specs/107/evidence/performance.json"
            campaign = subprocess.run([
                sys.executable, str(cli), "--repo-root", str(root),
                "campaign", "preregister", "--kind", "performance",
                "--ordinal", "1", "--candidate", str(candidate_path),
                "--command-digest", digest("b"),
                "--campaign-output-root", "results/spec107-c1-performance-r1",
                "--output", str(campaign_path),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(campaign.returncode, 0, campaign.stderr)
            self.assertEqual(json.loads(campaign_path.read_text())["kind"], "performance")

            duplicate = subprocess.run([
                sys.executable, str(cli), "--repo-root", str(root),
                "campaign", "preregister", "--kind", "performance",
                "--candidate", str(candidate_path), "--command-digest", digest("b"),
                "--campaign-output-root", "results/spec107-c1-performance-r1",
                "--output", str(campaign_path),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(duplicate.returncode, 2)
            self.assertIn("OUTPUT_EXISTS", duplicate.stderr)

    def test_cli_materializes_candidate_inputs_from_reviewed_files(self) -> None:
        cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "spec107@test.invalid"],
                           cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Spec 107 Test"],
                           cwd=root, check=True)
            keys = (
                "profile", "model", "plan", "artifact", "lineage",
                "workload", "tokenizer", "trust-policy", "command",
            )
            inputs = {}
            for index, key in enumerate(keys):
                path = root / f"{key}.json"
                path.write_text(f'{{"index":{index}}}\n', encoding="utf-8")
                inputs[key] = path
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)

            output = root / "results/spec107-candidate-inputs.json"
            command = [
                sys.executable, str(cli), "--repo-root", str(root),
                "candidate", "inputs",
            ]
            for key in keys:
                command.extend([f"--{key}", str(inputs[key])])
            command.extend(["--output", str(output)])
            result = subprocess.run(
                command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                payload["schema"], "ndnsf-di-spec107-candidate-inputs-v1")
            self.assertEqual(payload["digests"]["source"], committed_source_digest(root))
            for cli_key, digest_key in (
                ("profile", "profile"), ("model", "model"),
                ("plan", "plan"), ("artifact", "artifact"),
                ("lineage", "lineage"), ("workload", "workload"),
                ("tokenizer", "tokenizer"),
                ("trust-policy", "trustPolicy"), ("command", "command"),
            ):
                expected = "sha256:" + hashlib.sha256(
                    inputs[cli_key].read_bytes()).hexdigest()
                self.assertEqual(payload["digests"][digest_key], expected)

            candidate = root / "results/spec107-candidate.json"
            create = subprocess.run([
                sys.executable, str(cli), "--repo-root", str(root),
                "candidate", "create", "--digests", str(output),
                "--output", str(candidate),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False)
            self.assertEqual(create.returncode, 0, create.stderr)
            self.assertTrue(candidate.is_file())

    def test_cli_candidate_inputs_rejects_missing_or_spec105_source(self) -> None:
        cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "spec107@test.invalid"],
                           cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Spec 107 Test"],
                           cwd=root, check=True)
            normal = root / "input.json"
            normal.write_text("{}\n", encoding="utf-8")
            forbidden = root / "spec105-input.json"
            forbidden.write_text("{}\n", encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)
            output = root / "results/inputs.json"
            command = [
                sys.executable, str(cli), "--repo-root", str(root),
                "candidate", "inputs",
                "--profile", str(forbidden),
            ]
            for key in (
                "model", "plan", "artifact", "lineage", "workload",
                "tokenizer", "trust-policy", "command",
            ):
                command.extend([f"--{key}", str(normal)])
            command.extend(["--output", str(output)])
            result = subprocess.run(
                command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("SPEC105_IDENTITY_REJECTED", result.stderr)
            self.assertFalse(output.exists())

            missing_command = list(command)
            profile_index = missing_command.index("--profile") + 1
            missing_command[profile_index] = str(root / "missing.json")
            result = subprocess.run(
                missing_command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("CANDIDATE_INPUT_INVALID:profile", result.stderr)
            self.assertFalse(output.exists())

            normal.write_text('{"dirty":true}\n', encoding="utf-8")
            dirty_command = list(command)
            dirty_command[profile_index] = str(normal)
            result = subprocess.run(
                dirty_command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("CANDIDATE_SOURCE_TREE_DIRTY", result.stderr)
            self.assertFalse(output.exists())

    def test_candidate_create_rejects_dirty_tracked_source_tree(self) -> None:
        cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "spec107@test.invalid"],
                           cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Spec 107 Test"],
                           cwd=root, check=True)
            tracked = root / "tracked.txt"
            tracked.write_text("frozen\n", encoding="utf-8")
            subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)
            tracked.write_text("dirty\n", encoding="utf-8")
            digests = root / "digests.json"
            digests.write_text(json.dumps(candidate_inputs()), encoding="utf-8")
            output = root / "results/spec107-candidate.json"
            result = subprocess.run([
                sys.executable, str(cli), "--repo-root", str(root),
                "candidate", "create", "--digests", str(digests),
                "--output", str(output),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("CANDIDATE_SOURCE_TREE_DIRTY", result.stderr)
            self.assertFalse(output.exists())

    def test_candidate_create_rejects_forged_committed_source_digest(self) -> None:
        cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "spec107@test.invalid"],
                           cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Spec 107 Test"],
                           cwd=root, check=True)
            (root / "tracked.txt").write_text("frozen\n", encoding="utf-8")
            subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)
            expected = committed_source_digest(root)
            self.assertEqual(expected, committed_source_digest(root))
            values = candidate_inputs()
            values["source"] = digest("f")
            digests = root / "digests.json"
            digests.write_text(json.dumps(values), encoding="utf-8")
            output = root / "results/spec107-candidate.json"
            result = subprocess.run([
                sys.executable, str(cli), "--repo-root", str(root),
                "candidate", "create", "--digests", str(digests),
                "--output", str(output),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("CANDIDATE_SOURCE_DIGEST_MISMATCH", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
