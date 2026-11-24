#!/usr/bin/env python3
"""Spec 107 diagnostic attribution and single-branch gate tests."""

from __future__ import annotations

from pathlib import Path
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from run_spec107_attribution import (  # noqa: E402
    AttributionError,
    build_bottleneck_decision,
    derive_attribution_inputs,
    load_json_lines,
    parse_timeline_log,
)
from spec107_artifacts import materialize_artifact_set  # noqa: E402
from spec107_identity import (  # noqa: E402
    build_campaign_identity,
    build_candidate_identity,
    committed_source_digest,
)


CANDIDATE = (
    "spec107-c1-111111111111-222222222222-333333333333-"
    "444444444444-555555555555-666666666666")


def diagnostic_campaign() -> dict[str, object]:
    return {
        "schema": "ndnsf-di-spec107-campaign-v1",
        "campaignId": "spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
        "candidateId": CANDIDATE,
        "kind": "diagnostic",
        "eligibility": "DIAGNOSTIC_INELIGIBLE",
        "releaseEligible": False,
    }


def reconciliation() -> dict[str, object]:
    return {
        "schema": "ndnsf-di-spec107-timing-reconciliation-v1",
        "candidateId": CANDIDATE,
        "campaignId": "spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
        "coverageRatio": 1.0,
        "verdict": "PASS",
        "steps": [
            {"observedMs": 1000.0, "reconciledMs": 1000.0, "errors": []},
            {"observedMs": 800.0, "reconciledMs": 800.0, "errors": []},
        ],
    }


