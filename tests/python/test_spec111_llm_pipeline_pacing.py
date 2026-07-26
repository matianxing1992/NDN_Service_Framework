from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_DIR = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"
USER = PIPELINE_DIR / "user.py"


def load_user_module():
    sys.path.insert(0, str(PIPELINE_DIR))
    spec = importlib.util.spec_from_file_location("spec111_llm_pipeline_user", USER)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load LLM pipeline user")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FixedRatePacingTest(unittest.TestCase):
    def test_sixty_second_one_hz_window_has_exactly_sixty_start_slots(self):
        module = load_user_module()
        started = 100.0
        deadline = started + 60.0
        slots = [
            module._fixed_rate_slot_time(started, index, 1.0, deadline)
            for index in range(61)
        ]
        self.assertEqual(slots[:60], [started + index for index in range(60)])
        self.assertIsNone(slots[60])

    def test_request_latency_does_not_shift_the_next_start_slot(self):
        module = load_user_module()
        started = 100.0
        self.assertEqual(
            module._fixed_rate_slot_time(started, 7, 1.0, started + 60.0),
            107.0,
        )


if __name__ == "__main__":
    unittest.main()
