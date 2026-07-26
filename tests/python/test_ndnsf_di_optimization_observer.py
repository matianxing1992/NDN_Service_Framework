from __future__ import annotations

import sys
from pathlib import Path
import unittest

from ndnsf_distributed_inference.core.ports import ExecutionOutcome
from ndnsf_distributed_inference.sdk.observer import ObserverRegistry


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/fixtures/ndnsf-di-external-optimizer/src"))
from ndnsf_di_external_optimizer import FixtureObserver  # noqa: E402


class FailingObserver:
    name = "failing"; version = "1"
    def observe(self, outcome, idempotency_key): raise RuntimeError("boom")


class OptimizationObserverTest(unittest.TestCase):
    def test_delivery_is_idempotent_and_failure_isolated(self):
        good = FixtureObserver(); registry = ObserverRegistry()
        registry.register(good); registry.register(FailingObserver())
        outcome = ExecutionOutcome("r", 1, "OK", "sha256:result")
        first = registry.deliver(outcome, "r:1")
        second = registry.deliver(outcome, "r:1")
        self.assertEqual(first, {"fixture-observer": "delivered", "failing": "failed"})
        self.assertEqual(second, {"fixture-observer": "duplicate", "failing": "duplicate"})
        self.assertEqual(good.keys, ["r:1"])


if __name__ == "__main__": unittest.main()