class Spec107AttributionTest(unittest.TestCase):


    def test_raw_events_derive_complete_reconciliation_and_hypotheses(self) -> None:
        campaign = diagnostic_campaign()
        request_id = "/request/1"
        client = []
        for component, event, start, end in (
            ("inter-token", "inter_token", 0.0, 0.0),
            ("encode-decode", "request_encode", 0.0, 1.0),
            ("encode-decode", "response_decode", 9.0, 10.0),
            ("observed-step", "token_step", 0.0, 10.0),
        ):
            client.append({
                "schema": "ndnsf-di-spec107-client-timing-event-v1",
                "candidateId": campaign["candidateId"],
                "campaignId": campaign["campaignId"],
                "generationId": "generation-1", "tokenEpoch": 0,
                "requestId": request_id, "attemptEpoch": 0,
                "component": component, "event": event,
                "startMs": start, "endMs": end,
                "status": "COMPLETED", "sampled": True,
            })
        pairs = (
            ("request_created", "request_publish_done"),
            ("ack_selection_start", "ack_selection_done"),
            ("role_validation_start", "role_validation_done"),
            ("role_queue_enter", "role_queue_exit"),
            ("role_compute_start", "role_compute_done"),
            ("dependency_fetch_start", "dependency_fetch_done"),
            ("dependency_publish_start", "dependency_publish_done"),
            ("response_observed", "callback_done"),
        )
        timeline = []
        steady = 0
        for start_event, done_event in pairs:
            timeline.extend([
                {"event": start_event, "steadyUs": steady,
                 "requestId": request_id, "fields": {}},
                {"event": done_event, "steadyUs": steady + 1000,
                 "requestId": request_id, "fields": {}},
            ])
            steady += 1000
        reconciliation, hypotheses = derive_attribution_inputs(
            campaign=campaign, client_events=client, timeline_rows=timeline)
        self.assertEqual(reconciliation["verdict"], "PASS")
        self.assertEqual(reconciliation["coverageRatio"], 1.0)
        self.assertEqual(len(hypotheses), 4)
    def test_selects_exactly_one_unique_branch_at_or_above_twenty_five_percent(self) -> None:
        hypotheses = [
            {"branch": "generation-session", "avoidableMs": 500.0,
             "sourceTouchpoints": ["llm_pipeline/user.py"]},
            {"branch": "onnx-compute", "avoidableMs": 200.0,
             "sourceTouchpoints": ["OnnxRuntimeModelRunner.cpp"]},
            {"branch": "tensor-codec", "avoidableMs": 100.0,
             "sourceTouchpoints": ["TensorBundleCodec.cpp"]},
        ]
        result = build_bottleneck_decision(
            campaign=diagnostic_campaign(), reconciliation=reconciliation(),
            hypotheses=hypotheses)
        self.assertEqual(result["verdict"], "SELECTED")
        self.assertEqual(result["selectedBranch"], "generation-session")
        self.assertAlmostEqual(result["dominanceRatio"], 500.0 / 900.0)
        self.assertEqual(
            [row["branch"] for row in result["rejectedBranches"]],
            ["onnx-compute", "tensor-codec"],
        )
        self.assertFalse(result["releaseEligible"])
        self.assertEqual(result["eligibility"], "DIAGNOSTIC_INELIGIBLE")

    def test_no_branch_at_threshold_requires_replan(self) -> None:
        result = build_bottleneck_decision(
            campaign=diagnostic_campaign(), reconciliation=reconciliation(),
            hypotheses=[
                {"branch": "a", "avoidableMs": 200.0,
                 "sourceTouchpoints": ["a.cpp"]},
                {"branch": "b", "avoidableMs": 190.0,
                 "sourceTouchpoints": ["b.cpp"]},
            ])
        self.assertEqual(result["verdict"], "BLOCK_REPLAN")
        self.assertIsNone(result["selectedBranch"])
        self.assertEqual(result["reason"], "NO_BRANCH_MEETS_DOMINANCE")

    def test_tied_largest_branches_require_replan_not_arbitrary_selection(self) -> None:
        result = build_bottleneck_decision(
            campaign=diagnostic_campaign(), reconciliation=reconciliation(),
            hypotheses=[
                {"branch": "a", "avoidableMs": 300.0,
                 "sourceTouchpoints": ["a.cpp"]},
                {"branch": "b", "avoidableMs": 300.0,
                 "sourceTouchpoints": ["b.cpp"]},
            ])
        self.assertEqual(result["verdict"], "BLOCK_REPLAN")
        self.assertEqual(result["reason"], "LARGEST_BRANCH_NOT_UNIQUE")

    def test_acceptance_eligible_or_identity_mismatched_campaign_is_rejected(self) -> None:
        campaign = diagnostic_campaign()
        campaign["releaseEligible"] = True
        campaign["eligibility"] = "EVIDENCE_ELIGIBLE"
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_CAMPAIGN_NOT_DIAGNOSTIC"):
            build_bottleneck_decision(
                campaign=campaign, reconciliation=reconciliation(), hypotheses=[])

        campaign = diagnostic_campaign()
        evidence = reconciliation()
        evidence["campaignId"] = "spec107-c1-diagnostic-r2-bbbbbbbbbbbb"
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_IDENTITY_MISMATCH"):
            build_bottleneck_decision(
                campaign=campaign, reconciliation=evidence, hypotheses=[])

    def test_incomplete_timing_duplicate_hypothesis_and_invalid_measurement_fail(self) -> None:
        evidence = reconciliation()
        evidence["coverageRatio"] = 0.98
        evidence["verdict"] = "BLOCK_COVERAGE"
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_TIMING_INVALID"):
            build_bottleneck_decision(
                campaign=diagnostic_campaign(), reconciliation=evidence, hypotheses=[])

        duplicate = [
            {"branch": "a", "avoidableMs": 300.0, "sourceTouchpoints": ["a"]},
            {"branch": "a", "avoidableMs": 200.0, "sourceTouchpoints": ["b"]},
        ]
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_BRANCH_DUPLICATE"):
            build_bottleneck_decision(
                campaign=diagnostic_campaign(), reconciliation=reconciliation(),
                hypotheses=duplicate)

        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_MEASUREMENT_INVALID"):
            build_bottleneck_decision(
                campaign=diagnostic_campaign(), reconciliation=reconciliation(),
                hypotheses=[{"branch": "a", "avoidableMs": -1,
                             "sourceTouchpoints": ["a"]}])


    def test_parses_core_timeline_and_json_lines(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "provider.log"
            log.write_text(
                "prefix NDNSF_TIMELINE role=di-provider event=role_compute_start "
                "steady_us=123 timestamp_us=456 requestId=/request/1 "
                "sessionId=generation-1 role=/LLM/Stage/0 attemptEpoch=0\n",
                encoding="utf-8")
            rows = parse_timeline_log(log)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["event"], "role_compute_start")
            self.assertEqual(rows[0]["fields"]["attemptEpoch"], "0")

            jsonl = root / "events.jsonl"
            jsonl.write_text('{"sampled":true}\n', encoding="utf-8")
            self.assertEqual(load_json_lines(jsonl), [{"sampled": True}])


if __name__ == "__main__":
    unittest.main()
