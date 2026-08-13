import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "Experiments/generate_spec171_provider_transition_trace.py"
SPEC = importlib.util.spec_from_file_location("spec171_transition", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171ProviderTransitionTraceTest(unittest.TestCase):
    def test_three_registered_phases(self):
        before = MODULE.availability(19.9, 20.0, 40.0)
        overlap = MODULE.availability(20.0, 20.0, 40.0)
        after = MODULE.availability(40.0, 20.0, 40.0)
        self.assertEqual(sum(before.values()), 3)
        self.assertFalse(before["arizona"])
        self.assertEqual(sum(overlap.values()), 4)
        self.assertEqual(sum(after.values()), 1)
        self.assertTrue(after["arizona"])

    def test_generator_emits_four_providers_per_epoch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            rows = MODULE.generate(
                path, duration_s=1.0, step_s=0.1, join_s=0.2, retire_s=0.8)
            with path.open() as stream:
                data = list(csv.DictReader(stream))
            self.assertEqual(rows, 44)
            self.assertEqual(len(data), 44)
            self.assertEqual(
                {row["provider"] for row in data}, set(MODULE.PROVIDERS))


if __name__ == "__main__":
    unittest.main()
