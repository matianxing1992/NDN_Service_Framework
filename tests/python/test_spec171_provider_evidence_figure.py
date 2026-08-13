#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "Experiments/plot_spec171_provider_evidence.py"
SPEC = importlib.util.spec_from_file_location("spec171_provider_figure", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171ProviderEvidenceFigureTest(unittest.TestCase):
    def test_frozen_evidence_values_and_outputs(self):
        data = MODULE.load_evidence(
            ROOT / "specs/171-four-provider-mobility-advantage/evidence/"
                   "provider-transition-results-20260809/transition-summary.json",
            ROOT / "specs/171-four-provider-mobility-advantage/evidence/"
                   "opportunity-analysis-20260809/opportunity-summary.json")
        self.assertEqual(data["transition_replays"], 3)
        self.assertEqual(data["switch_required_requests"], 1747)
        self.assertEqual(
            data["post_retirement_systems"]["ndnsf"]["success"], 357)
        self.assertEqual(
            data["post_retirement_systems"]["ndnsf"]
                ["configured_provider_count"], 0)
        self.assertEqual(
            data["post_retirement_systems"]["grpc-static-3"]["success"], 0)
        self.assertEqual(
            data["post_retirement_systems"]["grpc-preregistered-4"]["success"],
            357)
        self.assertAlmostEqual(
            data["paired_seed_p95_reduction_ms"]["mean"], 512.9343276824951)

        with tempfile.TemporaryDirectory() as temporary:
            outputs = MODULE.write_figure(data, Path(temporary))
            self.assertEqual({path.suffix for path in outputs},
                             {".png", ".pdf", ".svg"})
            self.assertTrue(all(path.is_file() and path.stat().st_size > 0
                                for path in outputs))
            self.assertTrue((Path(temporary) / "figure-data.json").is_file())


if __name__ == "__main__":
    unittest.main()
