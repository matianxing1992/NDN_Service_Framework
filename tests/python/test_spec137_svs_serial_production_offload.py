#!/usr/bin/env python3
"""Contract tests for the Spec 137 one-binary causal comparison."""

from __future__ import annotations

import json
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
BENCHMARK = (
    REPO
    / "Experiments/ndn-svs-pubsub-benchmark/svs-serial-production-offload.cpp"
)
MEASUREMENT_PATCH = (
    REPO
    / "Experiments/ndn-svs-pubsub-benchmark/spec137-measurement.patch"
)
ALLOWED_TREATMENT_FIELDS = {
    "production_mode",
    "parallel_sync_production",
    "production_workers",
    "production_queue_capacity",
    "sign_in_worker",
    "build_extra_in_worker",
    "worker_cpu_active",
}
BUILDER = REPO / "Experiments/build_svs_serial_production_offload.py"
RUNNER = REPO / "Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py"
ANALYZER = REPO / "Experiments/analyze_svs_serial_production_offload.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec137SourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = BENCHMARK.read_text(encoding="utf-8")
        cls.patch = MEASUREMENT_PATCH.read_text(encoding="utf-8")

    def test_measurement_patch_is_reviewable_and_scoped(self):
        changed = set(
            re.findall(r"^diff --git a/(\S+) b/\S+$", self.patch, re.MULTILINE)
        )
        self.assertEqual(
            changed,
            {
                "ndn-svs/core.cpp",
                "ndn-svs/core.hpp",
                "ndn-svs/svspubsub.cpp",
                "tests/unit-tests/core.t.cpp",
                "tests/unit-tests/svspubsub.t.cpp",
            },
        )
        self.assertNotIn("diff --git a/wscript", self.patch)
        self.assertIn("setSyncProductionDiagnostics", self.patch)
        self.assertIn("SyncProductionDiagnosticsExposeQueueFallback", self.patch)
        self.assertIn(
            "ShutdownJoinsParallelProductionBeforePubSubStateDestruction",
            self.patch,
        )

    def test_same_binary_selects_only_runtime_production_location(self):
        self.assertIn('"face-serial"', self.source)
        self.assertIn('"worker-serial"', self.source)
        self.assertNotIn("SPEC137_WORKER_BINARY", self.source)
        self.assertIn("setParallelSyncProcessing(false)", self.source)
        self.assertNotIn("setParallelSyncProcessing(true", self.source)
        self.assertRegex(
            self.source,
            r"setParallelSyncProduction\(\s*true,\s*1,\s*"
            r"PRODUCTION_QUEUE_CAPACITY,\s*true,\s*true,",
        )
        self.assertIn("setSyncInterestBatching(false)", self.source)
        self.assertIn('"--diagnostics"', self.source)
        self.assertIn(
            "setSyncProductionDiagnostics(m_options.diagnostics, 100)",
            self.source,
        )
        self.assertIn("index * NS / m_options.rate", self.source)
        self.assertIn("skippedMeasuredReleases", self.source)
        self.assertIn(
            "deadline >= m_measuredStart ? 1 : 0",
            self.source,
        )

    def test_workload_and_security_are_symmetric(self):
        self.assertIn("publishAsync(name, make_span(payload))", self.source)
        self.assertIn('"--publish-enabled"', self.source)
        self.assertIn("setSigningHmacKey", self.source)
        self.assertIn("dataSigner->signingInfo.setSha256Signing()", self.source)
        self.assertIn("SvsProtocolVersion::V2", self.source)
        self.assertIn("std::vector<uint8_t> payload(256)", self.source)
        self.assertIn('subscribe(Name("/spec137/publication")', self.source)

    def test_required_unsampled_records_and_shutdown_are_present(self):
        for event in (
            "runtime-config",
            "ready",
            "phase-boundary",
            "production-terminal-anomaly",
            "signer-concurrency-max",
            "worker-stats",
            "shutdown-start",
            "shutdown-complete",
            "process-summary",
        ):
            self.assertIn(f'"{event}"', self.source)
        self.assertIn("spec137.event.v1", self.source)
        self.assertIn("core.setParallelSyncProduction(false)", self.source)
        self.assertIn("faceThread.join()", self.source)


