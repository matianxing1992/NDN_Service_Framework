"""Infrastructure checks only; native probe correctness remains a C++ oracle."""
import ast
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import unittest
from unittest.mock import Mock, patch


SOURCE = Path(__file__).resolve().parents[1] / "Experiments/NDNSF_NetworkProbeCalibration_Minindn.py"
tree = ast.parse(SOURCE.read_text())
# Load the actual infrastructure functions without requiring a live MiniNDN.
functions = ast.Module(body=[node for node in tree.body if isinstance(node, ast.FunctionDef)
                            and node.name in ("verify_probe_fib", "stop_process", "trust_schema",
                                              "measurement_cells", "policy_services", "verify_probe_face_mtu",
                                              "parse_iperf_version", "uses_sustained_reference",
                                              "parse_iperf_udp_receiver_summary",
                                              "remaining_campaign_timeout")], type_ignores=[])
namespace = {"re": re, "math": math, "os": os, "signal": signal, "subprocess": subprocess, "time": time}
namespace["MATRIX"] = ast.literal_eval(next(node.value for node in tree.body
    if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and
    target.id == "MATRIX" for target in node.targets)))
namespace["PROBE"] = ast.literal_eval(next(node.value for node in tree.body
    if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and
    target.id == "PROBE" for target in node.targets)))
exec(compile(functions, str(SOURCE), "exec"), namespace)


