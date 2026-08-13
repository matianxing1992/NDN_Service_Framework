import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import yaml


REPO = Path(__file__).resolve().parents[2]
ANALYZER = REPO / "scripts" / "analyze_paper_submission_evidence.py"
REGISTRATION = (
    REPO / "specs/173-paper-submission-evidence/contracts/experiment-registration.yaml"
)
SCHEMA = REPO / "specs/173-paper-submission-evidence/contracts/artifact-index.schema.json"
ARTIFACT_INDEX = REPO / "specs/173-paper-submission-evidence/evidence/artifact-index.json"


def load_analyzer_module():
    spec = importlib.util.spec_from_file_location("paper_submission_evidence", ANALYZER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_cell(root, system, summary, comparison="one-provider-baseline", rate=10,
               repetition="r1", extra_files=None):
    cell = root / system
    cell.mkdir(parents=True)
    summary_path = cell / "summary.json"
    summary_path.write_text(json.dumps(summary, sort_keys=True) + "\n")
    for name, content in (extra_files or {}).items():
        (cell / name).write_text(content)
    manifest = {
        "schemaVersion": 1,
        "cellId": f"{comparison}--{repetition}--rps-{rate}--{system}",
        "comparison": comparison,
        "repetition": repetition,
        "seed": 17300 + int(repetition[1:]),
        "rateRps": rate,
        "system": system,
        "outputDirectory": str(cell),
        "exactCommand": ["fixture", system],
        "registrationSha256": "a" * 64,
        "toolchainManifestSha256": "b" * 64,
        "sourceRevisions": {"ndnsf": {"head": "c" * 40}},
        "runtimeArtifacts": {},
    }
    (cell / "cell-manifest.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")
    hashes = {"summary.json": sha256(summary_path)}
    for name in extra_files or {}:
        hashes[name] = sha256(cell / name)
    result = {
        "schemaVersion": 1,
        "status": "valid",
        "exitCode": 0,
        "requiredSummaries": ["summary.json"],
        "artifactHashes": hashes,
    }
    (cell / "cell-result.json").write_text(json.dumps(result, sort_keys=True) + "\n")
    return cell


class PaperSubmissionEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.analyzer = load_analyzer_module()
        self.registration = yaml.safe_load(REGISTRATION.read_text())

    def test_normalizes_three_frameworks_to_one_outcome_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ndnsf = write_cell(root, "ndnsf", {
                "sent_count": 600,
                "total_successful_responses": 570,
                "timed_out_count": 20,
                "pending_at_shutdown": 5,
                "average_latency_ms": 42.0,
                "p50_latency_ms": 40.0,
                "p95_latency_ms": 70.0,
                "selected_provider_distribution": {"/ucla": 570},
                "provider_final_response_count": {"/ucla": 570},
            })
            grpc_log = "\n".join([
                "request_failed error=StatusCode.DEADLINE_EXCEEDED" for _ in range(7)
            ]) + "\nGRPC_CLIENT_RATE sent=600 success=590 failures=10 duration_s=60\n"
            grpc = write_cell(root, "grpc", {
                "count": 600,
                "duration_s": 60.0,
                "summary_line": (
                    "GRPC_CLIENT_SUMMARY count=590 avg_ms=7.5 p50_ms=7.0 "
                    "p95_ms=9.0 min_ms=6.0 max_ms=12.0"
                ),
            }, extra_files={"client.log": grpc_log})
            nsc = write_cell(root, "nsc", {
                "duration_s": 60.0,
                "summaries": [{
                    "count": 600, "success": 580, "timeout": 15,
                    "avg_ms": 120.0, "p50_ms": 110.0, "p95_ms": 180.0,
                }],
            })

            normalized = {
                name: self.analyzer.normalize_cell(path, self.registration)
                for name, path in (("ndnsf", ndnsf), ("grpc", grpc), ("nsc", nsc))
            }
            self.assertEqual(normalized["ndnsf"]["scheduledRequests"], 600)
            self.assertEqual(normalized["ndnsf"]["issuedRequests"], 600)
            self.assertEqual(normalized["ndnsf"]["successfulRequests"], 570)
            self.assertEqual(normalized["ndnsf"]["timedOutRequests"], 20)
            self.assertEqual(normalized["ndnsf"]["otherFailedRequests"], 5)
            self.assertEqual(normalized["ndnsf"]["pendingRequests"], 5)
            self.assertEqual(normalized["ndnsf"]["throughputSuccessfulRps"], 9.5)
            self.assertEqual(normalized["ndnsf"]["latencySampleCount"], 570)

            self.assertEqual(normalized["grpc"]["successfulRequests"], 590)
            self.assertEqual(normalized["grpc"]["timedOutRequests"], 7)
            self.assertEqual(normalized["grpc"]["otherFailedRequests"], 3)
            self.assertEqual(normalized["grpc"]["latencyMeanMs"], 7.5)

            self.assertEqual(normalized["nsc"]["successfulRequests"], 580)
            self.assertEqual(normalized["nsc"]["timedOutRequests"], 15)
            self.assertEqual(normalized["nsc"]["otherFailedRequests"], 5)
            self.assertAlmostEqual(normalized["nsc"]["successRateIssued"], 580 / 600)

    def test_mechanism_aware_load_validity_does_not_reject_intended_admission(self):
        base = {
            "comparison": "one-provider-baseline",
            "system": "ndnsf",
            "scheduledRequests": 600,
            "issuedRequests": 470,
            "admittedRequests": None,
            "successfulRequests": 470,
            "timedOutRequests": 0,
            "otherFailedRequests": 0,
            "pendingRequests": 0,
            "admissionCountersPresent": False,
        }
        valid, reasons = self.analyzer.validate_normalized_run(base, self.registration)
        self.assertFalse(valid)
        self.assertTrue(any("80%" in reason for reason in reasons))

        admitted = dict(base)
        admitted.update({
            "comparison": "admission-control",
            "system": "ndnsf-admission-enabled",
            "issuedRequests": 400,
            "admittedRequests": 400,
            "successfulRequests": 395,
            "timedOutRequests": 5,
            "admissionCountersPresent": True,
        })
        valid, reasons = self.analyzer.validate_normalized_run(admitted, self.registration)
        self.assertTrue(valid, reasons)
        admitted["admissionCountersPresent"] = False
        valid, reasons = self.analyzer.validate_normalized_run(admitted, self.registration)
        self.assertFalse(valid)
        self.assertTrue(any("admission counters" in reason for reason in reasons))

    def test_aggregation_uses_independent_repetitions_not_packets(self):
        runs = []
        for repetition, throughput in (("r1", 9.0), ("r2", 10.0), ("r3", 11.0)):
            runs.append({
                "comparison": "one-provider-baseline",
                "rateRps": 10,
                "system": "ndnsf",
                "repetition": repetition,
                "valid": True,
                "throughputSuccessfulRps": throughput,
                "successRateIssued": throughput / 10,
                "completionRateScheduled": throughput / 10,
                "latencyMeanMs": 40.0 + throughput,
                "latencyP50Ms": 35.0 + throughput,
                "latencyP95Ms": 60.0 + throughput,
            })
        aggregate = self.analyzer.aggregate_repetitions(runs)[0]
        throughput = aggregate["metrics"]["throughputSuccessfulRps"]
        self.assertEqual(aggregate["independentUnit"], "process-repetition")
        self.assertEqual(aggregate["repetitions"], ["r1", "r2", "r3"])
        self.assertEqual(throughput, {
            "n": 3, "mean": 10.0, "sampleSd": 1.0, "min": 9.0, "max": 11.0,
        })
        self.assertNotIn("packetCount", aggregate)
        self.assertFalse(aggregate["significanceClaimAllowed"])

    def test_artifact_schema_and_manuscript_audit_reject_removed_precision(self):
        index = json.loads(ARTIFACT_INDEX.read_text())
        self.analyzer.validate_artifact_index(index, SCHEMA)
        broken = json.loads(json.dumps(index))
        supported = next(item for item in broken["entries"] if item["status"] == "supported")
        supported["evidenceIds"] = []
        with self.assertRaises(Exception):
            self.analyzer.validate_artifact_index(broken, SCHEMA)

        with tempfile.TemporaryDirectory() as temporary:
            tex = Path(temporary) / "paper.tex"
            tex.write_text("\\begin{table} 42.0 ms \\label{tab:admission-control}\\end{table}\n")
            findings = self.analyzer.audit_manuscript_precision(tex, index)
            self.assertEqual(len(findings), 1)
            self.assertEqual(findings[0]["manuscriptId"], "tab:admission-control")
            self.assertEqual(findings[0]["status"], "removed")

    def test_fixture_analysis_is_byte_deterministic(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            campaign = root / "campaign"
            attempt = campaign / "runs" / "one-provider-baseline--r1--rps-10" / "attempt-0001"
            write_cell(attempt, "ndnsf", {
                "sent_count": 600,
                "total_successful_responses": 600,
                "timed_out_count": 0,
                "pending_at_shutdown": 0,
                "average_latency_ms": 40.0,
                "p50_latency_ms": 39.0,
                "p95_latency_ms": 50.0,
                "selected_provider_distribution": {"/ucla": 600},
                "provider_final_response_count": {"/ucla": 600},
            })
            (attempt / "block-result.json").write_text(json.dumps({
                "schemaVersion": 1,
                "blockId": "one-provider-baseline--r1--rps-10",
                "status": "valid",
            }) + "\n")
            first = root / "first"
            second = root / "second"
            self.analyzer.analyze_campaign(campaign, first, REGISTRATION)
            self.analyzer.analyze_campaign(campaign, second, REGISTRATION)
            for name in (
                "normalized-runs.json", "aggregate-statistics.json",
                "exclusions.json", "analysis-manifest.json",
            ):
                self.assertEqual((first / name).read_bytes(), (second / name).read_bytes())


if __name__ == "__main__":
    unittest.main()
