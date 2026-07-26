from __future__ import annotations

from pathlib import Path
import sys
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from Experiments import NDN_SVS_Latency_Rate_Extension_Minindn as runner
from Experiments import analyze_svs_latency_rate_extension as analyzer


class Spec141ContractTests(unittest.TestCase):
    def test_exact_rate_mode_order(self) -> None:
        self.assertEqual(
            runner.MATRIX,
            (
                ("face-inline-rsa", 600),
                ("worker-rsa", 600),
                ("face-inline-rsa", 800),
                ("worker-rsa", 800),
            ),
        )

    def test_timing_is_identical_to_spec140(self) -> None:
        self.assertEqual(runner.TIMING, (10, 60, 10))
        self.assertEqual(runner.TIMING, runner.spec140.TIMING)

    def test_binary_is_identical_to_spec140(self) -> None:
        self.assertEqual(runner.BINARY, runner.spec140.DEFAULT_BINARY)
        self.assertEqual(
            runner.EXPECTED_BINARY_SHA256,
            "a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a",
        )

    def test_negative_load_terminal_is_retained(self) -> None:
        self.assertEqual(
            runner.ACCEPTED_TERMINALS, {"COMPLETE", "LOAD_UNSUSTAINED"}
        )

    def test_zero_delivery_has_no_fabricated_latency(self) -> None:
        self.assertEqual(
            analyzer.zero_distribution(),
            {
                "deliverySamples": 0,
                "deliveryMeanNs": None,
                "deliveryP50Ns": None,
                "deliveryP95Ns": None,
                "deliveryP99Ns": None,
            },
        )


if __name__ == "__main__":
    unittest.main()
