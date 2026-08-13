#!/usr/bin/env python3

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "mobility_comparison_analysis",
    REPO_ROOT / "Experiments" / "analyze_mobility_comparison.py",
)
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


class MobilityComparisonAnalysisTests(unittest.TestCase):
    def _fixture(self, root: Path) -> Path:
        records = []
        for seed, scale in ((43, 0.0), (44, 0.02), (45, 0.04)):
            trace_hash = f"trace-{seed}"
            metrics = {
                "whole_trace": {"epochs": 10, "start_s": None, "end_s": None,
                                "at_least_one_fraction": 0.8,
                                "all_unreachable_fraction": 0.2,
                                "at_least_two_fraction": 0.4,
                                "max_reachable": 3},
                "measurement_window": {"epochs": 600, "start_s": 4.0,
                                        "end_s": 64.0,
                                        "at_least_one_fraction": 0.8,
                                        "all_unreachable_fraction": 0.2,
                                        "at_least_two_fraction": 0.4,
                                        "max_reachable": 3},
            }
            for system, success_rate, mean_ms, p95_ms in (
                ("grpc-single", 0.40 + scale, 20.0, 40.0),
                ("grpc", 0.70 + scale, 100.0, 300.0),
                ("ndnsf", 0.80 + scale, 50.0, 100.0),
            ):
                sent = 10
                success = round(sent * success_rate)
                manifest = root / f"manifest-{seed}-{system}.json"
                manifest.write_text(json.dumps({
                    "trace_sha256": trace_hash,
                    "traffic_start_delay_s": 4.0,
                }) + "\n", encoding="utf-8")
                records.append({
                    "seed": seed,
                    "condition": "range-50-speed-2p0",
                    "system": system,
                    "status": "complete",
                    "cell_manifest": str(manifest),
                    "trace_source_match": True,
                    "request_count_match": True,
                    "traffic_phase_match": True,
                    "trace_metrics": metrics,
                    "summary": {
                        "sent": sent,
                        "success": success,
                        "mean_ms": mean_ms,
                        "p50_ms": mean_ms,
                        "p95_ms": p95_ms,
                        "p99_ms": p95_ms,
                        "attempts": 1 if system == "grpc-single" else 2,
                        "failovers": 0 if system == "grpc-single" else 1,
                        "provider_executions": success if system == "ndnsf" else 0,
                        "deadline_failures": sent - success,
                        "traffic_launch_offset_s": 4.0,
                        "measurement_start_lateness_ms": 1.0,
                    },
                })
        aggregate = root / "aggregate.json"
        aggregate.write_text(json.dumps({"records": records}) + "\n", encoding="utf-8")
        return aggregate

    def test_validate_summarize_and_write_publication_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            aggregate = self._fixture(root)
            rows = analysis.validate_records(json.loads(aggregate.read_text()))
            result = analysis.summarize(rows, aggregate_path=aggregate)
            self.assertEqual(result["claim_verdict"], "CONFIRMED_CONDITIONAL_ADVANTAGE")
            self.assertEqual(result["coverage"]["all_unreachable_fraction"], 0.2)
            output = root / "figure"
            analysis.write_outputs(result, output)
            for name in (
                "analysis.json", "per-seed.csv", "README.md",
                "mobility-comparison.png", "mobility-comparison.svg",
                "mobility-comparison.pdf",
            ):
                self.assertTrue((output / name).is_file(), name)
            readme = (output / "README.md").read_text(encoding="utf-8")
            self.assertIn("Seed-level paired success", readme)
            self.assertIn("not treated as independent seed replicates", readme)

    def test_validation_rejects_a_mismatched_measurement_phase(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            aggregate = self._fixture(root)
            payload = json.loads(aggregate.read_text())
            payload["records"][0]["traffic_phase_match"] = False
            with self.assertRaisesRegex(ValueError, "traffic phase mismatch"):
                analysis.validate_records(payload)


if __name__ == "__main__":
    unittest.main()
