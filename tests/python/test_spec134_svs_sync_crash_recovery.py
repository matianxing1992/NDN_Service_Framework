from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
BUILDER_PATH = REPO / "Experiments/build_svs_sync_crash_recovery.py"
RUNNER_PATH = REPO / "Experiments/NDN_SVS_Sync_Crash_Recovery_Minindn.py"
DRIVER_PATH = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-crash-recovery.cpp"
IO_DRIVER_PATH = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-io-qualification.cpp"


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec134BuilderContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.builder = load(BUILDER_PATH, "spec134_builder")

    def test_exact_frozen_identity_and_separate_sanitizers(self):
        self.assertEqual(self.builder.BASE_COMMIT,
                         "a9944019f76791773604999f00128057b9534ace")
        self.assertEqual(self.builder.CLEAN_HEAD,
                         "bf1e3e37f0c4c7a5a04d678f0fa439283ee46d2d")
        self.assertEqual(set(self.builder.MODES), {"asan-ubsan", "tsan"})
        self.assertIn("address,undefined", self.builder.MODES["asan-ubsan"])
        self.assertNotIn("thread", self.builder.MODES["asan-ubsan"])
        self.assertIn("sanitize=thread", self.builder.MODES["tsan"])
        self.assertEqual(self.builder.MODE_COMPILERS,
                         {"asan-ubsan": "g++", "tsan": "clang++"})

    def test_boost_linkage_rejects_missing_or_mixed_versions(self):
        self.builder.verify_boost_171(
            "libboost_log.so.1.71.0 => /lib/libboost_log.so.1.71.0\n"
        )
        with self.assertRaisesRegex(RuntimeError, "not exclusively 1.71"):
            self.builder.verify_boost_171("")
        with self.assertRaisesRegex(RuntimeError, "not exclusively 1.71"):
            self.builder.verify_boost_171(
                "libboost_log.so.1.74.0 => /lib/libboost_log.so.1.74.0\n"
            )

    def test_builder_is_detached_isolated_and_frozen_at_j2(self):
        source = BUILDER_PATH.read_text(encoding="utf-8")
        self.assertIn('"worktree", "add", "--detach"', source)
        self.assertIn('"waf", "build", "-j2"', source)
        self.assertIn("protected_snapshot", source)
        self.assertNotIn("checkout -f", source)
        self.assertNotIn("reset --hard", source)

    def test_repair_artifacts_cannot_overwrite_baseline_artifacts(self):
        source = BUILDER_PATH.read_text(encoding="utf-8")
        self.assertIn('build_variant(output, f"repaired-{mode}"', source)
        self.assertNotIn('build_variant(output, mode, f"repaired-{mode}"', source)

    def test_io_qualification_uses_clean_subject_and_distinct_artifacts(self):
        source = BUILDER_PATH.read_text(encoding="utf-8")
        self.assertIn("IO_DRIVER", source)
        self.assertIn("build_io_qualification", source)
        self.assertIn('"spec134-io-qualification-manifest-v1"', source)
        self.assertIn('"io-qualification-normal"', source)
        self.assertIn("CLEAN_HEAD", source)


