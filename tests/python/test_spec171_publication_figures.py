#!/usr/bin/env python3

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "spec171_publication_figures",
    REPO_ROOT / "Experiments" / "generate_spec171_publication_figures.py",
)
figures = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(figures)


class Spec171PublicationFigureTests(unittest.TestCase):
    AGGREGATE = (REPO_ROOT / "results" /
                 "four_provider_work_efficiency_confirmatory_20260806" /
                 "combined-six-seed-aggregate.json")

    def test_frozen_aggregate_values_and_outputs(self):
        report = figures.load_work_efficiency(self.AGGREGATE)
        self.assertEqual(report["requests"], 1800)
        self.assertEqual(report["seeds"], [20, 21, 22, 23, 24, 25])
        self.assertEqual(report["success_pct"], [99.88888888888889, 100.0, 100.0])
        self.assertAlmostEqual(report["executions_per_request"][0], 1.0)
        self.assertAlmostEqual(report["executions_per_request"][1], 3.988888888888889)
        self.assertAlmostEqual(report["execution_ratio"], 0.25069637883008357)

        with tempfile.TemporaryDirectory() as temporary:
            outputs = figures.write_figure(report, Path(temporary))
            self.assertEqual({path.suffix for path in outputs}, {".png", ".pdf", ".svg"})
            self.assertTrue(all(path.is_file() and path.stat().st_size > 0
                                for path in outputs))


if __name__ == "__main__":
    unittest.main()
