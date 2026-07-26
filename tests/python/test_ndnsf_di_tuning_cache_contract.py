from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core.decision_validation import validate_cache, validate_tuning
from ndnsf_distributed_inference.core.ports import CacheAction, CacheProposal, TuningProposal
from ndnsf_distributed_inference.planner.execution_tuning_policy import BoundedExecutionTuningPolicy
from support.spec111_policy import request


class TuningCacheContractTest(unittest.TestCase):
    def test_declared_tuning_and_epoch_cache_validate(self):
        result = BoundedExecutionTuningPolicy().propose(request(metadata={
            "declared_families": ["microbatch"], "parameters": {"microbatch_size": 2}}))
        validate_tuning(result.value)
        validate_cache(CacheProposal(CacheAction.PLACE, "sha256:key", {}, 3), expected_epoch=3)

    def test_undeclared_tuning_and_stale_cache_rejected(self):
        with self.assertRaisesRegex(ValueError, "undeclared"):
            validate_tuning(TuningProposal({"dispatch_capacity": 10}, ("microbatch",)))
        with self.assertRaisesRegex(ValueError, "epoch"):
            validate_cache(CacheProposal(CacheAction.EVICT, "sha256:key", {}, 2), expected_epoch=3)


if __name__ == "__main__": unittest.main()
