"""Check timing attribution identity and missing-data handling, not native behavior."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'tools/ndnsf-di/analyze_dispatch_measurement.py'
SPEC = importlib.util.spec_from_file_location('dispatch_analysis', SOURCE)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DispatchAnalysis(unittest.TestCase):
    def analyze(self, name='/8=p/8=obj/8=MANIFEST', started=3000, mode='outer-dispatch'):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'native').mkdir()
            (root / 'capture').mkdir()
            (root / 'analysis.json').write_text(json.dumps(dict(objects=[dict(
                turn=1, producer_log='provider-0.log', planned_name='/p/obj',
                packed_publication_payload_bytes=4900, ready_to_admitted_ms=2)])))
            (root / 'packet-analysis.json').write_text(json.dumps(dict(objects=[dict(
                planned_name='/p/obj', observed_data_packets=1, first_data_epoch_ms=2,
                last_data_to_admitted_ms=1)])))
            (root / 'capture/capture-start.json').write_text(json.dumps(dict(
                ready_epoch_ns=1000000, ready_steady_ns=0)))
            (root / 'native/provider-0.log').write_text(
                'NDNSF_EXACT_DATA_DISPATCH mode=' + mode + ' name=' + name +
                ' queued_steady_ns=1000 started_steady_ns=' + str(started) +
                ' put_started_steady_ns=4000 put_returned_steady_ns=6000\n')
            return MODULE.analyze(root)

    def test_component_equivalent_name_and_independent_queue_clock(self):
        result = self.analyze()
        self.assertEqual(result['counts']['matched_object'], 1)
        self.assertEqual(result['missing'], [])
        self.assertAlmostEqual(result['objects'][0]['queued_to_callback_ms'], .002)
        self.assertAlmostEqual(result['objects'][0]['put_call_ms'], .002)
        self.assertAlmostEqual(result['objects'][0]['put_return_to_capture_ms'], .994)

    def test_string_prefix_collision_stays_missing(self):
        result = self.analyze(name='/p/object/MANIFEST')
        self.assertEqual(result['objects'], [])
        self.assertEqual(result['missing'][0]['planned_name'], '/p/obj')

    def test_reversed_core_clocks_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'not monotonic'):
            self.analyze(started=500)

    def test_removed_callback_stage_is_not_reported_as_measured_zero(self):
        result = self.analyze(mode='native-enqueue', started=1000)
        self.assertIsNone(result['objects'][0]['queued_to_callback_ms'])
        self.assertEqual(result['turns'][0]['queued_to_callback_ms']['status'], 'NOT_APPLICABLE')


if __name__ == '__main__':
    unittest.main()
