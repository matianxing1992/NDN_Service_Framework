#!/usr/bin/env python3
"""Generic half-window source contract for Spec 156."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
FACADE = ROOT / "ndn-service-framework/StreamFacade.cpp"


class Spec156HalfWindowTest(unittest.TestCase):
    def test_core_horizon_is_generic_half_capacity(self) -> None:
        source = FACADE.read_text(encoding="utf-8")
        begin = source.index("computePredictiveFutureCursorHorizon")
        end = source.index("} // namespace detail", begin)
        helper = source[begin:end]
        self.assertRegex(helper, r"capacity\s*/\s*2")
        self.assertNotRegex(helper, r"capacity\s*\+\s*3")
        for forbidden in (
            "uav", "video", "fps", "audio", "telemetry", "codec",
            "sampleclass", "workload",
        ):
            self.assertNotIn(forbidden, helper.lower())


if __name__ == "__main__":
    unittest.main()