class Spec134RunnerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load(RUNNER_PATH, "spec134_runner")

    def test_diagnostic_and_qualification_timing_are_distinct(self):
        self.assertEqual(self.runner.DIAGNOSTIC["measureSeconds"], 5)
        self.assertEqual(self.runner.QUALIFICATION, {
            "ratePpsPerPeer": 1000,
            "payloadBytes": 256,
            "warmupSeconds": 10,
            "measureSeconds": 60,
            "drainSeconds": 10,
        })

    def test_existing_attempt_path_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "attempt path already exists"):
                self.runner.run_once(Path("missing"), output, "tsan",
                                     dict(self.runner.DIAGNOSTIC), "diagnostic")

    def test_process_stop_is_required_for_complete_event_flush(self):
        with tempfile.TemporaryDirectory() as directory:
            events = Path(directory) / "events.jsonl"
            events.write_text(json.dumps({
                "event": "heartbeat", "details": {"attemptedMeasured": 3}
            }) + "\n", encoding="utf-8")
            complete, counters = self.runner.final_counters(events)
            self.assertFalse(complete)
            self.assertEqual(counters, {})
            events.write_text(json.dumps({
                "event": "process-stop",
                "details": {"attemptedMeasured": 10, "deliveredMeasured": 9,
                            "invalidRemoteMeasured": 0, "publishErrors": 0,
                            "scheduledMeasured": 10, "missedReleaseMeasured": 0,
                            "localDeliveryIgnored": 1},
            }) + "\n", encoding="utf-8")
            complete, counters = self.runner.final_counters(events)
            self.assertTrue(complete)
            self.assertEqual(counters["attemptedMeasured"], 10)

    def test_corruption_scan_covers_sanitizers_and_allocator_symptoms(self):
        with tempfile.TemporaryDirectory() as directory:
            cell = Path(directory)
            (cell / "peer-a.stderr").write_text(
                "ERROR: AddressSanitizer: heap-use-after-free\n", encoding="utf-8")
            findings = self.runner.scan_corruption(cell)
            self.assertTrue(findings)
            self.assertIn("AddressSanitizer", findings[0]["match"])

    def test_runner_has_no_automatic_retry_or_five_rate_campaign(self):
        source = RUNNER_PATH.read_text(encoding="utf-8")
        self.assertIn('"automaticRetry": False', source)
        self.assertNotIn("RATES = (200, 400, 600, 800, 1000)", source)
        self.assertNotIn("for attempt in", source)

    def test_io_qualification_command_and_clean_schema_exist(self):
        source = RUNNER_PATH.read_text(encoding="utf-8")
        self.assertIn("qualify-io", source)
        self.assertIn("spec134-io-qualification-manifest-v1", source)
        self.assertIn('"io-qualification-normal"', source)
        self.assertIn("missedReleaseMeasured", source)
        self.assertIn("localDeliveryIgnored", source)


class Spec134DriverContractTests(unittest.TestCase):
    def test_frozen_diagnostic_driver_keeps_original_cross_thread_model(self):
        source = DRIVER_PATH.read_text(encoding="utf-8")
        self.assertIn("m_pubsub->publish(name, make_span(payload))", source)
        self.assertIn("std::thread faceThread", source)
        self.assertIn("m_face.processEvents", source)
        self.assertNotIn("publishAsync", source)
        self.assertNotIn("boost::asio::post", source)

    def test_driver_uses_crash_safe_low_volume_journal(self):
        source = DRIVER_PATH.read_text(encoding="utf-8")
        self.assertIn("m_output.flush()", source)
        self.assertIn('record("heartbeat"', source)
        self.assertIn('record("process-stop"', source)
        self.assertNotIn('record("api-return"', source)

    def test_io_driver_uses_one_face_io_thread_for_publish_and_receive(self):
        source = IO_DRIVER_PATH.read_text(encoding="utf-8")
        self.assertIn("boost::asio::steady_timer", source)
        self.assertIn("m_face.processEvents", source)
        self.assertIn("m_pubsub->publish(name, make_span(payload))", source)
        self.assertNotIn("std::thread", source)
        self.assertNotIn("publishAsync", source)
        self.assertNotIn("boost::asio::post", source)
        self.assertNotIn("sleep_until", source)

    def test_io_driver_accounts_slots_and_excludes_local_delivery(self):
        source = IO_DRIVER_PATH.read_text(encoding="utf-8")
        self.assertIn("scheduledMeasured", source)
        self.assertIn("missedReleaseMeasured", source)
        self.assertIn("localDeliveryIgnored", source)
        self.assertIn("invalidRemoteMeasured", source)
        self.assertIn("m_ownSenderHash", source)
        self.assertIn("m_remoteSenderHash", source)
        self.assertIn("armPublication", source)


if __name__ == "__main__":
    unittest.main()
