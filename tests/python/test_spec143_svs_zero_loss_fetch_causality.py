from __future__ import annotations

import inspect
import json
from pathlib import Path
import tempfile
import unittest

from Experiments import NDN_SVS_RSA_Single_Worker_Minindn as base
from Experiments import NDN_SVS_Zero_Loss_Fetch_Causality_Minindn as runner
from Experiments import analyze_svs_zero_loss_fetch_causality as analyzer


def event(peer: str, name: str, nonce: str, kind: str, **fields):
    return {
        "peer": peer,
        "name": name,
        "nonce": nonce,
        "event": kind,
        "source": f"{peer}.trace.log",
        "sourceLine": fields.pop("sourceLine", 1),
        **fields,
    }


def summary() -> dict:
    return {
        "resourceMeasureStartSteadyNs": 100,
        "resourceMeasureEndSteadyNs": 1000,
        "resourceMeasureWallNs": 60_000_000_000,
        "resourceUserCpuUs": 12_000_000,
        "resourceSystemCpuUs": 3_000_000,
        "resourceTotalCpuUs": 15_000_000,
        "resourceCpuPctOneCore": 25.0,
        "resourceCpuPctFourCore": 6.25,
        "resourceThreadsAtMeasureStart": 12,
        "resourceThreadsAtMeasureEnd": 12,
        "resourceMaxRssKiBAtMeasureEnd": 1024,
    }


class CausalityClassificationTests(unittest.TestCase):
    def classify(self, producer_events, consumer_extra=(), name="/p/data", nonce="a1"):
        timeout = event(
            "peer-a",
            name,
            nonce,
            "fetcher_timeout",
            attempt_id=7,
            terminal_mono_ns=500,
            pending_us=500_000,
        )
        queued = event(
            "peer-a",
            name,
            nonce,
            "fetcher_queued",
            attempt_id=7,
            queue_us=20,
        )
        result = analyzer.classify_timeouts(
            {
                "peer-a": [queued, timeout, *consumer_extra],
                "peer-b": list(producer_events),
            },
            {"peer-a": summary(), "peer-b": summary()},
        )
        self.assertEqual(len(result), 1)
        return result[0]

    def test_producer_store_miss(self):
        row = self.classify(
            [event("peer-b", "/p/data", "a1", "producer_store_miss")]
        )
        self.assertEqual(row["classification"], "PRODUCER_STORE_MISS")
        self.assertEqual(row["queueToDispatchUs"], 20)

    def test_mapping_empty_is_store_miss(self):
        name = "/p/sync/MAPPING/t=1/seq=1/seq=2"
        row = self.classify(
            [event("peer-b", name, "a1", "mapping_producer_empty")],
            name=name,
        )
        self.assertEqual(row["semanticKind"], "mapping")
        self.assertEqual(row["classification"], "PRODUCER_STORE_MISS")

    def test_mapping_provider_put_overrides_overlapping_generic_store_miss(self):
        name = "/p/sync/MAPPING/t=1/seq=1/seq=2"
        row = self.classify(
            [
                event("peer-b", name, "a1", "producer_store_miss"),
                event("peer-b", name, "a1", "mapping_producer_interest"),
                event("peer-b", name, "a1", "mapping_producer_data_put"),
            ],
            name=name,
        )
        self.assertEqual(
            row["classification"], "PRODUCER_PUT_WITHOUT_CONSUMER_DATA"
        )
        self.assertNotIn(
            "producer_store_miss",
            {entry["event"] for entry in row["evidence"]},
        )

    def test_producer_hit_without_put(self):
        row = self.classify(
            [event("peer-b", "/p/data", "a1", "producer_store_hit")]
        )
        self.assertEqual(
            row["classification"], "PRODUCER_STORE_HIT_WITHOUT_PUT"
        )

    def test_producer_put_without_consumer_data(self):
        row = self.classify(
            [
                event("peer-b", "/p/data", "a1", "producer_store_hit"),
                event("peer-b", "/p/data", "a1", "producer_data_put"),
            ]
        )
        self.assertEqual(
            row["classification"], "PRODUCER_PUT_WITHOUT_CONSUMER_DATA"
        )

    def test_no_producer_observation(self):
        row = self.classify([])
        self.assertEqual(row["classification"], "NO_PRODUCER_OBSERVATION")

    def test_unclassified_when_same_attempt_has_put_and_data(self):
        row = self.classify(
            [event("peer-b", "/p/data", "a1", "producer_data_put")],
            [event("peer-a", "/p/data", "a1", "fetcher_data", attempt_id=7)],
        )
        self.assertEqual(row["classification"], "UNCLASSIFIED")

    def test_nonce_separates_retries_and_preserves_later_same_name_data(self):
        later = event(
            "peer-a",
            "/p/data",
            "b2",
            "fetcher_data",
            attempt_id=8,
            terminal_mono_ns=700,
        )
        row = self.classify(
            [
                event(
                    "peer-b",
                    "/p/data",
                    "other-nonce",
                    "producer_interest",
                )
            ],
            [later],
        )
        self.assertEqual(row["classification"], "NO_PRODUCER_OBSERVATION")
        self.assertTrue(row["laterSameNameData"])
        self.assertTrue(row["producerSameNameOtherNonce"])

    def test_timeout_outside_measurement_is_excluded(self):
        timeout = event(
            "peer-a",
            "/p/data",
            "a1",
            "fetcher_timeout",
            attempt_id=1,
            terminal_mono_ns=99,
        )
        rows = analyzer.classify_timeouts(
            {"peer-a": [timeout], "peer-b": []},
            {"peer-a": summary(), "peer-b": summary()},
        )
        self.assertEqual(rows, [])


