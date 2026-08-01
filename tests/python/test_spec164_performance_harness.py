import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO / "Experiments/spec164_artifact_campaign.py"
SPEC = importlib.util.spec_from_file_location("spec164_artifact_campaign", MODULE_PATH)
campaign = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = campaign
SPEC.loader.exec_module(campaign)


def complete_run(campaign_id="campaign-1", run_id="run-1"):
    values = {field: 0 for field in campaign.RUN_CSV_FIELDS}
    values.update({
        "schemaVersion": campaign.RUN_SCHEMA_VERSION,
        "campaignId": campaign_id,
        "runId": run_id,
        "cellId": "s1048576-r1-c1-raw",
        "pairId": "s1048576-r1-c1",
        "subject": "raw-segmented-ndn",
        "repetition": 0,
        "warmup": True,
        "admissible": True,
        "verdict": "PASS",
        "coldDestinationVisible": True,
        "failureReason": "",
    })
    return values


class PerformanceHarnessTests(unittest.TestCase):
    def test_frozen_matrix_has_one_raw_pair_for_each_repository_cell(self):
        cells = campaign.build_cells()
        self.assertEqual(len(cells), 4 * 2 * 3 * 4)
        raw = {cell["cellId"] for cell in cells if cell["subject"] == "raw-segmented-ndn"}
        repository = [cell for cell in cells if cell["subject"] in campaign.REPOSITORY_SUBJECTS]
        self.assertEqual(len(raw), 24)
        self.assertEqual(len(repository), 72)
        self.assertTrue(all(cell["pairedRawCellId"] in raw for cell in repository))

    def test_schedule_has_one_warmup_and_measured_matched_blocks(self):
        cells = campaign.build_cells(
            payload_sizes=(1 << 20,), replica_counts=(1,),
            concurrency_levels=(1,),
        )
        first = campaign.build_run_schedule(
            cells, repetitions=5, randomization_seed=164
        )
        second = campaign.build_run_schedule(
            cells, repetitions=5, randomization_seed=164
        )
        self.assertEqual(first, second)
        self.assertEqual(len(first), len(cells) * 6)
        self.assertEqual(sum(1 for run in first if run["warmup"]), len(cells))
        self.assertEqual(
            {run["pairId"] for run in first}, {"s1048576-r1-c1"}
        )

    def test_manifest_is_sealed_and_cannot_be_replaced(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            manifest = campaign.create_campaign_manifest(
                campaign_id="campaign-1",
                repo_root=REPO,
                topology={"kind": "linear", "bandwidthMbps": 1000},
                admissibility={"preflight": "PASS"},
                cells=campaign.build_cells(
                    payload_sizes=(1 << 20,), replica_counts=(1,),
                    concurrency_levels=(1,),
                ),
                repetitions=1,
                quick_smoke=True,
                measurement_window_seconds=1,
            )
            campaign.freeze_campaign(target, manifest)
            self.assertEqual(
                campaign.load_frozen_campaign(target)["campaignId"], "campaign-1"
            )
            with self.assertRaises(FileExistsError):
                campaign.freeze_campaign(target, manifest)
            path = target / "campaign-manifest.json"
            path.write_text(path.read_text() + " ", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                campaign.load_frozen_campaign(target)

    def test_append_only_records_reject_duplicate_run_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            manifest = campaign.create_campaign_manifest(
                campaign_id="campaign-1",
                repo_root=REPO,
                topology={},
                admissibility={},
                cells=[],
                repetitions=1,
                quick_smoke=True,
                measurement_window_seconds=1,
            )
            campaign.freeze_campaign(target, manifest)
            record = complete_run()
            campaign.append_run_record(target, record)
            with self.assertRaises(FileExistsError):
                campaign.append_run_record(target, record)
            lines = (target / "campaign-runs.jsonl").read_text().splitlines()
            self.assertEqual(len(lines), 1)
            self.assertEqual(json.loads(lines[0])["runId"], "run-1")

    def test_byte_counter_invariants_fail_closed(self):
        record = complete_run()
        record["dataWireBytes"] = 10
        record["interestWireBytes"] = 2
        record["wireBytes"] = 11
        with self.assertRaisesRegex(ValueError, "wire byte counters"):
            campaign.validate_run_record(record)

    def test_campaign_lock_is_process_level_single_writer(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            with campaign.CampaignLock(target):
                code = (
                    "import importlib.util,pathlib,sys;"
                    f"p=pathlib.Path({str(MODULE_PATH)!r});"
                    "s=importlib.util.spec_from_file_location('m',p);"
                    "m=importlib.util.module_from_spec(s);sys.modules['m']=m;"
                    "s.loader.exec_module(m);"
                    f"m.CampaignLock(pathlib.Path({str(target)!r})).__enter__()"
                )
                result = subprocess.run(
                    [sys.executable, "-c", code],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)

    def test_sampling_is_stable_and_rate_is_validated(self):
        decisions = [campaign.stable_sample(f"op-{i}", 0.1) for i in range(1000)]
        self.assertEqual(
            decisions,
            [campaign.stable_sample(f"op-{i}", 0.1) for i in range(1000)],
        )
        self.assertGreater(sum(decisions), 50)
        self.assertLess(sum(decisions), 150)
        with self.assertRaises(ValueError):
            campaign.stable_sample("op", 1.1)

    def test_formal_window_is_at_least_sixty_seconds(self):
        campaign.validate_measurement_window(60, False)
        campaign.validate_measurement_window(1, True)
        with self.assertRaises(ValueError):
            campaign.validate_measurement_window(59.99, False)

    def test_resource_sample_has_bounded_schema_fields(self):
        sample = campaign.read_proc_sample(
            os.getpid(), operation_id="op-1", phase="transfer"
        )
        schema = json.loads((
            REPO
            / "specs/164-distributed-repo-large-artifact-transport"
            / "evidence/schemas/phase-resource-sample.schema.json"
        ).read_text())
        self.assertEqual(set(sample), set(schema["required"]))
        self.assertGreater(sample["rssBytes"], 0)
        self.assertGreaterEqual(sample["peakRssBytes"], sample["rssBytes"])

    def test_current_phases_and_byte_schema_exclude_ack_reservation(self):
        self.assertNotIn("reservation", campaign.PHASES)
        self.assertIn("ackCollection", campaign.PHASES)
        self.assertIn("queueWait", campaign.PHASES)
        self.assertIn("sessionStart", campaign.PHASES)
        required = set(campaign.RUN_CSV_FIELDS)
        self.assertTrue({
            "dataWireBytes", "interestWireBytes",
            "payloadStoreBytesRead", "payloadStoreBytesWritten",
            "metadataStoreBytesRead", "metadataStoreBytesWritten",
            "coldRetrievalElapsedMs", "coldDestinationVisible",
        }.issubset(required))

    def test_iperf2_csv_parser_uses_final_three_fields(self):
        parsed = campaign.parse_iperf2_csv(
            "20260730,127.0.0.1,5001,127.0.0.1,41200,1,0.0-60.0,7500000000,1000000000"
        )
        self.assertEqual(parsed.interval_seconds, 60.0)
        self.assertEqual(parsed.transferred_bytes, 7_500_000_000)
        self.assertEqual(parsed.goodput_mbps, 1000.0)

    def test_quick_smoke_manifest_never_claims_performance(self):
        manifest = campaign.create_campaign_manifest(
            campaign_id="smoke",
            repo_root=REPO,
            topology={},
            admissibility={},
            cells=[],
            repetitions=1,
            quick_smoke=True,
            measurement_window_seconds=1,
        )
        self.assertFalse(manifest["performanceClaim"])

    def test_json_schemas_are_present_and_require_identity(self):
        schema_dir = (
            REPO
            / "specs/164-distributed-repo-large-artifact-transport"
            / "evidence/schemas"
        )
        for name in (
            "campaign-manifest.schema.json",
            "campaign-run.schema.json",
            "phase-resource-sample.schema.json",
        ):
            value = json.loads((schema_dir / name).read_text())
            self.assertEqual(value["type"], "object")
            self.assertIn("schemaVersion", value["required"])


if __name__ == "__main__":
    unittest.main()
