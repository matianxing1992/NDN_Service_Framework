"""Offline feedback join rejects mismatched identities and preserves overlapping boundaries."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools/ndnsf-di'
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location('feedback_measurement', TOOLS / 'analyze_feedback_measurement.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FeedbackMeasurementTests(unittest.TestCase):
    def fixture(self, root, complete_name='/p/f', done=1500000, changed_tid=False):
        native = root / 'native'
        native.mkdir()
        (root / 'outputs').mkdir()
        (root / 'outputs/turn-0.bin').write_text(json.dumps(dict(tokenIds=[1])))
        (native / 'requester-statistics.jsonl').write_text(json.dumps(dict(
            type='request', requestId='/r', turn=0)) + '\n')
        for role, epoch, timing in [('P1', 0, dict(finishedSteadyNs=1000000)),
                                    ('P0', 1, dict(startedSteadyNs=2000000))]:
            row = dict(requestId='/r', role=role, runtimeMetrics=[dict(inferenceEpoch=epoch,
                lastBackendExecution=dict(attemptEpoch=1, **timing))])
            (native / f'provider-{role}-statistics.jsonl').write_text(json.dumps(row) + '\n')
        lines = []
        for boundary, timestamp in [('ready', 1200000), ('publish_begin', 1300000),
                                     ('publish_done', done)]:
            tid = 99 if changed_tid and boundary == 'publish_done' else 7
            lines.append(f'NDNSF_DI_FEEDBACK_BOUNDARY requestId=/r attempt=1 role=P1 epoch=0 '
                         f'boundary={boundary} planned_name=/p/f manifest_name=/p/f/MANIFEST '
                         f'steady_ns={timestamp} tid={tid}')
        (native / 'provider-P1.log').write_text('\n'.join(lines) + '\n')
        (native / 'provider-P0.log').write_text('NDNSF_DI_PROVIDER_STAGE stage=DEPENDENCY_FETCH '
            f'status=complete requestId=/r attemptEpoch=1 plannedName={complete_name} '
            'steady_us=1800 role=P0\n')

    def test_exact_name_join_and_additive_partition(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            result = MODULE.analyze(root)
        row = result['intervals'][0]
        self.assertEqual(row['status'], 'ADDITIVE')
        self.assertAlmostEqual(sum(row['parts_ms'].values()), 1.0)
        self.assertEqual(row['manifest_name'], '/p/f/MANIFEST')

    def test_manifest_cannot_replace_logical_name(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, complete_name='/p/f/MANIFEST')
            with self.assertRaises(KeyError):
                MODULE.analyze(root)

    def test_worker_identity_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, changed_tid=True)
            with self.assertRaises(AssertionError):
                MODULE.analyze(root)

    def test_publish_return_after_admission_is_not_forced_additive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, done=1900000)
            result = MODULE.analyze(root)
        self.assertEqual(result['intervals'][0]['status'], 'OVERLAPPING_OR_QUANTIZED')
        self.assertEqual(result['turns'][0]['additive_samples'], 0)


if __name__ == '__main__':
    unittest.main()
