#!/usr/bin/env python3
"""One-shot and analysis invariants for Spec 146."""

from __future__ import annotations

import fcntl
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))


def load(name: str, relative: str):
    path = ROOT / relative
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


cell = load(
    "spec146_cell",
    "Experiments/NDNSF_Acoustic_Stability_Minindn.py")
analysis = load(
    "spec146_analysis",
    "Experiments/analyze_spec146_acoustic_stability.py")
matrix = load(
    "spec146_matrix",
    "Experiments/run_spec146_acoustic_stability_matrix.py")


class Spec146AcousticStabilityTest(unittest.TestCase):
    def test_matrix_has_exactly_16_unique_one_shot_acoustic_cells(self) -> None:
        cells = matrix.frozen_cells()
        self.assertEqual(len(cells), 16)
        self.assertEqual(len({value["cellId"] for value in cells}), 16)
        self.assertTrue(all(value["workload"] == "acoustic" for value in cells))
        self.assertTrue(all(value["invocations"] == 0 for value in cells))
        counts = {}
        for value in cells:
            counts[value["profile"]] = counts.get(value["profile"], 0) + 1
        self.assertEqual(
            counts, {"zero-loss": 1, "loss": 5, "reorder": 5, "combined": 5})

    def test_wrapper_rejects_wrong_workload_and_spec144_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with self.assertRaisesRegex(ValueError, "acoustic"):
                cell.build_commands(output, {}, "telemetry")
            with self.assertRaisesRegex(RuntimeError, "Spec 144"):
                cell.run(output / "spec144-forbidden", "loss")

    def test_provider_keeps_frozen_measurement_then_drains(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            commands = cell.build_commands(Path(temporary), {
                "provider": "unix:///run/nfd/provider.sock",
                "consumer": "unix:///run/nfd/consumer.sock",
            }, "acoustic")
        self.assertIn("--warmup-seconds 5 --measurement-seconds 60",
                      commands["provider"])
        self.assertIn("--post-measurement-hold-seconds 20",
                      commands["provider"])
        self.assertIn("--post-measurement-hold-seconds 5",
                      commands["consumer"])
        self.assertTrue(all(
            "NDNSF_STREAM_PACKET_TIMELINE_TRACE=0" in command
            for command in commands.values()))

    def test_manifest_rejects_duplicate_retry_and_nonfrozen_state(self) -> None:
        manifest = {
            "formalFrozen": True,
            "automaticRetry": False,
            "cells": matrix.frozen_cells(),
        }
        matrix.validate_manifest(manifest)
        manifest["cells"][1]["cellId"] = manifest["cells"][0]["cellId"]
        with self.assertRaisesRegex(ValueError, "unique"):
            matrix.validate_manifest(manifest)
        manifest["cells"] = matrix.frozen_cells()
        manifest["cells"][0]["invocations"] = 2
        with self.assertRaisesRegex(ValueError, "exceeds one"):
            matrix.validate_manifest(manifest)
        manifest["cells"] = matrix.frozen_cells()
        manifest["formalFrozen"] = False
        with self.assertRaisesRegex(ValueError, "not frozen"):
            matrix.validate_manifest(manifest)

    def test_aggregate_requires_one_and_four_of_five(self) -> None:
        rows = []
        for profile, count, accepted in (
                ("zero-loss", 1, 1), ("loss", 5, 4),
                ("reorder", 5, 5), ("combined", 5, 4)):
            rows.extend({
                "workload": "acoustic",
                "profile": profile,
                "passed": index < accepted,
            } for index in range(count))
        result = analysis.aggregate(rows)
        self.assertTrue(result["acousticStabilityVerdict"])
        rows[-5]["passed"] = False
        result = analysis.aggregate(rows)
        self.assertFalse(result["acousticStabilityVerdict"])

    def test_wrapper_reports_like_unit_recovery_ratios(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            original_run = cell.frozen.run

            def fake_run(path, workload, profile, repetition, formal=False):
                del workload, profile, repetition, formal
                path.mkdir()
                consumer = {
                    "nativeStatus": {
                        "terminalMissingSources": 2,
                        "recoveryEligibleSources": 2,
                        "recoverableGroups": 1,
                        "recovered": 2,
                        "recoveredGroups": 1,
                        "recoveryAttempts": 1,
                        "recoveryExhaustions": 0,
                    },
                }
                result = {"cellId": "acoustic-loss-r01", "passed": True,
                          "analysis": {"recovery": {}}}
                (path / "consumer-status.json").write_text(
                    json.dumps(consumer), encoding="utf-8")
                (path / "summary.json").write_text(
                    json.dumps(result), encoding="utf-8")
                return result

            cell.frozen.run = fake_run
            try:
                result = cell.run(output / "cell", "loss")
            finally:
                cell.frozen.run = original_run
            recovery = result["analysis"]["recovery"]
            self.assertEqual(recovery["sourceRecoveryRatio"]["value"], 1.0)
            self.assertEqual(recovery["groupRecoveryRatio"]["value"], 1.0)
            self.assertEqual(recovery["algorithmInvocations"], 1)

    def test_predecessor_hashes_and_freeze_scope_are_enforced(self) -> None:
        matrix.verify_predecessor()
        self.assertIn(
            "Experiments/NDNSF_Acoustic_Stability_Minindn.py",
            matrix.HASHED_INPUTS)
        self.assertIn(
            "Experiments/analyze_spec144_uav_sensor_stream.py",
            matrix.HASHED_INPUTS)
        self.assertIn(
            "build/examples/UavSensorStreamNode",
            matrix.HASHED_BINARIES)

    def test_global_single_owner_lock(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "lock"
            first = matrix.acquire_campaign_lock(path)
            try:
                with self.assertRaisesRegex(RuntimeError, "campaign owner"):
                    matrix.acquire_campaign_lock(path)
            finally:
                fcntl.flock(first, fcntl.LOCK_UN)
                first.close()


if __name__ == "__main__":
    unittest.main()
