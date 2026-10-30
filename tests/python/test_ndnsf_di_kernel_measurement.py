"""Kernel trace pairing: ambiguity and sleep must not become fast queue/runnable samples."""
import importlib.util
from pathlib import Path
import unittest
import struct
import tempfile
import signal
import sys
import time
from types import SimpleNamespace
from unittest.mock import Mock, patch

PATH = Path(__file__).resolve().parents[2] / 'tools/ndnsf-di/analyze_kernel_measurement.py'
SPEC = importlib.util.spec_from_file_location('kernel_measurement', PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
CAPTURE_SPEC = importlib.util.spec_from_file_location('kernel_capture',
    PATH.parents[2] / 'Experiments/ndnsf_di_kernel_measurement.py')
CAPTURE = importlib.util.module_from_spec(CAPTURE_SPEC)
CAPTURE_SPEC.loader.exec_module(CAPTURE)


def net(event, timestamp, address=42):
    return dict(event='net:' + event, time_ns=timestamp,
                fields=dict(name='owned0', skbaddr=address, len=128, rc=0))


def switch(timestamp, previous, following, state):
    return dict(event='sched:sched_switch', time_ns=timestamp,
                fields=dict(prev_pid=previous, next_pid=following, prev_state=state))


class KernelMeasurementTests(unittest.TestCase):
    def test_trace_close_rejects_unowned_interrupt_or_incomplete_cleanup(self):
        row = dict(status='CLOSED', group=dict(status='ZERO', forced=False),
                   forced=False, interrupted=True, exit_code=-signal.SIGINT,
                   error=None, target_error=None)
        MODULE.validate_close(row)
        for change in [dict(interrupted=False), dict(forced=True),
                       dict(group=dict(status='UNKNOWN')), dict(error='cleanup failed'),
                       dict(target_error='target changed')]:
            with self.subTest(change=change), self.assertRaises(AssertionError):
                MODULE.validate_close(dict(row, **change))

    def test_owned_interrupt_is_expected_but_unsolicited_interrupt_fails(self):
        for requested in (True, False):
            with self.subTest(requested=requested):
                capture = CAPTURE.KernelMeasurement.__new__(CAPTURE.KernelMeasurement)
                capture.closed = False
                capture.child = Mock(pid=1234, returncode=-signal.SIGINT)
                capture.child.poll.side_effect = [None if requested else -signal.SIGINT,
                                                  -signal.SIGINT]
                capture.threads = Mock(return_value=[])
                capture.initial_threads = []
                capture.proof = dict(pid=1234)
                capture.log = Mock()
                capture.output = Path('/unused')
                capture.started = time.monotonic_ns()
                records = []
                capture.write_json = lambda path, row: records.append(row)
                processes = SimpleNamespace(stop_owned=Mock(return_value=dict(
                    status='ZERO', forced=False)))
                with patch.dict(sys.modules, {
                        'ndnsf_distributed_inference.ops.processes': processes}), \
                     patch.object(CAPTURE.os, 'killpg') as kill:
                    if requested:
                        capture.close()
                        kill.assert_called_once_with(1234, signal.SIGINT)
                    else:
                        with self.assertRaisesRegex(RuntimeError, 'KERNEL_MEASUREMENT_CLOSE_FAILED'):
                            capture.close()
                        kill.assert_not_called()
                self.assertEqual(records[0]['exit_code'], -signal.SIGINT)
                self.assertTrue(capture.closed)
                processes.stop_owned.assert_called_once_with(capture.proof)

    def test_queue_residence_excludes_driver_time(self):
        rows = [net('net_dev_queue', 1), net('net_dev_start_xmit', 5000001),
                net('net_dev_xmit', 5000101)]
        result = MODULE.pair_samples(rows, {})
        self.assertEqual(result['queue_intervals'][0]['finished_ns'], 5000001)
        self.assertEqual(result['driver_intervals'][0]['started_ns'], 5000001)
        self.assertEqual(result['counts']['xmit_rc_0'], 1)

    def test_ambiguous_requeue_is_excluded(self):
        result = MODULE.pair_samples([net('net_dev_queue', 1), net('net_dev_queue', 2),
                                      net('net_dev_start_xmit', 3)], {})
        self.assertEqual(result['queue_intervals'], [])
        self.assertEqual(result['counts']['ambiguous_requeue'], 1)

    def test_duplicate_transmit_start_is_not_a_driver_sample(self):
        result = MODULE.pair_samples([net('net_dev_start_xmit', 1),
            net('net_dev_start_xmit', 2), net('net_dev_xmit', 3)], {})
        self.assertEqual(result['driver_intervals'], [])
        self.assertEqual(result['counts']['start_without_previous_xmit_end'], 1)

    def test_perf_lost_records_are_counted(self):
        header = bytearray(72)
        header[:8] = b'PERFILE2'
        lost = struct.pack('<IHHQQ', 2, 0, 24, 99, 7)
        struct.pack_into('<QQ', header, 40, 72, len(lost))
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'lost.data'
            path.write_bytes(header + lost)
            samples, count, unknown = MODULE.read_samples(path, {})
        self.assertEqual((samples, count, unknown), ([], 7, {}))

    def test_perf_throttle_is_a_trace_gap(self):
        header = bytearray(72)
        header[:8] = b'PERFILE2'
        record = struct.pack('<IHH', 5, 0, 8)
        struct.pack_into('<QQ', header, 40, 72, len(record))
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'throttle.data'
            path.write_bytes(header + record)
            samples, count, unknown = MODULE.read_samples(path, {})
        self.assertEqual((samples, count, unknown), ([], 0, {'PERF_RECORD_THROTTLE': 1}))

    def test_sleep_is_not_runnable_wait(self):
        result = MODULE.pair_samples([switch(1, 7, 8, 1),
                                      dict(event='sched:sched_wakeup', time_ns=101,
                                           fields=dict(pid=7)), switch(106, 8, 7, 1)], {7})
        self.assertEqual(result['runnable_intervals'], [dict(tid=7, started_ns=101,
            finished_ns=106, reason='wakeup')])

    def test_preempted_runnable_and_unmatched_boundary(self):
        result = MODULE.pair_samples([switch(1, 7, 8, 256), switch(11, 8, 7, 1),
                                      switch(12, 7, 8, 0)], {7})
        self.assertEqual(result['runnable_intervals'][0]['finished_ns'], 11)
        self.assertEqual(result['counts']['unmatched_runnable_at_end'], 1)

    def test_decode_preserves_raw_pointer_and_dynamic_name(self):
        address = 0xffff888012345678
        raw = address.to_bytes(8, 'little') + ((7 << 16) | 12).to_bytes(4, 'little') + b'owned0\0'
        fields = dict(skbaddr=dict(offset=0, size=8, signed=0, dynamic=False, string=False),
                      name=dict(offset=8, size=4, signed=0, dynamic=True, string=True))
        self.assertEqual(MODULE.decode_fields(raw, fields), dict(skbaddr=address, name='owned0'))
        with self.assertRaises(AssertionError):
            MODULE.decode_fields(raw[:-1], fields)


if __name__ == '__main__':
    unittest.main()
