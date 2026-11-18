"""Regression for the YOLO split client's result boundary.

A successful-looking line is not a pass: the client must compare only tensors of identical shape, and the
MiniNDN smoke must treat a non-zero exit as failure even when the expected markers were printed.

Two of these cases assert on source order rather than by executing the branch, because both live inside a
MiniNDN run that cannot be entered from a unit test; they pin that the guard comes before the comparison it
protects. The broadcast case is behavioural and shows why the guard is needed. Executing the real branch needs
the installed two-node smoke.
"""

from __future__ import annotations

import re
import types
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "examples/python/NDNSF-DistributedInference/yolo_split/user.py"
EXPERIMENT = ROOT / "Experiments/NDNSF_DI_YoloSplit_Minindn.py"


class ResultBoundaryTest(unittest.TestCase):
    def test_mismatched_shape_fails_before_any_difference(self) -> None:
        source = CLIENT.read_text(encoding="utf-8")
        # Shape equality is asserted before the subtraction, so broadcasting cannot hide a wrong tensor.
        shape_check = source.index("expected_np.shape) != tuple(actual_np.shape)")
        subtraction = source.index("diff = abs(expected_np - actual_np)")
        self.assertLess(shape_check, subtraction)
        self.assertIn("expected_shape=", source)
        self.assertIn("return 4", source)

    def test_broadcastable_but_unequal_shapes_would_differ(self) -> None:
        # Demonstrates why the guard exists: these shapes broadcast, and the difference looks small.
        expected = np.ones((1, 84, 21), dtype=np.float32)
        actual = np.ones((1, 84, 1), dtype=np.float32) * 0.5
        self.assertNotEqual(expected.shape, actual.shape)
        broadcast_max = float(np.abs(expected - actual).max())
        self.assertLess(broadcast_max, 1.0)

    def test_smoke_requires_a_zero_exit_status(self) -> None:
        source = EXPERIMENT.read_text(encoding="utf-8")
        verdict = source.index("if user_proc.returncode != 0:")
        markers = source.index('if "YOLO_SPLIT_RESULT" not in user_text')
        self.assertLess(verdict, markers)
        self.assertRegex(source, r"if user_proc\.returncode != 0:\s*\n\s*raise RuntimeError")


if __name__ == "__main__":
    unittest.main()
