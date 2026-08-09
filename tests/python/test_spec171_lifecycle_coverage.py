#!/usr/bin/env python3

import csv
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
EXPERIMENTS = REPO_ROOT / "Experiments"
sys.path.insert(0, str(EXPERIMENTS))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mobility = load_module(
    "spec171_mobility", EXPERIMENTS / "WifiRouterMobilityReliability.py")
analyzer = load_module(
    "spec171_lifecycle", EXPERIMENTS / "analyze_ndnsf_mobility_lifecycle.py")


class FakeNode:
    def __init__(self, name):
        self.name = name


class FakeNdn:
    def __init__(self, names):
        self.net = {name: FakeNode(name) for name in names}


class Spec171LifecycleCoverageTests(unittest.TestCase):
    def tearDown(self):
        mobility.configure_profile("three-provider")

    def test_replay_records_actual_gate_application_timestamps(self):
        mobility.configure_profile("four-provider-single-ap", speed_mps=10.0)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace = root / "trace.csv"
            trace.write_text(
                "time_s,provider,x,y,distance_m,in_range,nearest_ap\n" +
                "".join(
                    f"0.000,{name},200,200,0,1,ap1\n"
                    for name in mobility.provider_nodes()) +
                "".join(
                    f"0.010,{name},200,200,0,{int(name != 'wustl')},ap1\n"
                    for name in mobility.provider_nodes()))
            stop = threading.Event()
            thread, output, _ = mobility.start_coverage_gate(
                FakeNdn(mobility.provider_nodes()), root / "run", 100.0,
                stop, seed=61, block_network=False,
                trace_replay_path=trace,
                epoch_monotonic=time.monotonic() + 0.01)
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
            with output.open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 8)
            self.assertTrue(all(float(row["applied_unix_s"]) > 0 for row in rows))
            self.assertTrue(all(float(row["applied_monotonic_s"]) > 0 for row in rows))
            self.assertLessEqual(
                float(rows[0]["applied_monotonic_s"]),
                float(rows[-1]["applied_monotonic_s"]))
            replay = mobility.load_mobility_trace(
                output, expected_range=100.0, expected_seed=61)
            self.assertEqual(len(replay), 2)

    def test_analyzer_prefers_actual_gate_state_over_scheduled_epoch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "summary.json").write_text(json.dumps({
                "summaries": [{"traffic_launch_offset_s": 4.0}],
            }))
            (root / "runtime-commands.json").write_text("{}\n")
            with (root / "mobility_trace.csv").open("w", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerow((
                    "time_s", "provider", "x", "y", "distance_m", "in_range",
                    "applied_unix_s", "applied_monotonic_s"))
                for name in analyzer.PROVIDER_NODE_TO_LABEL:
                    writer.writerow((0, name, 0, 0, 0, 1, 1000.0, 10.0))
                for name in analyzer.PROVIDER_NODE_TO_LABEL:
                    writer.writerow((
                        1, name, 0, 0, 0, int(name != "wustl"), 1001.4, 11.4))
            request_id = "/request-1"
            (root / "ndnsf-user.log").write_text(
                "[NDNSF_TRACE] role=user event=REQUEST_PUBLISHED "
                f"timestamp_us=1001200000 requestId={request_id}\n"
                "[NDNSF_TRACE] role=user event=TIMEOUT_FIRED "
                f"timestamp_us=1006200000 requestId={request_id}\n")
            report = analyzer.analyze(root)
            with (root / "request-lifecycle-coverage.csv").open() as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["coverage_basis"], "actual_gate_application")
            # The scheduled t=1 state says B is down, but it was not actually
            # applied until 200 ms after this Request publication.
            self.assertEqual(row["coverage_state"], "A=1|B=1|C=1|D=1")
            self.assertTrue(report["coverage_alignment"]["uses_actual_gate_timestamps"])

    def test_analyzer_distinguishes_svs_publish_begin_from_completion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "summary.json").write_text(json.dumps({
                "summaries": [{"traffic_launch_offset_s": 0.0}],
            }))
            (root / "runtime-commands.json").write_text("{}\n")
            (root / "mobility_trace.csv").write_text(
                "time_s,provider,x,y,distance_m,in_range,"
                "applied_unix_s,applied_monotonic_s\n" +
                "".join(
                    f"0,{name},0,0,0,1,1.0,1.0\n"
                    for name in analyzer.PROVIDER_NODE_TO_LABEL))
            request_id = "/request-1"
            (root / "ndnsf-user.log").write_text(
                "[NDNSF_TRACE] role=user event=REQUEST_PUBLISHED "
                f"timestamp_us=1000000 requestId={request_id}\n"
                "[NDNSF_TRACE] role=user event=TIMEOUT_FIRED "
                f"timestamp_us=6000000 requestId={request_id}\n")
            (root / "ndnsf-provider-A.log").write_text(
                "[NDNSF_TRACE] role=provider event=ACK_PUBLISHED "
                f"timestamp_us=1005000 requestId={request_id} providerName=/provider/A\n"
                "[NDNSF_TRACE] role=provider event=SVS_PUBLISH_BEGIN "
                "timestamp_us=1010000 providerName=/provider/A "
                "messageName=/provider/A/NDNSF/ACK/user/service/request-1\n"
                "[NDNSF_TRACE] role=provider event=SVS_PUBLISH_DONE "
                "timestamp_us=1015000 providerName=/provider/A "
                "messageName=/provider/A/NDNSF/ACK/user/service/request-1 "
                "seqNo=42 mode=hybrid-message-crypto\n")

            report = analyzer.analyze(root)
            with (root / "request-lifecycle-coverage.csv").open() as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["ack_publish_started_delta_ms"], "10.000")
            self.assertEqual(row["ack_published_delta_ms"], "15.000")
            self.assertEqual(report["ack_published_evidence"]["done"], 1)
            self.assertEqual(report["ack_published_evidence"]["begin_fallback"], 0)

    def test_analyzer_records_response_reselection_and_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "summary.json").write_text(json.dumps({
                "summaries": [{"traffic_launch_offset_s": 0.0}],
            }))
            (root / "runtime-commands.json").write_text("{}\n")
            (root / "mobility_trace.csv").write_text(
                "time_s,provider,x,y,distance_m,in_range,"
                "applied_unix_s,applied_monotonic_s\n" +
                "".join(
                    f"0,{name},0,0,0,1,1.0,1.0\n"
                    for name in analyzer.PROVIDER_NODE_TO_LABEL))
            request_id = "/request-reselected"
            (root / "ndnsf-user.log").write_text(
                "[NDNSF_TRACE] role=user event=REQUEST_PUBLISHED "
                f"timestamp_us=1000000 requestId={request_id}\n"
                "[NDNSF_TRACE] role=user event=PROVIDER_SELECTED "
                f"timestamp_us=1010000 requestId={request_id} "
                "selectedProvider=/provider/A\n"
                "[NDNSF_TRACE] role=user event=RESPONSE_ATTEMPT_STARTED "
                f"timestamp_us=1011000 requestId={request_id} "
                "providerName=/provider/A attempt=1\n"
                "[NDNSF_TRACE] role=user event=RESPONSE_RETRY_CANDIDATE_STORED "
                f"timestamp_us=1020000 requestId={request_id} "
                "providerName=/provider/B\n"
                "[NDNSF_TRACE] role=user event=RESPONSE_ATTEMPT_TIMEOUT "
                f"timestamp_us=2011000 requestId={request_id} "
                "providerName=/provider/A attempt=1\n"
                "[NDNSF_TRACE] role=user event=RESPONSE_RESELECTION "
                f"timestamp_us=2012000 requestId={request_id} "
                "providerName=/provider/B nextAttempt=2\n"
                "[NDNSF_TRACE] role=user event=RESPONSE_OBSERVED "
                f"timestamp_us=2079000 requestId={request_id} "
                "providerName=/provider/B\n"
                "[NDNSF_TRACE] role=user event=CALLBACK_FIRED "
                f"timestamp_us=2080000 requestId={request_id}\n")

            report = analyzer.analyze(root)
            self.assertEqual(report["response_retry"]["total_reselections"], 1)
            self.assertEqual(
                report["response_retry"]["successful_after_reselection"], 1)
            self.assertEqual(report["response_retry"]["timed_out_after_reselection"], 0)
            with (root / "request-lifecycle-coverage.csv").open() as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["response_reselection_providers"], "B")
            self.assertEqual(row["response_observed_delta_ms"], "1079.000")


if __name__ == "__main__":
    unittest.main()
