#!/usr/bin/env python3
"""Spec 107 sampled critical-path timing contract tests."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from spec107_timing import (  # noqa: E402
    COMPONENTS,
    TimingError,
    TimingSpan,
    reconcile_timing,
    stable_sample_allows,
)


def span(component: str, start: float, end: float, *, token_epoch: int = 0) -> dict[str, object]:
    return {
        "candidateId": "spec107-c1-111111111111-222222222222-333333333333-"
                       "444444444444-555555555555-666666666666",
        "campaignId": "spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
        "generationId": "generation-1",
        "tokenEpoch": token_epoch,
        "requestId": "request-1",
        "attemptEpoch": 0,
        "providerName": "/provider/0",
        "providerBootId": "boot-0",
        "role": "/LLM/Stage/0",
        "component": component,
        "startMs": start,
        "endMs": end,
        "status": "COMPLETED",
        "sampled": True,
    }


def observed(end_to_end_ms: float, *, token_epoch: int = 0) -> dict[str, object]:
    value = span("admission", 0, 1, token_epoch=token_epoch)
    result = {
        key: value[key]
        for key in ("candidateId", "campaignId", "generationId", "tokenEpoch",
                    "requestId", "attemptEpoch")
    }
    result.update({"endToEndMs": end_to_end_ms, "status": "COMPLETED"})
    return result


class Spec107TimingTest(unittest.TestCase):
    def _complete_spans(self, count: int = 1) -> list[dict[str, object]]:
        rows = []
        for token_epoch in range(count):
            for index, component in enumerate(COMPONENTS):
                rows.append(span(
                    component, index * 10.0, (index + 1) * 10.0,
                    token_epoch=token_epoch))
        return rows

    def test_span_schema_rejects_secrets_negative_duration_and_unknown_component(self) -> None:
        TimingSpan.from_dict(span("compute", 1, 2))
        secret = span("compute", 1, 2)
        secret["payload"] = "forbidden"
        with self.assertRaisesRegex(TimingError, "TIMING_FORBIDDEN_FIELD"):
            TimingSpan.from_dict(secret)
        with self.assertRaisesRegex(TimingError, "TIMING_INTERVAL_INVALID"):
            TimingSpan.from_dict(span("compute", 2, 1))
        with self.assertRaisesRegex(TimingError, "TIMING_COMPONENT_INVALID"):
            TimingSpan.from_dict(span("magic", 1, 2))

    def test_reconciles_complete_non_overlapping_critical_path(self) -> None:
        result = reconcile_timing(self._complete_spans(), [observed(100.0)])
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(result["coverageRatio"], 1.0)
        self.assertEqual(result["validStepCount"], 1)
        row = result["steps"][0]
        self.assertEqual(row["reconciledMs"], 100.0)
        self.assertEqual(row["unexplainedMs"], 0.0)

    def test_uses_larger_of_five_percent_or_ten_ms_tolerance(self) -> None:
        within = reconcile_timing(self._complete_spans(), [observed(109.0)])
        self.assertEqual(within["verdict"], "PASS")
        self.assertEqual(within["steps"][0]["toleranceMs"], 10.0)
        outside = reconcile_timing(self._complete_spans(), [observed(111.0)])
        self.assertEqual(outside["verdict"], "BLOCK_RECONCILIATION")
        self.assertIn("UNEXPLAINED_TIME", outside["steps"][0]["errors"])

    def test_rejects_overlap_missing_component_and_identity_mismatch(self) -> None:
        overlap = self._complete_spans()
        overlap[1]["startMs"] = 5.0
        result = reconcile_timing(overlap, [observed(100.0)])
        self.assertIn("OVERLAPPING_SPANS", result["steps"][0]["errors"])

        missing = self._complete_spans()
        missing.pop()
        result = reconcile_timing(missing, [observed(100.0)])
        self.assertIn("MISSING_COMPONENTS:inter-token", result["steps"][0]["errors"])

        mixed = self._complete_spans()
        mixed[0]["candidateId"] = "spec107-c1-other"
        with self.assertRaisesRegex(TimingError, "TIMING_IDENTITY_MISMATCH"):
            reconcile_timing(mixed, [observed(100.0)])

    def test_coverage_below_ninety_nine_percent_blocks(self) -> None:
        spans = self._complete_spans(100)
        observed_rows = [observed(100.0, token_epoch=index) for index in range(100)]
        passing = reconcile_timing(spans[:-10], observed_rows)
        self.assertEqual(passing["coverageRatio"], 0.99)
        self.assertEqual(passing["verdict"], "PASS")
        failing = reconcile_timing(spans[:-20], observed_rows)
        self.assertEqual(failing["coverageRatio"], 0.98)
        self.assertEqual(failing["verdict"], "BLOCK_COVERAGE")

    def test_sampler_is_stable_and_rate_one_keeps_every_request(self) -> None:
        values = [stable_sample_allows(f"/request/{index}", 10) for index in range(100)]
        self.assertEqual(values, [
            stable_sample_allows(f"/request/{index}", 10) for index in range(100)])
        self.assertGreater(sum(values), 0)
        self.assertLess(sum(values), 100)
        self.assertTrue(all(stable_sample_allows(f"/request/{index}", 1)
                            for index in range(100)))
        with self.assertRaisesRegex(TimingError, "TIMING_SAMPLE_RATE_INVALID"):
            stable_sample_allows("/request/1", 0)


if __name__ == "__main__":
    unittest.main()
