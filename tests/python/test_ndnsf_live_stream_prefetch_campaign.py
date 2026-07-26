#!/usr/bin/env python3
"""Tests for the frozen Spec 119 prefetch campaign and adoption rule."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]
CAMPAIGN = REPO / "Experiments/NDNSF_LiveStream_Prefetch_Campaign.py"


def load_campaign():
    spec = importlib.util.spec_from_file_location("live_stream_prefetch_campaign", CAMPAIGN)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def make_run(*, loss: int, pair: int, policy: str, lag: float,
             timeout_nack: int, accepted: bool = True,
             future_ratio: float = 1.0) -> dict:
    return {
        "lossPercent": loss,
        "pairId": pair,
        "prefetchPolicy": policy,
        "captureToDecodeP95Ms": lag,
        "maxTimeouts": timeout_nack,
        "maxNacks": 0,
        "maxCoreInFlight": 5,
        "accepted": accepted,
        "futurePayloadInterests": 8 if policy == "mapped-live-v1-future-on" else 0,
        "providerFutureEligible": 8 if policy == "mapped-live-v1-future-on" else 0,
        "providerFutureHitRatio": future_ratio if policy == "mapped-live-v1-future-on" else 0.0,
    }


class LiveStreamPrefetchCampaignTest(unittest.TestCase):
    def test_adopts_only_when_every_frozen_gate_passes(self) -> None:
        campaign = load_campaign()
        runs = []
        for loss in (0, 5):
            for pair in range(1, 6):
                runs.extend([
                    make_run(loss=loss, pair=pair, policy="mapped-pressure",
                             lag=300.0, timeout_nack=10),
                    make_run(loss=loss, pair=pair,
                             policy="mapped-live-v1-future-on",
                             lag=240.0, timeout_nack=10),
                    make_run(loss=loss, pair=pair,
                             policy="mapped-live-v1-future-off",
                             lag=280.0, timeout_nack=10),
                ])

        decision = campaign.analyze(runs)
        self.assertTrue(decision["completionGate"])
        self.assertTrue(decision["futureHitGate"])
        self.assertTrue(decision["adoptionGate"])
        self.assertEqual(decision["selectedDefault"], "mapped-live-v1-future-on")

    def test_failed_or_vacuous_future_run_preserves_pressure_default(self) -> None:
        campaign = load_campaign()
        runs = []
        for loss in (0, 5):
            for pair in range(1, 6):
                runs.extend([
                    make_run(loss=loss, pair=pair, policy="mapped-pressure",
                             lag=300.0, timeout_nack=0),
                    make_run(loss=loss, pair=pair,
                             policy="mapped-live-v1-future-on",
                             lag=200.0, timeout_nack=0,
                             future_ratio=0.98 if pair == 1 else 1.0),
                    make_run(loss=loss, pair=pair,
                             policy="mapped-live-v1-future-off",
                             lag=250.0, timeout_nack=0),
                ])

        decision = campaign.analyze(runs)
        self.assertFalse(decision["futureHitGate"])
        self.assertEqual(decision["selectedDefault"], "mapped-pressure")
        self.assertTrue(decision["negativeResultPreserved"])

        runs[0]["accepted"] = False
        decision = campaign.analyze(runs)
        self.assertFalse(decision["completionGate"])
        self.assertEqual(decision["selectedDefault"], "mapped-pressure")

    def test_missing_treatment_never_creates_a_favorable_pair(self) -> None:
        campaign = load_campaign()
        runs = [
            make_run(loss=0, pair=1, policy="mapped-pressure", lag=300.0,
                     timeout_nack=2),
            make_run(loss=0, pair=1, policy="mapped-live-v1-future-on",
                     lag=200.0, timeout_nack=2),
        ]
        decision = campaign.analyze(runs)
        self.assertFalse(decision["adoptionGate"])
        self.assertEqual(decision["pairedEffects"], [])


if __name__ == "__main__":
    unittest.main()
