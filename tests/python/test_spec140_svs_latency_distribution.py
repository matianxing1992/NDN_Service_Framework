from __future__ import annotations

import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from Experiments import NDN_SVS_Latency_Distribution_Minindn as runner
from Experiments import analyze_svs_latency_distribution as analyzer


class DistributionTests(unittest.TestCase):
    def test_known_population_records_all_requested_statistics(self) -> None:
        self.assertEqual(
            analyzer.distribution([1, 2, 3, 4, 5, 100]),
            {
                "deliverySamples": 6,
                "deliveryMeanNs": 19,
                "deliveryP50Ns": 3,
                "deliveryP95Ns": 100,
                "deliveryP99Ns": 100,
            },
        )

    def test_empty_population_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "empty"):
            analyzer.distribution([])

    def test_legacy_p99_only_summary_is_not_backfilled(self) -> None:
        summary = {
            "schema": "spec136.peer-summary.v6",
            "deliveredMeasured": 3,
            "deliveryP99Ns": 30,
        }
        _, errors = analyzer.validate_peer(summary, [10, 20, 30], peer="peer-a")
        self.assertTrue(any("legacy" in error for error in errors))
        self.assertTrue(any("deliveryP50Ns" in error for error in errors))
        self.assertTrue(any("deliveryP95Ns" in error for error in errors))

    def test_summary_count_and_statistics_must_match_raw_samples(self) -> None:
        summary = {
            "schema": analyzer.EXPECTED_SCHEMA,
            "deliveredMeasured": 2,
            **analyzer.distribution([10, 20, 30]),
        }
        summary["deliveryP50Ns"] = 999
        _, errors = analyzer.validate_peer(summary, [10, 20, 30], peer="peer-b")
        self.assertTrue(any("deliveredMeasured" in error for error in errors))
        self.assertTrue(any("deliveryP50Ns" in error for error in errors))


class CampaignTests(unittest.TestCase):
    @staticmethod
    def _write_json(path: Path, value: object) -> None:
        path.write_text(json.dumps(value) + "\n", encoding="utf-8")

    @staticmethod
    def _write_samples(path: Path, values: list[int]) -> None:
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(["latencyNs"])
            writer.writerows([[value] for value in values])

    def test_two_peer_samples_are_concatenated_before_percentiles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            matrix = [
                {"ordinal": 1, "mode": "face-inline-rsa", "ratePerPeer": 400},
                {"ordinal": 2, "mode": "worker-rsa", "ratePerPeer": 400},
            ]
            self._write_json(
                campaign / "campaign-manifest.json",
                {"schema": "spec140.campaign.v1", "matrix": matrix},
            )
            terminals = []
            samples_by_mode = {
                "face-inline-rsa": ([1, 2, 3], [100, 101, 102]),
                "worker-rsa": ([10, 20, 30], [40, 50, 60]),
            }
            for ordinal, (mode, _) in enumerate(runner.MATRIX, 1):
                cell_id = f"{ordinal:02d}-{mode}-400"
                cell = campaign / cell_id
                cell.mkdir()
                terminals.append(
                    {
                        "schema": "spec140.cell-terminal.v1",
                        "cellId": cell_id,
                        "mode": mode,
                        "ratePerPeer": 400,
                        "status": "COMPLETE",
                    }
                )
                for peer, values in zip(analyzer.PEERS, samples_by_mode[mode]):
                    self._write_samples(
                        cell / f"{peer}-delivery-latency.csv", list(values)
                    )
                    summary = {
                        "schema": analyzer.EXPECTED_SCHEMA,
                        "attemptedMeasured": 24000,
                        "deliveredMeasured": len(values),
                        **analyzer.distribution(values),
                    }
                    self._write_json(cell / f"{peer}-summary.json", summary)
            self._write_json(campaign / "campaign-terminals.json", terminals)

            result = analyzer.analyze(campaign)

            self.assertEqual(result["status"], "PASS")
            face = result["cells"][0]
            self.assertEqual(face["deliverySamples"], 6)
            self.assertEqual(face["deliveryP50Ns"], 3)
            self.assertEqual(face["deliveryP95Ns"], 102)

    def test_runner_contract_is_exactly_two_400_pps_cells(self) -> None:
        self.assertEqual(
            runner.MATRIX,
            (("face-inline-rsa", 400), ("worker-rsa", 400)),
        )
        self.assertEqual(runner.TIMING, (10, 60, 10))
        self.assertIn("spec140", str(runner.RESULT_ROOT))
        self.assertNotIn("spec136", str(runner.RESULT_ROOT))

    def test_runner_rejects_frozen_result_destination(self) -> None:
        frozen = runner.REPO / "results/spec136-rsa-single-worker"
        self.assertTrue(runner.is_within(frozen / "illegal", frozen))
        self.assertFalse(runner.is_within(runner.RESULT_ROOT, frozen))


if __name__ == "__main__":
    unittest.main()
