import importlib.util
from pathlib import Path
import statistics
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO / "Experiments/analyze_distributed_repo_artifact.py"
SPEC = importlib.util.spec_from_file_location(
    "analyze_distributed_repo_artifact", MODULE_PATH
)
analysis = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = analysis
SPEC.loader.exec_module(analysis)


class PerformanceAnalysisTests(unittest.TestCase):
    def test_percentile_uses_linear_interpolation(self):
        values = [1, 2, 3, 4, 5]
        self.assertEqual(analysis.percentile(values, 0.0), 1)
        self.assertEqual(analysis.percentile(values, 0.5), 3)
        self.assertEqual(analysis.percentile(values, 0.95), 4.8)
        self.assertEqual(analysis.percentile(values, 1.0), 5)

    def test_distribution_retains_all_values_and_is_deterministic(self):
        first = analysis.distribution([1, 2, 3, 4, 5], seed_text="fixed")
        second = analysis.distribution([1, 2, 3, 4, 5], seed_text="fixed")
        self.assertEqual(first, second)
        self.assertEqual(first["n"], 5)
        self.assertEqual(first["median"], statistics.median([1, 2, 3, 4, 5]))
        self.assertEqual(first["iqr"], 2)

    def test_v1_evidence_is_normalized_without_claiming_cold_retrieval(self):
        value = analysis._normalize_legacy_record({
            "schemaVersion": 1,
            "wireBytes": 1200,
            "storageBytesRead": 1024,
            "storageBytesWritten": 1050,
            "phaseLatencyMs": {"reservation": 2.0},
        })
        self.assertEqual(value["dataWireBytes"], 1200)
        self.assertEqual(value["interestWireBytes"], 0)
        self.assertEqual(value["payloadStoreBytesRead"], 1024)
        self.assertEqual(value["metadataStoreBytesRead"], 0)
        self.assertEqual(value["phaseLatencyMs"], {"sessionStart": 2.0})
        self.assertFalse(value["coldDestinationVisible"])

    def test_point_pass_with_uncertain_ci_is_not_reported_as_pass(self):
        summary = [{
            "pairId": "s67108864-r1-c1",
            "subject": "digest-only",
            "distribution": {
                "median": 0.99,
                "medianBootstrap95Ci": [0.70, 1.10],
            },
        }]
        signed = [{
            "pairId": "s67108864-r1-c1",
            "comparison": "signed/digest",
            "distribution": {
                "median": 0.95,
                "medianBootstrap95Ci": [0.80, 1.10],
            },
        }]
        manifest = {
            "runSchedule": [],
            "cells": [],
        }
        verdicts = analysis._threshold_verdicts(
            manifest, [], summary, signed
        )
        self.assertEqual(verdicts["SC-002"]["verdict"], "INCONCLUSIVE")
        self.assertEqual(verdicts["SC-003"]["verdict"], "INCONCLUSIVE")

    def test_zero_digest_goodput_is_not_used_as_ratio_denominator(self):
        base = {
            "pairId": "s1048576-r1-c4",
            "payloadBytes": 1048576,
            "replicas": 1,
            "concurrency": 4,
            "repetition": 1,
            "warmup": False,
        }
        records = [
            dict(base, subject="raw-segmented-ndn", logicalGoodputMbps=1.0),
            dict(base, subject="digest-only", logicalGoodputMbps=0.0),
            dict(base, subject="signed-manifest", logicalGoodputMbps=0.0),
        ]
        repository_raw, signed_digest = analysis._paired_ratios(records)
        self.assertEqual(len(repository_raw), 2)
        self.assertEqual(signed_digest, [])

    def test_failed_signed_sample_forces_sc003_completion_failure(self):
        summary = [{
            "pairId": "s67108864-r1-c4",
            "comparison": "signed/digest",
            "distribution": {
                "median": 0.99,
                "medianBootstrap95Ci": [0.95, 1.0],
            },
        }]
        common = {
            "warmup": False,
            "payloadBytes": 67108864,
            "replicas": 1,
            "concurrency": 4,
        }
        records = [
            dict(common, runId="digest", subject="digest-only", verdict="PASS"),
            dict(common, runId="signed", subject="signed-manifest", verdict="FAIL"),
        ]
        verdicts = analysis._threshold_verdicts(
            {"runSchedule": [], "cells": []}, records, [], summary
        )
        self.assertEqual(verdicts["SC-003"]["completionGate"], "FAIL")
        self.assertEqual(verdicts["SC-003"]["verdict"], "FAIL")

    def test_small_object_failure_is_reported_but_outside_sc003_gate(self):
        summary = [{
            "pairId": "s1048576-r1-c16",
            "comparison": "signed/digest",
            "distribution": {
                "median": 0.50,
                "medianBootstrap95Ci": [0.40, 0.60],
            },
        }, {
            "pairId": "s67108864-r1-c1",
            "comparison": "signed/digest",
            "distribution": {
                "median": 0.99,
                "medianBootstrap95Ci": [0.95, 1.01],
            },
        }]
        common = {
            "warmup": False,
            "replicas": 1,
            "concurrency": 1,
            "writeAmplification": 1.0,
            "readAmplification": 1.0,
            "payloadStoreBytesRead": 1,
            "coldDestinationVisible": True,
        }
        records = [
            dict(common, runId="small-digest", payloadBytes=1048576,
                 subject="digest-only", verdict="FAIL"),
            dict(common, runId="small-signed", payloadBytes=1048576,
                 subject="signed-manifest", verdict="PASS"),
            dict(common, runId="large-digest", payloadBytes=67108864,
                 subject="digest-only", verdict="PASS"),
            dict(common, runId="large-signed", payloadBytes=67108864,
                 subject="signed-manifest", verdict="PASS"),
        ]
        verdicts = analysis._threshold_verdicts(
            {"runSchedule": [], "cells": []}, records, [], summary
        )
        self.assertEqual(verdicts["SC-003"]["completionGate"], "PASS")
        self.assertEqual(verdicts["SC-003"]["verdict"], "PASS")
        self.assertEqual(verdicts["SC-003"]["eligibleCells"], 1)
        self.assertEqual(
            verdicts["SC-003"]["minimumArtifactBytes"], 67108864
        )


if __name__ == "__main__":
    unittest.main()