class Spec137BuiltBinaryContractTests(unittest.TestCase):
    def test_runtime_expansion_diff_is_exactly_allowlisted(self):
        configured = os.environ.get("SPEC137_BENCH_BINARY", "")
        if not configured:
            self.skipTest("set SPEC137_BENCH_BINARY for the built-binary gate")
        binary = Path(configured)
        self.assertTrue(binary.is_file(), binary)

        configs = {}
        for mode in ("face-serial", "worker-serial"):
            completed = subprocess.run(
                [str(binary), "--self-test", mode],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            configs[mode] = json.loads(completed.stdout)

        face = configs["face-serial"]
        worker = configs["worker-serial"]
        changed = {key for key in face if face[key] != worker[key]}
        self.assertEqual(changed, ALLOWED_TREATMENT_FIELDS)
        self.assertEqual(worker["production_workers"], 1)
        self.assertEqual(face["receive_workers"], 0)
        self.assertEqual(worker["receive_workers"], 0)
        for key in ALLOWED_TREATMENT_FIELDS:
            face.pop(key)
            worker.pop(key)
        self.assertEqual(face, worker)


class Spec137HarnessContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.builder = load_module("spec137_builder", BUILDER)
        cls.runner = load_module("spec137_runner", RUNNER)
        cls.analyzer = load_module("spec137_analyzer", ANALYZER)

    def test_builder_is_exact_commit_and_patch_allowlist_bound(self):
        self.assertEqual(
            self.builder.BASE_COMMIT,
            "6bb34545b4f89f1f6c265a68c18f1a40ade413eb",
        )
        patch = MEASUREMENT_PATCH.read_text(encoding="utf-8")
        self.assertEqual(
            self.builder.validate_measurement_patch(patch),
            {
                "ndn-svs/core.cpp",
                "ndn-svs/core.hpp",
                "ndn-svs/svspubsub.cpp",
                "tests/unit-tests/core.t.cpp",
                "tests/unit-tests/svspubsub.t.cpp",
            },
        )
        with self.assertRaisesRegex(RuntimeError, "unexpected measurement patch"):
            self.builder.validate_measurement_patch(
                patch + "\ndiff --git a/wscript b/wscript\n"
            )
        identity = self.builder.canonical_boost_patch_bytes()
        self.assertIn(b"107400", identity)
        self.assertIn(b"107100", identity)

    def test_builder_rejects_hash_tamper(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "artifact"
            artifact.write_bytes(b"first")
            expected = self.builder.sha256_file(artifact)
            self.builder.verify_artifact(artifact, expected, "fixture")
            artifact.write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                self.builder.verify_artifact(artifact, expected, "fixture")

    def test_runner_parser_has_one_fail_closed_action_and_no_operator_rate(self):
        parser = self.runner.build_parser()
        parsed = parser.parse_args(["--campaign", "/tmp/c", "--preflight"])
        self.assertTrue(parsed.preflight)
        with self.assertRaises(SystemExit):
            parser.parse_args(
                ["--campaign", "/tmp/c", "--preflight", "--pilot"]
            )
        with self.assertRaises(SystemExit):
            parser.parse_args(
                ["--campaign", "/tmp/c", "--run-formal", "--formal-rate", "800"]
            )

    def test_formal_matrix_is_exact_ab_ba_ab_and_10_60_10(self):
        cells = self.runner.formal_cells(60)
        self.assertEqual(
            [(row["ordinal"], row["pair"], row["mode"]) for row in cells],
            [
                (1, 1, "face-serial"),
                (2, 1, "worker-serial"),
                (3, 2, "worker-serial"),
                (4, 2, "face-serial"),
                (5, 3, "face-serial"),
                (6, 3, "worker-serial"),
            ],
        )
        self.assertTrue(
            all(
                (row["warmup"], row["measure"], row["drain"]) == (10, 60, 10)
                for row in cells
            )
        )

    def test_receipt_ledger_rejects_second_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            ledger = self.runner.ReceiptLedger(Path(directory))
            receipt = {
                "schema": "spec137.receipt.v1",
                "ordinal": 1,
                "terminalStatus": "completed",
                "retryCount": 0,
            }
            ledger.append(receipt)
            with self.assertRaisesRegex(RuntimeError, "already has a receipt"):
                ledger.append(receipt)
            stored = json.loads((Path(directory) / "01.json").read_text())
            self.assertEqual(stored["ordinal"], 1)

    def test_rate_selector_uses_lowest_stressing_else_highest_joint(self):
        rows = [
            self.analyzer.pilot_fixture(60, face_heartbeat_p99_ns=1_200_000),
        ]
        selected = self.analyzer.select_stress_rate(rows)
        self.assertEqual(selected["selectedRate"], 60)
        self.assertEqual(selected["reason"], "FIXED_RATE_ADMITTED")
        self.assertTrue(selected["evaluated"][0]["faceStressing"])

        neutral = [
            self.analyzer.pilot_fixture(60, face_heartbeat_p99_ns=500_000),
        ]
        selected = self.analyzer.select_stress_rate(neutral)
        self.assertEqual(selected["selectedRate"], 60)
        self.assertEqual(selected["reason"], "FIXED_RATE_ADMITTED")

        rejected = [
            self.analyzer.pilot_fixture(60, attempted_error=0.10),
        ]
        selected = self.analyzer.select_stress_rate(rejected)
        self.assertIsNone(selected["selectedRate"])
        self.assertEqual(selected["reason"], "FIXED_RATE_INADMISSIBLE")

    def test_event_admission_closes_config_lifecycle_and_conservation(self):
        face = self.analyzer.synthetic_peer_evidence("face-serial")
        worker = self.analyzer.synthetic_peer_evidence("worker-serial")
        checks = self.analyzer.admit_pair(face, worker, target_rate=60)
        self.assertTrue(all(checks.values()), checks)

        worker["stats"]["fallbacks"] = 1
        next(
            event
            for event in worker["events"]
            if event["event"] == "worker-stats"
        )["details"]["fallbacks"] = 1
        checks = self.analyzer.admit_pair(face, worker, target_rate=60)
        self.assertFalse(checks["production_fallback_zero"])

        worker = self.analyzer.synthetic_peer_evidence("worker-serial")
        worker["events"].append(worker["events"][0])
        with self.assertRaisesRegex(RuntimeError, "runtime-config"):
            self.analyzer.validate_peer_evidence(worker, target_rate=60)

    def test_jsonl_parser_rejects_schema_and_identity_tamper(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            event = {
                "schema": "spec137.event.v1",
                "campaignId": "c",
                "cellId": "x",
                "peerId": "peer-a",
                "phase": "startup",
                "event": "ready",
                "monotonicNs": 1,
                "threadRole": "face",
                "logicalId": 0,
                "productionId": 0,
                "details": {},
            }
            path.write_text(json.dumps(event) + "\n", encoding="utf-8")
            self.assertEqual(
                len(self.analyzer.read_events(path, "c", "x", "peer-a")), 1
            )
            event["schema"] = "wrong"
            path.write_text(json.dumps(event) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "identity/schema"):
                self.analyzer.read_events(path, "c", "x", "peer-a")


if __name__ == "__main__":
    unittest.main()
