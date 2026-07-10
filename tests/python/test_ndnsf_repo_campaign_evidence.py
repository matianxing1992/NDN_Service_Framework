#!/usr/bin/env python3
"""Evidence-contract tests for Repo outage/recovery campaigns."""

from __future__ import annotations

import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from Experiments.repo_campaign_evidence import (
    correlate_recovered_repairs,
    parse_catalog_sync_metric,
)


class RepoCampaignEvidenceTest(unittest.TestCase):
    def test_parses_repair_cycle_and_merge_metrics(self) -> None:
        cycle = parse_catalog_sync_metric(
            "catalog_sync repair_cycle repo=/repo/A phase=post-merge "
            "created=4 claimable=3 claimed=3 completed=2 failed=1 "
            "scanMs=12.500 timestampMs=1234")
        self.assertEqual(cycle["kind"], "repairCycle")
        self.assertEqual(cycle["repo"], "/repo/A")
        self.assertEqual(cycle["claimable"], 3)
        self.assertEqual(cycle["scanMs"], 12.5)
        merge = parse_catalog_sync_metric(
            "catalog_sync merged repo=/repo/A peer=/repo/B entries=37 "
            "mode=pull batches=1 segments=13 payloadBytes=74620 fallback=0 "
            "durationMs=88.250 timestampMs=1300")
        self.assertEqual(merge["kind"], "catalogMerge")
        self.assertEqual(merge["entries"], 37)
        self.assertEqual(merge["mode"], "pull")
        self.assertEqual(merge["segments"], 13)
        self.assertEqual(merge["payloadBytes"], 74620)
        self.assertEqual(merge["durationMs"], 88.25)
        self.assertIsNone(parse_catalog_sync_metric("unrelated log line"))

    def test_correlates_only_successful_writes_completed_during_outage(self) -> None:
        rows = [
            {"operation": "write", "success": "1", "objectName": "/before",
             "startedEpochMs": "900", "completedEpochMs": "950"},
            {"operation": "write", "success": "1", "objectName": "/outage-a",
             "startedEpochMs": "1100", "completedEpochMs": "1200"},
            {"operation": "write", "success": "1", "objectName": "/outage-b",
             "startedEpochMs": "1300", "completedEpochMs": "1400"},
            {"operation": "write", "success": "0", "objectName": "/failed",
             "startedEpochMs": "1300", "completedEpochMs": "1400"},
            {"operation": "read", "success": "1", "objectName": "/outage-a",
             "startedEpochMs": "1300", "completedEpochMs": "1400"},
            {"operation": "write", "success": "1", "objectName": "/after",
             "startedEpochMs": "1600", "completedEpochMs": "1700"},
        ]
        events = [
            {"repoNode": "/repo/A", "objectName": "/outage-a",
             "timestampMs": 1800},
            {"repoNode": "/repo/B", "objectName": "/outage-b",
             "timestampMs": 1750},
            {"repoNode": "/repo/A", "objectName": "/unrelated",
             "timestampMs": 1900},
            {"repoNode": "/repo/A", "objectName": "/failed",
             "timestampMs": 1950},
        ]

        evidence = correlate_recovered_repairs(
            rows, events, recovered_repo="/repo/A",
            failure_epoch_ms=1000, restart_epoch_ms=1500)

        self.assertEqual(evidence["outageWriteObjects"],
                         ["/outage-a", "/outage-b"])
        self.assertEqual(evidence["repairedOutageObjects"], ["/outage-a"])
        self.assertEqual(evidence["unrepairedOutageObjects"], ["/outage-b"])
        self.assertEqual(evidence["repairCoverage"], 0.5)
        self.assertEqual(evidence["successfulWriteObjects"],
                         ["/after", "/before", "/outage-a", "/outage-b"])
        self.assertEqual(evidence["failedOnlyWriteObjects"], ["/failed"])
        self.assertEqual(evidence["invalidRepairEventCount"], 1)
        self.assertEqual(
            evidence["repairEventsForFailedWrites"][0]["objectName"],
            "/failed",
        )
        self.assertEqual(evidence["recoveredTargetRepairEventCount"], 3)
        self.assertEqual(evidence["firstRepairAfterRestartMs"], 300)
        self.assertEqual(evidence["lastRepairAfterRestartMs"], 450)

    def test_empty_outage_has_zero_coverage_without_division_error(self) -> None:
        evidence = correlate_recovered_repairs(
            [], [], recovered_repo="/repo/A",
            failure_epoch_ms=1000, restart_epoch_ms=1500)
        self.assertEqual(evidence["outageSuccessfulWriteCount"], 0)
        self.assertEqual(evidence["repairCoverage"], 0.0)
        self.assertEqual(evidence["invalidRepairEventCount"], 0)
        self.assertIsNone(evidence["firstRepairAfterRestartMs"])


if __name__ == "__main__":
    unittest.main()
