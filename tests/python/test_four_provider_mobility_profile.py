#!/usr/bin/env python3

import importlib.util
import csv
from pathlib import Path
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
EXPERIMENTS = REPO_ROOT / "Experiments"
sys.path.insert(0, str(EXPERIMENTS))
SPEC = importlib.util.spec_from_file_location(
    "wifi_mobility_four_profile", EXPERIMENTS / "WifiRouterMobilityReliability.py")
mobility = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mobility)
PILOT_SPEC = importlib.util.spec_from_file_location(
    "single_ap_range_speed_pilot", EXPERIMENTS / "single_ap_range_speed_pilot.py")
pilot = importlib.util.module_from_spec(PILOT_SPEC)
PILOT_SPEC.loader.exec_module(pilot)


class FourProviderMobilityProfileTests(unittest.TestCase):
    def tearDown(self):
        mobility.configure_profile("three-provider")

    def test_trace_records_four_providers_nearest_ap_and_fixed_speed(self):
        mobility.configure_profile("four-provider-multi-ap", speed_mps=15.0)
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / "range_100.csv"
            metadata = mobility.generate_mobility_trace(
                trace, 100.0, seed=7, horizon_s=2.0, interval_s=1.0,
                profile="random-waypoint")
            self.assertEqual(metadata["providers"], ["ucla", "wustl", "uiuc", "arizona"])
            self.assertEqual(metadata["ap_layout"], "multi-ap")
            self.assertEqual(metadata["model"]["speed_mps"], [15.0, 15.0])
            groups = mobility.load_mobility_trace(
                trace, expected_range=100.0, expected_seed=7)
            self.assertEqual(len(groups[0][1]), 4)
            self.assertTrue(all("nearest_ap" in row for row in groups[0][1]))

    def test_mobility_ndnsf_runtime_settings_are_matched(self):
        args = mobility.make_perf_args()
        self.assertEqual(args.rate_rps, 5.0)
        self.assertFalse(args.adaptive_admission_control)
        self.assertTrue(args.disable_adaptive_admission_control)

    def test_reconnect_face_repair_only_reconfigures_selected_provider(self):
        class FakeNode:
            def __init__(self, name, ip):
                self.name = name
                self._ip = ip
                self.commands = []
                self._next_face_id = 300

            def IP(self):
                return self._ip

            def cmd(self, command):
                self.commands.append(command)
                if command.startswith("nfdc face create"):
                    face_id = self._next_face_id
                    self._next_face_id += 1
                    return f"face-created id={face_id}"
                return ""

        mobility.configure_profile("four-provider-single-ap", speed_mps=10.0)
        names = ["memphis", "ucla", "wustl", "uiuc", "arizona"]
        nodes = [FakeNode(name, f"10.0.0.{index + 1}")
                 for index, name in enumerate(names)]

        mobility.configure_ndn_multicast(
            nodes, target_names={"uiuc"}, recreate_faces=True)

        uiuc = next(node for node in nodes if node.name == "uiuc")
        self.assertEqual(
            sum("nfdc face destroy" in command for command in uiuc.commands), 4)
        self.assertEqual(
            sum("nfdc face create" in command for command in uiuc.commands), 4)
        self.assertTrue(any(
            "route add prefix /example/hello/group nexthop 300" in command
            for command in uiuc.commands))
        self.assertTrue(all(
            not node.commands for node in nodes if node.name != "uiuc"))

    def test_primary_single_ap_profile_records_one_ap_and_fixed_speed(self):
        mobility.configure_profile("four-provider-single-ap", speed_mps=2.0)
        self.assertEqual(mobility.active_ap_positions(), ((200.0, 200.0),))
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / "single_ap_range_50.csv"
            metadata = mobility.generate_mobility_trace(
                trace, 50.0, seed=40, horizon_s=2.0, interval_s=1.0,
                profile="random-waypoint")
            self.assertEqual(mobility.ACTIVE_PROFILE, "four-provider-single-ap")
            self.assertEqual(metadata["profile"], "random-waypoint")
            self.assertEqual(metadata["ap_layout"], "single")
            self.assertEqual(metadata["model"]["speed_mps"], [2.0, 2.0])
            self.assertEqual(metadata["model"]["ap_positions_m"], [[200.0, 200.0]])

    def test_random_waypoint_burn_in_is_deterministic_and_recorded(self):
        mobility.configure_profile("four-provider-single-ap", speed_mps=2.0)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cold = root / "cold.csv"
            warm_a = root / "warm-a.csv"
            warm_b = root / "warm-b.csv"
            mobility.generate_mobility_trace(
                cold, 100.0, seed=61, horizon_s=2.0, interval_s=1.0,
                profile="random-waypoint", mobility_warmup_s=0.0)
            metadata_a = mobility.generate_mobility_trace(
                warm_a, 100.0, seed=61, horizon_s=2.0, interval_s=1.0,
                profile="random-waypoint", mobility_warmup_s=300.0,
                measurement_start_s=0.0, measurement_duration_s=2.0)
            metadata_b = mobility.generate_mobility_trace(
                warm_b, 100.0, seed=61, horizon_s=2.0, interval_s=1.0,
                profile="random-waypoint", mobility_warmup_s=300.0,
                measurement_start_s=0.0, measurement_duration_s=2.0)

            self.assertEqual(metadata_a["mobility_warmup_s"], 300.0)
            self.assertEqual(metadata_a["model"]["mobility_warmup_s"], 300.0)
            self.assertEqual(metadata_a["measurement_coverage"]["epoch_count"], 2)
            self.assertEqual(
                metadata_a["measurement_coverage"]["reachable_provider_count_epochs"],
                {"1": 2})
            self.assertEqual(warm_a.read_bytes(), warm_b.read_bytes())
            self.assertNotEqual(cold.read_bytes(), warm_a.read_bytes())

    def test_pilot_propagates_burn_in_and_response_reselection(self):
        class FakeHarness:
            def __init__(self):
                self.kwargs = None

            def generate_mobility_trace(self, path, ap_range, seed, horizon,
                                        **kwargs):
                self.kwargs = kwargs
                with path.open("w", newline="") as stream:
                    writer = csv.DictWriter(
                        stream,
                        fieldnames=("time_s", "provider", "in_range"))
                    writer.writeheader()
                    for provider in ("ucla", "wustl", "uiuc", "arizona"):
                        writer.writerow({"time_s": "4.0", "provider": provider,
                                         "in_range": "1"})
                return {"mobility_warmup_s": kwargs["mobility_warmup_s"]}

        keys = ("mobility_warmup_s", "ndnsf_response_retry")
        original = {key: pilot.CONFIG.get(key) for key in keys}
        try:
            pilot.CONFIG["mobility_warmup_s"] = 300.0
            pilot.CONFIG["ndnsf_response_retry"] = True
            fake = FakeHarness()
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                trace = root / "trace.csv"
                pilot.trace_for(fake, 62, 2.0, 100.0, trace)
                ndnsf = pilot.command_for(
                    "ndnsf", 62, "campaign", trace, root / "ndnsf", 100.0, 2.0)
                grpc = pilot.command_for(
                    "grpc", 62, "campaign", trace, root / "grpc", 100.0, 2.0)

            self.assertEqual(fake.kwargs["mobility_warmup_s"], 300.0)
            self.assertEqual(fake.kwargs["measurement_start_s"], 4.0)
            self.assertEqual(fake.kwargs["measurement_duration_s"], 60.0)
            self.assertIn("--ndnsf-response-retry", ndnsf)
            self.assertNotIn("--ndnsf-response-retry", grpc)
        finally:
            for key, value in original.items():
                if value is None:
                    pilot.CONFIG.pop(key, None)
                else:
                    pilot.CONFIG[key] = value

    def test_single_active_handoff_has_exactly_one_reachable_provider(self):
        mobility.configure_profile("four-provider-multi-ap", speed_mps=15.0)
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / "handoff.csv"
            metadata = mobility.generate_mobility_trace(
                trace, 75.0, seed=9, horizon_s=4.0, interval_s=0.1,
                profile="single-active-handoff", handoff_period_s=1.0)
            self.assertEqual(metadata["profile"], "single-active-handoff")
            self.assertEqual(metadata["handoff_period_s"], 1.0)
            self.assertEqual(metadata["handoff_order"],
                             ["ucla", "wustl", "uiuc", "arizona"])
            groups = mobility.load_mobility_trace(
                trace, expected_range=75.0, expected_seed=9)
            for _, rows in groups:
                self.assertEqual(sum(int(row["in_range"]) for row in rows), 1)

    def test_single_provider_controls_use_one_target_and_disable_failover(self):
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / "trace.csv"
            command = pilot.command_for(
                "grpc-single", 40, "campaign", trace,
                Path(temporary) / "cell", 50.0, 2.0)
        self.assertIn("--systems", command)
        self.assertEqual(command[command.index("--systems") + 1], "grpc")
        self.assertIn("--provider-scope", command)
        self.assertIn("ucla", command)
        self.assertIn("--grpc-no-health-routing", command)
        self.assertIn("--lock-file", command)
        self.assertTrue(command[command.index("--lock-file") + 1].endswith(
            "/.campaign.lock"))

    def test_lower_coverage_stress_ranges_are_explicit(self):
        self.assertEqual(
            pilot.parse_registered_values(
                "35,40", pilot.CONFIG["registered_ranges_m"], "ranges"),
            [35.0, 40.0])
        self.assertEqual(
            pilot.parse_registered_values(
                "75", pilot.CONFIG["registered_ranges_m"], "ranges"),
            [75.0])
        self.assertEqual(pilot.CONFIG["ranges_m"], [50.0, 100.0])
        self.assertEqual(
            pilot.parse_registered_values(
                "", list(pilot.DEFAULT_PRIMARY_RANGES_M), "ranges"),
            [50.0, 100.0])
        self.assertEqual(
            pilot.parse_registered_values(
                "5,10", list(pilot.REGISTERED_SPEEDS_MPS), "speeds"),
            [5.0, 10.0])
        self.assertEqual(pilot.parse_seed_values("40"), [40])

    def test_timeout_sensitivity_is_paired_and_first_responding_is_explicit(self):
        keys = ("global_deadline_ms", "attempt_timeout_ms", "ack_timeout_ms",
                "timeout_condition")
        original = {key: pilot.CONFIG[key] for key in keys}
        try:
            pilot.apply_timeout_overrides(
                global_deadline_ms=5000,
                attempt_timeout_ms=2000,
                ack_timeout_ms=2000)
            self.assertEqual(pilot.CONFIG["timeout_condition"],
                             "sensitivity-2000ms-attempt-5000ms-global-2000ms-ack")
            self.assertIn("FirstResponding publishes selection immediately",
                          pilot.CONFIG["ndnsf_first_responding_semantics"])
            with tempfile.TemporaryDirectory() as temporary:
                command = pilot.command_for(
                    "grpc", 40, "campaign", Path(temporary) / "trace.csv",
                    Path(temporary) / "cell", 50.0, 2.0)
            self.assertEqual(command[command.index("--attempt-timeout-ms") + 1], "2000")
            self.assertEqual(command[command.index("--ack-timeout-ms") + 1], "2000")
            self.assertEqual(command[command.index("--timeout-ms") + 1], "5000")
        finally:
            pilot.CONFIG.update(original)

    def test_trace_coverage_metrics_use_the_registered_measurement_window(self):
        rows = [
            {"time_s": "0.0", "in_range": "0"},
            {"time_s": "0.0", "in_range": "0"},
            {"time_s": "4.0", "in_range": "1"},
            {"time_s": "4.0", "in_range": "0"},
            {"time_s": "5.0", "in_range": "0"},
            {"time_s": "5.0", "in_range": "0"},
            {"time_s": "6.0", "in_range": "1"},
            {"time_s": "6.0", "in_range": "1"},
            {"time_s": "7.0", "in_range": "0"},
            {"time_s": "7.0", "in_range": "0"},
        ]
        whole = pilot.coverage_metrics(rows)
        measured = pilot.coverage_metrics(rows, start_s=4.0, duration_s=3.0)

        self.assertEqual(whole["epochs"], 5)
        self.assertAlmostEqual(whole["all_unreachable_fraction"], 0.6)
        self.assertEqual(measured["epochs"], 3)
        self.assertAlmostEqual(measured["all_unreachable_fraction"], 1.0 / 3.0)
        self.assertAlmostEqual(measured["at_least_two_fraction"], 1.0 / 3.0)

    def test_traffic_phase_gate_rejects_the_old_two_second_ndnsf_offset(self):
        self.assertTrue(pilot.traffic_phase_matches({"traffic_launch_offset_s": 4.01}))
        self.assertFalse(pilot.traffic_phase_matches({"traffic_launch_offset_s": 2.0}))

    def test_all_holdout_system_commands_use_the_same_four_second_phase(self):
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / "trace.csv"
            for system in ("ndnsf", "grpc", "grpc-single"):
                command = pilot.command_for(
                    system, 43, "holdout", trace,
                    Path(temporary) / system, 50.0, 2.0)
                self.assertEqual(
                    command[command.index("--traffic-start-delay-s") + 1], "4.0")


if __name__ == "__main__":
    unittest.main()
