#!/usr/bin/env python3
"""One-shot runner invariants for Spec 144."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def load(name: str, relative: str):
    path = ROOT / relative
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


cell_runner = load(
    "spec144_cell",
    "Experiments/NDNSF_UAV_Sensor_Stream_Generality_Minindn.py")
matrix = load(
    "spec144_matrix",
    "Experiments/run_spec144_uav_sensor_stream_matrix.py")


class Spec144RunnerTest(unittest.TestCase):
    def test_matrix_has_exactly_32_unique_one_shot_cells(self) -> None:
        cells = matrix.frozen_cells()
        self.assertEqual(len(cells), 32)
        self.assertEqual(len({cell["cellId"] for cell in cells}), 32)
        self.assertTrue(all(cell["invocations"] == 0 for cell in cells))
        self.assertTrue(all(cell["terminal"] is False for cell in cells))
        counts = {}
        for cell in cells:
            counts[(cell["workload"], cell["profile"])] = \
                counts.get((cell["workload"], cell["profile"]), 0) + 1
        for workload in ("telemetry", "acoustic"):
            self.assertEqual(counts[(workload, "zero-loss")], 1)
            self.assertEqual(counts[(workload, "loss")], 5)
            self.assertEqual(counts[(workload, "reorder")], 5)
            self.assertEqual(counts[(workload, "combined")], 5)

    def test_netem_profiles_are_exact_and_bidirectionally_applicable(self) -> None:
        reorder = cell_runner.netem_command("provider-eth0", "reorder")
        self.assertEqual(
            reorder,
            "tc qdisc replace dev provider-eth0 root netem delay 20ms "
            "10ms distribution normal reorder 25% 50% gap 5")
        combined = cell_runner.netem_command("consumer-eth0", "combined")
        self.assertIn("loss 1%", combined)
        self.assertIn("reorder 25% 50% gap 5", combined)
        effective = json.dumps([{
            "kind": "netem", "root": True,
            "options": {
                "delay": {"delay": 0.02, "jitter": 0.01},
                "loss-random": {"loss": 0.01},
                "reorder": {"reorder": 0.25, "correlation": 0.5},
                "gap": 5,
            },
        }])
        self.assertTrue(cell_runner.qdisc_matches_profile(effective, "combined"))
        self.assertFalse(cell_runner.qdisc_matches_profile(effective, "reorder"))
        self.assertFalse(cell_runner.qdisc_matches_profile("[]", "combined"))

    def test_commands_use_real_cpp_uav_app_and_60_second_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            commands = cell_runner.build_commands(output, {
                "provider": "unix:///run/nfd/provider.sock",
                "consumer": "unix:///run/nfd/consumer.sock",
            }, "telemetry")
            self.assertIn("build/examples/UavSensorStreamNode", commands["provider"])
            self.assertIn("--role provider", commands["provider"])
            self.assertIn("--role consumer", commands["consumer"])
            self.assertNotIn("workload_provider.py", json.dumps(commands))
            plan = cell_runner.planned_cell(
                output, "telemetry", "zero-loss", 1, formal=True)
            self.assertEqual(plan["measurementSeconds"], 60)
            self.assertEqual(plan["expectedMeasured"], 1200)
            self.assertFalse(plan["automaticRetry"])
            self.assertIn("--warmup-seconds 5 --measurement-seconds 60",
                          commands["provider"])
            self.assertIn("--warmup-seconds 5 --measurement-seconds 60",
                          commands["consumer"])

    def test_reused_destination_is_rejected_before_minindn_import(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "existing").write_text("immutable", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "reused"):
                cell_runner.run(output, "telemetry", "zero-loss")

    def test_manifest_rejects_duplicate_ids_retry_and_mutation(self) -> None:
        cells = matrix.frozen_cells()
        manifest = {
            "formalFrozen": True, "cells": cells,
            "automaticRetry": False,
        }
        matrix.validate_manifest(manifest)
        cells[1]["cellId"] = cells[0]["cellId"]
        with self.assertRaisesRegex(ValueError, "unique"):
            matrix.validate_manifest(manifest)
        manifest["cells"] = matrix.frozen_cells()
        manifest["cells"][0]["invocations"] = 2
        with self.assertRaisesRegex(ValueError, "exceeds one"):
            matrix.validate_manifest(manifest)
        manifest["cells"] = matrix.frozen_cells()
        manifest["automaticRetry"] = True
        with self.assertRaisesRegex(ValueError, "retry"):
            matrix.validate_manifest(manifest)

    def test_frozen_hash_drift_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "subject"
            path.write_text("a", encoding="utf-8")
            manifest = {"inputs": {str(path): matrix.sha256(path)}}
            original = matrix.REPO
            try:
                matrix.REPO = Path("/")
                matrix.verify_frozen(manifest)
                path.write_text("b", encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "drift"):
                    matrix.verify_frozen(manifest)
            finally:
                matrix.REPO = original

    def test_freeze_scope_includes_runtime_and_security_subjects(self) -> None:
        self.assertIn("NDNSF-UAV-APP/configs/uav_demo.policies",
                      matrix.HASHED_INPUTS)
        self.assertIn("examples/trust-schema.conf", matrix.HASHED_INPUTS)
        self.assertIn("build/libndn-service-framework.so",
                      matrix.HASHED_BINARIES)
        identity = matrix.environment_identity()
        for key in ("compiler", "boostHeader", "ndnCxx", "nfd", "python",
                    "tc", "miniNdn", "kernel"):
            self.assertIn(key, identity)

    def test_campaign_owner_lock_is_global_across_output_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            lock_path = Path(temporary) / "global.lock"
            first = matrix.acquire_campaign_lock(lock_path)
            try:
                with self.assertRaisesRegex(RuntimeError, "campaign owner"):
                    matrix.acquire_campaign_lock(lock_path)
            finally:
                import fcntl
                fcntl.flock(first, fcntl.LOCK_UN)
                first.close()


if __name__ == "__main__":
    unittest.main()
