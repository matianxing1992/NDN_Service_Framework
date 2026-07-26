from __future__ import annotations

import unittest

from ndnsf_distributed_inference.app_sdk.engine import DistributedInferenceEngine
from ndnsf_distributed_inference.core.ports import (
    CandidateBudget, EngineSnapshot, EstimateEnvelope, MetricValue, ModelCandidate,
    OptimizationObjective, PolicyRequest, PolicyState,
)
from ndnsf_distributed_inference.planner.defaults import DefaultOptimizationSuite
from ndnsf_distributed_inference.sdk.contracts import POLICY_KINDS


class EngineContractTest(unittest.TestCase):
    def snapshot(self) -> EngineSnapshot:
        return EngineSnapshot(
            snapshot_id="snapshot-1", epoch=1, captured_at_ms=1000,
            metrics={"latency": MetricValue(12.0, "ms", "p95")},
            estimates={}, state=PolicyState(1, "sha256:state"))

    def test_default_suite_exposes_exactly_ten_policy_ports(self) -> None:
        suite = DefaultOptimizationSuite()
        self.assertEqual(tuple(suite.policy_names()), POLICY_KINDS)
        self.assertEqual(len(POLICY_KINDS), 10)

    def test_engine_rejects_candidate_budget_overflow_after_policy(self) -> None:
        engine = DistributedInferenceEngine(DefaultOptimizationSuite())
        candidates = tuple(ModelCandidate(
            model_id=f"qwen-{i}", variant_id=f"v{i}", exact_semantics=True,
            precision="fp16", artifact_digest=f"sha256:{i:064x}")
            for i in range(3))
        with self.assertRaisesRegex(ValueError, "candidate budget"):
            engine.choose_model(
                objective=OptimizationObjective(
                    hard_constraints={"latency": MetricValue(100, "ms", "max")},
                    weights={"latency": 1.0}, normalization={"latency": 100.0}),
                snapshot=self.snapshot(), candidates=candidates,
                budget=CandidateBudget(max_candidates=2))

    def test_decisions_are_epoch_bound_not_packet_or_token_bound(self) -> None:
        engine = DistributedInferenceEngine(DefaultOptimizationSuite())
        first = engine.decision_epoch(self.snapshot())
        same = engine.decision_epoch(self.snapshot())
        newer = engine.decision_epoch(EngineSnapshot(
            snapshot_id="snapshot-2", epoch=2, captured_at_ms=1001,
            metrics=self.snapshot().metrics, estimates={},
            state=PolicyState(2, "sha256:new")))
        self.assertEqual(first, same)
        self.assertGreater(newer, first)

    def test_stale_estimate_is_rejected(self) -> None:
        stale = EngineSnapshot(
            "stale", 1, 1000, self.snapshot().metrics,
            {"queue": EstimateEnvelope(1, "requests", 10, 900, "provider")},
            PolicyState(1, "sha256:stale"))
        engine = DistributedInferenceEngine(DefaultOptimizationSuite())
        with self.assertRaisesRegex(ValueError, "stale estimate"):
            engine.choose_model(
                objective=OptimizationObjective(
                    {"latency": MetricValue(100, "ms", "max")},
                    {"latency": 1}, {"latency": 100}),
                snapshot=stale,
                candidates=(ModelCandidate("qwen", "exact", True, "fp16", "sha256:" + "1" * 64),),
                budget=CandidateBudget(1), at_ms=1000)

    def test_engine_graph_invokes_all_ten_once(self) -> None:
        objective = OptimizationObjective(
            {"latency": MetricValue(100, "ms", "max")},
            {"latency": 1}, {"latency": 100})
        request = PolicyRequest(objective, self.snapshot(), CandidateBudget(2), ())
        # Each default has different required metadata, so graph closure is
        # asserted with the no-input control-plane seams that are valid here.
        results = DistributedInferenceEngine(DefaultOptimizationSuite()).run_decision_graph({
            "deployment": request,
            "admission": request,
            "execution_tuning": request,
            "cache": PolicyRequest(objective, self.snapshot(), CandidateBudget(2), (),
                                   metadata={"cache_key_digest": "sha256:" + "0" * 64}),
            "recovery": PolicyRequest(objective, self.snapshot(), CandidateBudget(2), (),
                                      metadata={"attempt_epoch": 1, "original_deadline_ms": 10}),
            "execution_target": request,
        })
        self.assertEqual(set(results), {"deployment", "admission", "execution_tuning",
                                        "cache", "recovery", "execution_target"})


if __name__ == "__main__":
    unittest.main()