class CalibrationRunnerInfrastructure(unittest.TestCase):
    def test_reference_and_matrix_share_campaign_deadline(self):
        remaining = namespace["remaining_campaign_timeout"]
        with patch.object(time, "monotonic", return_value=100):
            self.assertEqual(remaining(105, 20), 5)
            self.assertEqual(remaining(130, 20), 20)
            for deadline, limit in ((100, 20), (99, 20), (130, 0)):
                with self.assertRaises(RuntimeError):
                    remaining(deadline, limit)
        source = SOURCE.read_text()
        self.assertIn("client.wait(timeout=remaining_campaign_timeout(campaign_deadline, 20))", source)
        self.assertIn("oracle.wait(timeout=remaining_campaign_timeout(campaign_deadline, 15))", source)
        self.assertEqual(source.count("ready_deadline = time.monotonic() + remaining_campaign_timeout(campaign_deadline, 5)"), 2)

    def test_last_cell_keeps_native_budget_after_observed_campaign_elapsed(self):
        assignments = [node for node in ast.walk(tree) if isinstance(node, ast.Assign)]
        campaign = next(node for node in assignments if any(
            isinstance(target, ast.Name) and target.id == "campaign_deadline"
            for target in node.targets))
        remaining = next(node for node in assignments if any(
            isinstance(target, ast.Name) and target.id == "remaining"
            for target in node.targets) and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Name) and node.value.func.id == "min"
            and isinstance(node.value.args[0], ast.Constant) and node.value.args[0].value == 110)
        environment = {"time": time, "started": 100 + 472.6476469140034}
        code = compile(ast.Module(body=[campaign, remaining], type_ignores=[]), str(SOURCE), "exec")
        with patch.object(time, "monotonic", return_value=100):
            exec(code, environment)
        self.assertEqual(environment["campaign_deadline"], 760)
        self.assertEqual(environment["remaining"], 110)

    def test_reference_is_required_for_matrix_but_separate_from_authorization(self):
        uses = namespace["uses_sustained_reference"]
        for control in (None, "sustained-reference", "sustained-reference-burst", "default-burst"):
            self.assertTrue(uses(control, None))
            for case in ("deny", "revoke"):
                self.assertFalse(uses(control, case))
        for control in ("high-bandwidth", "aligned-face-mtu"):
            self.assertFalse(uses(control, None))

    def test_receiver_summary_ignores_sender_intervals_and_reordering_reports(self):
        parse = namespace["parse_iperf_udp_receiver_summary"]
        receiver = "[  5]  0.0-10.3 sec  4.77 MBytes  3.90 Mbits/sec   5.705 ms 32195/35600 (90%)"
        intervals = "[  5]  0.0- 1.0 sec  0.46 MBytes  3.83 Mbits/sec   4.508 ms 3203/3529 (91%)"
        sender = "[  3]  0.0-10.0 sec  49.0 MBytes  41.9 Mbits/sec"
        reordered = "[  5] 0.0000-10.2793 sec  1 datagrams received out-of-order"
        self.assertEqual(parse("\n".join((sender, intervals, receiver, reordered))),
                         {"duration_s": 10.3, "received_payload_mbps": 3.9})
        for bad in ("", sender, intervals, receiver + "\n" + receiver,
                    receiver.replace("10.3", "5.0"), receiver.replace("10.3", "21.0"),
                    receiver.replace("0.0-", "1.0-"), receiver.replace("3.90", "0.00"),
                    receiver.replace("3.90", "NaN"), receiver.replace("3.90", "9" * 400),
                    receiver.replace("3.90", "3..9")):
            with self.subTest(log=bad[:100]), self.assertRaises(RuntimeError):
                parse(bad)

    def test_iperf2_version_exit_one_requires_actual_version_text(self):
        parse = namespace["parse_iperf_version"]
        text = b"iperf version 2.0.13 (21 Jan 2019) pthreads\n"
        self.assertEqual(parse(b"", text, 1), text.decode().strip())
        self.assertEqual(parse(text, b"", 0), text.decode().strip())
        for stdout, stderr, code in ((b"", b"", 1), (b"", b"failed", 1),
                                    (text, b"", 2), (b"iperf 3.0", b"", 0)):
            with self.subTest(code=code), self.assertRaises(RuntimeError):
                parse(stdout, stderr, code)

    def test_authorization_campaign_is_bounded_and_separate(self):
        cells = namespace["measurement_cells"]
        for case in ("deny", "revoke"):
            self.assertEqual(cells(False, None, case), [(1, 10, None)])
            for smoke, control in ((True, None), (False, "default-burst"),
                                   (False, "high-bandwidth")):
                with self.assertRaises(ValueError):
                    cells(smoke, control, case)
        with self.assertRaises(ValueError):
            cells(False, None, "invalid")

    def test_policy_preserves_context_and_provider_probe_grants(self):
        policy = namespace["policy_services"]
        both = "/NDNSF/Network/Probe/v1\n/NDNSF/Calibration"
        for case in (None, "deny", "revoke"):
            self.assertEqual(policy("provider", case), both)
        self.assertEqual(policy("user", "deny"), "/NDNSF/Calibration")
        self.assertEqual(policy("user", None), both)
        self.assertEqual(policy("user", "revoke"), both)

    def test_control_campaigns_are_separate_from_accuracy_matrix(self):
        cells = namespace["measurement_cells"]
        self.assertEqual(cells(False, None), [(1, 10, "15k"), (4, 10, "15k"),
                         (10, 10, "15k"), (4, 2, "15k"), (4, 30, "15k"), (4, 70, "15k")])
        self.assertEqual(cells(True, None), [(1, 10, "15k")])
        self.assertEqual(cells(False, "high-bandwidth"), [(1000, 10, None)])
        self.assertEqual(cells(False, "default-burst"), [(4, 10, "15k")])
        self.assertEqual(cells(False, "aligned-face-mtu"), [(4, 10, None)])
        self.assertEqual(cells(False, "sustained-reference"), [(4, 10, None)])
        self.assertEqual(cells(False, "sustained-reference-burst"), [(4, 10, 2000)])
        for control in ("high-bandwidth", "default-burst", "aligned-face-mtu", "sustained-reference", "sustained-reference-burst"):
            with self.assertRaises(ValueError):
                cells(True, control)
        with self.assertRaises(ValueError):
            cells(False, "invalid")

    def test_face_mtu_readback_binds_the_original_face_and_remote(self):
        verify = namespace["verify_probe_face_mtu"]
        state = "faceid=262 remote=udp4://10.0.0.2:6363 local=udp4://10.0.0.1:6363 mtu=1472 counters={}\n"
        verify(state, 262, "udp4://10.0.0.2:6363", 1472)
        for bad in ("", state + state, state.replace("262", "263"),
                    state.replace("10.0.0.2", "10.0.0.3"), state.replace("1472", "8800"),
                    state.replace("1472", "14720")):
            with self.subTest(state=bad), self.assertRaises(RuntimeError):
                verify(bad, 262, "udp4://10.0.0.2:6363", 1472)

    def test_peer_interest_rule_is_scoped_and_file_anchored(self):
        schema = namespace["trust_schema"]("/tmp/test-controller.cert")
        self.assertNotIn("type any", schema)
        self.assertIn('trust-anchor { type file file-name "/tmp/test-controller.cert" }', schema)
        peer = next(line for line in schema.splitlines() if 'id "peer probes"' in line)
        self.assertIn("for interest", peer)
        self.assertIn("<NDNSF><RPC>", peer)
        self.assertIn("[<PROBE-RTT><PROBE-GOODPUT><PROBE-READY>]", peer)
        self.assertIn("<provider>[^<KEY>]*<KEY>", peer)

    def test_node_launch_does_not_detach_twice(self):
        calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                 and isinstance(node.func, ast.Name) and node.func.id == "getPopen"]
        self.assertEqual(len(calls), 2)
        for call in calls:
            options = {item.arg: item.value for item in call.keywords}
            self.assertTrue(ast.literal_eval(options["start_new_session"]))
            launcher = options["mncmd"]
            self.assertEqual([ast.literal_eval(part) for part in launcher.elts[:2]], ["mnexec", "-a"])
            self.assertEqual(ast.dump(launcher.elts[2]), ast.dump(ast.parse("str(node.pid)", mode="eval").body))

    def test_fib_matches_actual_nfd_text_and_exact_prefix(self):
        prefix = "/example/probe-calibration/provider/source/NDNSF/RPC/PROBE-GOODPUT"
        namespace["verify_probe_fib"]("FIB:\n  " + prefix +
                                      " nexthops={faceid=123 (cost=0)}\n", prefix, "123")
        for text in ("FIB:\n", "  " + prefix + "-OTHER nexthops={faceid=123 (cost=0)}",
                     "  " + prefix + " nexthops={faceid=124 (cost=0)}",
                     "  " + prefix + " nexthops={faceid=123 (cost=0), faceid=124 (cost=2)}"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                namespace["verify_probe_fib"](text, prefix, "123")

    def test_exit_between_poll_and_signal_is_reaped(self):
        process = Mock(pid=987654, returncode=0)
        process.poll.return_value = None
        with patch.object(os, "killpg", side_effect=ProcessLookupError):
            self.assertEqual(namespace["stop_process"](process), 0)
        process.wait.assert_called_once_with(timeout=8)

    def test_kill_race_after_term_timeout_is_reaped(self):
        process = Mock(pid=987654, returncode=-signal.SIGKILL)
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired("owned-child", 8), -signal.SIGKILL]
        with patch.object(os, "killpg", side_effect=[None, ProcessLookupError]) as kill:
            self.assertEqual(namespace["stop_process"](process), -signal.SIGKILL)
        self.assertEqual(kill.call_count, 2)
        self.assertEqual(process.wait.call_count, 2)

    def test_finished_child_is_not_signalled(self):
        process = Mock(pid=987654, returncode=0)
        process.poll.return_value = 0
        with patch.object(os, "killpg") as kill:
            self.assertEqual(namespace["stop_process"](process), 0)
        kill.assert_not_called()


if __name__ == "__main__":
    unittest.main()