class ResourceAndParserTests(unittest.TestCase):
    def test_resource_values_recompute(self):
        result = analyzer.validate_resource(summary(), "peer-a")
        self.assertTrue(result["valid"], result["errors"])

    def test_resource_mismatch_is_rejected(self):
        broken = summary()
        broken["resourceTotalCpuUs"] += 1
        result = analyzer.validate_resource(broken, "peer-a")
        self.assertFalse(result["valid"])

    def test_trace_parser_retains_raw_reference_and_numeric_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "peer-a.trace.log"
            path.write_text(
                "0 TRACE: [ndn_svs.Fetcher] event=fetcher_timeout "
                "name=/x nonce=01020304 attempt_id=9 "
                "terminal_mono_ns=500 pending_us=250000\n",
                encoding="utf-8",
            )
            rows = analyzer.parse_trace(path, "peer-a")
        self.assertEqual(rows[0]["event"], "fetcher_timeout")
        self.assertEqual(rows[0]["attempt_id"], 9)
        self.assertEqual(rows[0]["sourceLine"], 1)

    def test_trace_parser_marks_malformed_numeric_field(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "peer-a.trace.log"
            path.write_text(
                "0 TRACE: [ndn_svs.Fetcher] event=fetcher_timeout "
                "name=/x nonce=01020304 attempt_id=not-a-number "
                "terminal_mono_ns=500\n",
                encoding="utf-8",
            )
            rows = analyzer.parse_trace(path, "peer-a")
        self.assertTrue(rows[0]["attempt_idParseError"])
        with self.assertRaisesRegex(ValueError, "malformed numeric fields"):
            analyzer.validate_events({"peer-a": rows})

    def test_timeline_never_derives_cross_peer_duration(self):
        row = CausalityClassificationTests().classify(
            [
                event(
                    "peer-b",
                    "/p/data",
                    "a1",
                    "producer_data_put",
                    mono_ns=10**15,
                )
            ]
        )
        self.assertNotIn("crossPeerDurationUs", row)
        self.assertEqual(row["dispatchToTerminalUs"], 500_000)


class RunnerContractTests(unittest.TestCase):
    def test_diagnostic_log_is_scoped_to_four_components(self):
        self.assertIn("*=WARN", runner.NDN_LOG)
        for component in (
            "ndn_svs.Fetcher=TRACE",
            "ndn_svs.SVSyncBase=TRACE",
            "ndn_svs.MappingProvider=TRACE",
            "ndn_svs.SVSPubSub=TRACE",
        ):
            self.assertIn(component, runner.NDN_LOG)
        self.assertNotIn("SyncTimeline=TRACE", runner.NDN_LOG)

    def test_base_cell_runner_accepts_scoped_ndn_log(self):
        parameters = inspect.signature(base.run_cell).parameters
        self.assertIn("ndn_log", parameters)
        self.assertEqual(parameters["ndn_log"].default, "*=WARN")

    def test_inline_has_no_execution_stage(self):
        source = Path(runner.__file__).read_text(encoding="utf-8")
        self.assertIn('choices=("preflight", "worker-400")', source)
        self.assertNotIn('choices=("preflight", "worker-400", "inline', source)

    def test_profile_mismatch_is_rejected(self):
        errors = runner.validate_peer(
            {"schema": runner.PEER_SUMMARY_SCHEMA},
            "worker-rsa",
            400,
            60,
            "peer-a",
        )
        self.assertTrue(
            any(error.startswith("PROFILE_INVALID:") for error in errors)
        )

    def test_duplicate_campaign_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            campaign = Path(directory) / "campaign"
            runner.new_campaign(campaign)
            with self.assertRaises(FileExistsError):
                runner.new_campaign(campaign)

    def test_analyzer_zero_timeouts_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            cell = Path(directory)
            for peer in ("peer-a", "peer-b"):
                (cell / f"{peer}-summary.json").write_text(
                    json.dumps(summary()), encoding="utf-8"
                )
                (cell / f"{peer}.trace.log").write_text("", encoding="utf-8")
            result = analyzer.analyze_cell(cell)
        self.assertEqual(result["classification"]["status"], "INCONCLUSIVE")
        self.assertEqual(
            result["classification"]["classificationCoverage"], 0.0
        )


if __name__ == "__main__":
    unittest.main()
