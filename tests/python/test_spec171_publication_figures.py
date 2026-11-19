#!/usr/bin/env python3
"""Synthetic aggregate/figure checks; these numbers are not experiment results."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "publication_figures", ROOT / "Experiments/generate_spec171_publication_figures.py")
figures = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(figures)


class PublicationFigureTests(unittest.TestCase):
    def aggregate(self):
        return dict(claim_verdict="NDNSF_MULTIPROVIDER_WORK_EFFICIENCY_ADVANTAGE_CONFIRMED",
            requests_per_system=10, seeds=[1, 2],
            success=dict(ndnsf=10, grpc_par_4=9, nsc_4=8, bootstrap_95_percent_points=[-2, 3]),
            provider_work=dict(ndnsf_provider_executions=10,
                grpc_server_handler_executions_snapshot_exact=40,
                nsc_provider_executions=11, ndnsf_to_grpc_execution_ratio=0.25))

    def test_synthetic_values_and_real_figure_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "synthetic-aggregate.json"
            path.write_text(json.dumps(self.aggregate()))
            report = figures.load_work_efficiency(path)
            self.assertEqual(report["requests"], 10)
            self.assertEqual(report["seeds"], [1, 2])
            self.assertEqual(report["success_pct"], [100.0, 90.0, 80.0])
            self.assertEqual(report["executions_per_request"], [1.0, 4.0, 1.1])
            self.assertEqual(report["execution_ratio"], 0.25)
            outputs = figures.write_figure(report, root / "figures")
            self.assertEqual({path.suffix for path in outputs}, {".png", ".pdf", ".svg"})
            self.assertTrue(all(path.is_file() and path.stat().st_size > 0 for path in outputs))

    def test_missing_measurement_cannot_borrow_a_paper_constant(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "aggregate.json"
            value = self.aggregate()
            del value["provider_work"]["nsc_provider_executions"]
            path.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError, "measured nsc_provider_executions"):
                figures.load_work_efficiency(path)


if __name__ == "__main__":
    unittest.main()
