from __future__ import annotations

import time
import unittest
from unittest.mock import patch

from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.planner.compatibility import (
    LegacyPlacementCompatibilityAdapter,
)
from ndnsf_distributed_inference.sdk.loader import (
    PlacementStrategyAllowlistEntry,
    discover_placement_strategies,
    distribution_record_digest,
    select_placement_strategy,
)
from ndnsf_distributed_inference.sdk.placement import (
    ModelPlacementStrategy,
    PlacementDecision,
    PlacementRequest,
    ProviderAssignment,
    evaluate_placement_strategy,
)
from ndnsf_distributed_inference.sdk.suite import OptimizationSuite


class _Distribution:
    metadata = {"Name": "research-placement"}
    version = "2.1"
    files = ("strategy.py", "RECORD")


class _Strategy(ModelPlacementStrategy):
    name = "research-latency-placement"
    version = "3"
    state_digest = "sha256:" + "a" * 64

    def plan(self, request):
        return PlacementDecision(
            split_id="split-a",
            split_digest="sha256:" + "b" * 64,
            assignments=(
                ProviderAssignment(
                    "stage-0", "/provider/a", 0, "cpu"),
            ),
            fallback_order={},
            input_digest=request.digest(),
            evidence_digest="sha256:" + "c" * 64,
        )


class _Entry:
    name = "research"
    dist = _Distribution()

    def __init__(self, loaded=_Strategy):
        self.loaded = loaded

    def load(self):
        return self.loaded


class _Entries(list):
    def select(self, *, group):
        return self if group == "ndnsf_di.placement_strategies" else []


def _allowlist(**changes):
    values = dict(
        distribution="research-placement",
        distribution_version="2.1",
        distribution_digest=distribution_record_digest(_Distribution()),
        entry_point="research",
        strategy_name=_Strategy.name,
        strategy_version=_Strategy.version,
        state_digest=_Strategy.state_digest,
    )
    values.update(changes)
    return PlacementStrategyAllowlistEntry(**values)


def _request(max_policy_ms=100):
    return PlacementRequest(
        request_id="request-external",
        attempt=1,
        deadline_ms=2_000_000_000_000,
        model_digest="sha256:" + "d" * 64,
        graph_digest="sha256:" + "e" * 64,
        candidate_ids=("split-a",),
        providers=(),
        required_roles=("stage-0",),
        budget=CandidateBudget(
            max_candidates=1, max_policy_ms=max_policy_ms),
    )


class ExternalPlacementStrategyTest(unittest.TestCase):
    def test_exact_package_entry_and_strategy_identity_are_required(self):
        with patch(
            "ndnsf_distributed_inference.sdk.loader.metadata.entry_points",
            return_value=_Entries([_Entry()]),
        ):
            self.assertEqual(discover_placement_strategies([]), {})
            self.assertEqual(
                discover_placement_strategies([
                    _allowlist(distribution_version="wrong")]),
                {},
            )
            strategies = discover_placement_strategies([_allowlist()])
        self.assertIsInstance(
            strategies[_Strategy.name], ModelPlacementStrategy)

        class _Changed(_Strategy):
            state_digest = "sha256:" + "f" * 64

        with patch(
            "ndnsf_distributed_inference.sdk.loader.metadata.entry_points",
            return_value=_Entries([_Entry(_Changed)]),
        ):
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                discover_placement_strategies([_allowlist()])

    def test_operator_configuration_selects_exact_version_and_state(self):
        strategy = _Strategy()
        config = {"planning": {"strategy": {
            "name": strategy.name,
            "version": strategy.version,
            "digest": strategy.state_digest,
        }}}
        self.assertIs(
            select_placement_strategy(config, {strategy.name: strategy}),
            strategy,
        )
        changed = {
            "planning": {"strategy": {**config["planning"]["strategy"],
                                      "version": "4"}}
        }
        with self.assertRaisesRegex(ValueError, "pin mismatch"):
            select_placement_strategy(changed, {strategy.name: strategy})

    def test_bounded_execution_and_untrusted_return_validation(self):
        class _Slow(_Strategy):
            def plan(self, request):
                time.sleep(0.05)
                return super().plan(request)

        started = time.monotonic()
        with self.assertRaisesRegex(TimeoutError, "time budget"):
            evaluate_placement_strategy(_Slow(), _request(2))
        self.assertLess(time.monotonic() - started, 0.04)

        class _Malformed(_Strategy):
            def plan(self, request):
                return {"provider_token": "must-not-cross"}

        with self.assertRaises((TypeError, ValueError)):
            evaluate_placement_strategy(_Malformed(), _request())

    def test_legacy_ports_and_decide_are_hints_not_authority(self):
        class _LegacyClient:
            def __init__(self):
                self.requests = None

            def decide(self, requests):
                self.requests = requests
                return {
                    "partition": "old-split-hint",
                    "provider_assignment": "old-provider-hint",
                }

        client = _LegacyClient()
        hints = LegacyPlacementCompatibilityAdapter(client).collect({
            "partition": object(),
            "provider_assignment": object(),
            "deployment": object(),
        })
        self.assertEqual(
            set(client.requests), {"partition", "provider_assignment"})
        self.assertFalse(hints.authoritative)
        with self.assertRaisesRegex(RuntimeError, "cannot authorize"):
            hints.to_placement_decision()

        suite = OptimizationSuite(
            {}, name="migration", version="1",
            state_digest="sha256:" + "1" * 64,
            placement_strategy=_Strategy(),
        )
        self.assertIsInstance(
            suite.joint_placement_strategy(), _Strategy)


if __name__ == "__main__":
    unittest.main()
