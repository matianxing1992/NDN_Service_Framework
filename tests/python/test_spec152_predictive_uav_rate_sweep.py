#!/usr/bin/env python3

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


analyzer = load(
    ROOT / "Experiments/analyze_spec152_predictive_uav_rate_sweep.py",
    "spec152_analyzer",
)
runner = load(
    ROOT / "Experiments/run_spec152_predictive_uav_rate_sweep.py",
    "spec152_runner",
)
successor = load(
    ROOT / "Experiments/run_spec153_uav_decode_stop_repair.py",
    "spec153_runner",
)
confirmation = load(
    ROOT / "Experiments/run_spec154_uav_stop_process_map.py",
    "spec154_runner",
)


class Spec152RateSweepTest(unittest.TestCase):
    def test_exact_frame_events_are_counted_only_inside_window(self):
        text = "\n".join([
            "9.9 DEBUG event=encoded-output-ready requestId=/NDNSF/UAV/VIDEO/FRAME/x",
            "10.0 DEBUG event=encoded-output-ready requestId=/NDNSF/UAV/VIDEO/FRAME/x",
            "10.5 DEBUG event=encoded-output-ready requestId=/NDNSF/STREAM/TIMELINE/x",
            "11.0 DEBUG event=encoded-output-ready requestId=/NDNSF/UAV/VIDEO/FRAME/x",
            "12.1 DEBUG event=encoded-output-ready requestId=/NDNSF/UAV/VIDEO/FRAME/x",
        ])
        self.assertEqual(
            analyzer.count_in_window(analyzer.FRAME_RE, text, 10.0, 12.0), 2
        )

    def test_matrix_has_six_ordered_rates_and_only_fps_varies(self):
        self.assertEqual(runner.RATES, (10, 20, 30, 40, 50, 60))
        with tempfile.TemporaryDirectory() as directory:
            commands = [
                runner.command_for(Path(directory) / f"fps-{fps:02d}", profile)
                for fps, profile in zip(runner.RATES, runner.ENGINE.PROFILES)
            ]
        for fps, command in zip(runner.RATES, commands):
            self.assertEqual(command[command.index("--video-fps") + 1], str(fps))
            self.assertEqual(command[command.index("--auto-stop-seconds") + 1], "80")
            self.assertEqual(
                command[command.index("--experiment-netem-loss-percent") + 1],
                "0.0",
            )

    def test_contract_does_not_reuse_frozen_output(self):
        source = (
            ROOT / "Experiments/run_spec152_predictive_uav_rate_sweep.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn(
            "results/spec151-predictive-bounded-catchup-formal-20260726T071901Z",
            source,
        )

    def test_diagnostic_rates_are_only_matrix_boundaries(self):
        self.assertEqual((10, 60), (min(runner.RATES), max(runner.RATES)))

    def test_launcher_waits_for_async_video_stopped_status(self):
        source = (
            ROOT / "Experiments/NDNSF_UAV_GUI_Minindn.py"
        ).read_text(encoding="utf-8")
        anchor = source.index('elif args.auto_video_test and args.no_cli:')
        block = source[anchor:source.index('print("NDNSF_UAV_GUI_MININDN_SMOKE_OK")', anchor)]
        self.assertIn("wait_log(", block)
        self.assertIn('"DRONE_STATUS drone=" + args.drone_id + " video stopped"', block)
        definition = source.index("drone_processes = {}")
        population = source.index("drone_processes[drone_id] = drone_proc")
        use = source.index("proc=drone_processes[args.drone_id]")
        self.assertLess(definition, population)
        self.assertLess(population, use)

    def test_successor_preserves_complete_six_rate_matrix(self):
        self.assertEqual(successor.BASE.RATES, (10, 20, 30, 40, 50, 60))
        self.assertIs(successor.BASE.ENGINE.RUNNER, successor.RUNNER)

    def test_confirmation_preserves_complete_six_rate_matrix(self):
        self.assertEqual(
            confirmation.BASE.BASE.RATES, (10, 20, 30, 40, 50, 60)
        )
        self.assertIs(
            confirmation.BASE.BASE.ENGINE.RUNNER, confirmation.RUNNER
        )


if __name__ == "__main__":
    unittest.main()
