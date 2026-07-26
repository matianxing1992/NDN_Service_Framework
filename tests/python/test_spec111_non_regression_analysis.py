from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/spec111/analyze_non_regression_campaign.py"
RUNNER = ROOT / "tools/spec111/run_non_regression_campaign.py"


def load_module():
    spec = importlib.util.spec_from_file_location("spec111_analysis", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load Spec 111 analysis")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runner():
    spec = importlib.util.spec_from_file_location("spec111_runner", RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load Spec 111 runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def passing_summary() -> dict[str, object]:
    results = []
    for pair in range(1, 11):
        for variant in ("baseline", "treatment"):
            treatment = variant == "treatment"
            results.append({
                "pair": pair,
                "variant": variant,
                "status": "PASS",
                "candidateId": "candidate-final" if treatment else "baseline",
                "completedRequests": 60,
                "failedRequests": 0,
                "p50Ms": 104.0 if treatment else 100.0,
                "p95Ms": 208.0 if treatment else 200.0,
                "throughputRps": 0.97 if treatment else 1.0,
                "peakProcessTreeRssKiB": 1100 if treatment else 1000,
                "queueSamples": 0,
                "maxQueueObserved": None,
            })
    return {"formalComparisonEligible": True, "results": results}


class NonRegressionAnalysisTest(unittest.TestCase):
    def test_each_command_uses_unique_ephemeral_app_state_outside_results(self):
        runner = load_runner()
        first_output = ROOT / "results/spec111/a"
        second_output = ROOT / "results/spec111/b"

        first = runner.command_for(ROOT, "cell-a", 1, first_output)
        second = runner.command_for(ROOT, "cell-b", 2, second_output)
        first_state = Path(first[first.index("--app-state-root") + 1])
        second_state = Path(second[second.index("--app-state-root") + 1])

        self.assertNotEqual(first_state, second_state)
        self.assertEqual(first_state.parent, Path("/tmp"))
        self.assertTrue(first_state.name.startswith("spec111-app-state-"))
        self.assertFalse(str(first_state).startswith(str(ROOT / "results")))

    def test_constant_paired_changes_have_deterministic_passing_intervals(self):
        module = load_module()

        result = module.analyze(passing_summary())

        self.assertEqual(result["pairCount"], 10)
        self.assertEqual(result["baselineCompletedRequests"], 600)
        self.assertEqual(result["treatmentCompletedRequests"], 600)
        self.assertAlmostEqual(
            result["metrics"]["p50Ms"]["medianPairedRelativeChange"],
            0.04,
        )
        self.assertAlmostEqual(
            result["metrics"]["p50Ms"]["bootstrap95"][0], 0.04)
        self.assertAlmostEqual(
            result["metrics"]["p50Ms"]["bootstrap95"][1], 0.04)
        self.assertTrue(result["gate"]["pass"])

    def test_diagnostic_campaign_is_rejected(self):
        module = load_module()
        summary = passing_summary()
        summary["formalComparisonEligible"] = False

        with self.assertRaisesRegex(
                ValueError, "SPEC111_CAMPAIGN_NOT_FORMALLY_ELIGIBLE"):
            module.analyze(summary)

    def test_pre_flag_runner_summary_is_accepted_only_from_complete_facts(self):
        module = load_module()
        summary = passing_summary()
        del summary["formalComparisonEligible"]

        result = module.analyze(summary)

        self.assertTrue(result["gate"]["correctnessCompletion"])

    def test_mixed_treatment_candidates_are_rejected(self):
        module = load_module()
        summary = passing_summary()
        treatments = [
            result for result in summary["results"]
            if result["variant"] == "treatment"
        ]
        treatments[-1]["candidateId"] = "candidate-after-fix"

        with self.assertRaisesRegex(
                ValueError, "SPEC111_TREATMENT_CANDIDATE_NOT_SINGLE"):
            module.analyze(summary)


if __name__ == "__main__":
    unittest.main()
