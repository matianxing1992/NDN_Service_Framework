#!/usr/bin/env python3
"""Spec 087 default-policy and ownership regressions."""

from __future__ import annotations

import unittest

import ndnsf_distributed_inference as di
from ndnsf_distributed_inference.planner_registry import default_planner_registry
from ndnsf_distributed_inference.retry import RetryPolicy, RetryReason


class DefaultImportBoundaryTest(unittest.TestCase):
    def test_removed_advisory_and_experimental_semantic_cache_are_not_exported(self) -> None:
        forbidden = {
            "AdvisoryCoordinator",
            "AdvisoryCoordinatorConfig",
            "AdvisorySuggestion",
            "PlanIntent",
            "SemanticServiceCacheManager",
            "SemanticServiceCacheKey",
            "SemanticCacheDisposition",
        }
        self.assertTrue(forbidden.isdisjoint(vars(di)))

    def test_exact_forward_cache_remains_default_provider_optimization(self) -> None:
        self.assertTrue(hasattr(di, "ExactForwardCacheManager"))

    def test_semantic_cache_implementation_is_physically_experimental(self) -> None:
        from ndnsf_distributed_inference.experimental.semantic_cache import (
            SemanticServiceCacheManager,
        )
        self.assertIn(".experimental.semantic_cache.", SemanticServiceCacheManager.__module__)


class ExecutablePlannerRegistryTest(unittest.TestCase):
    def test_default_registry_has_no_handlerless_backend(self) -> None:
        self.assertTrue(all(item.handler is not None for item in default_planner_registry().backends()))


class TypedRetryTest(unittest.TestCase):
    def test_non_idempotent_operation_never_retries(self) -> None:
        policy = RetryPolicy(max_attempts=3)
        self.assertFalse(policy.should_retry(RetryReason.TIMEOUT, idempotent=False))

    def test_text_does_not_authorize_retry(self) -> None:
        policy = RetryPolicy(max_attempts=3)
        self.assertFalse(policy.should_retry(RetryReason.UNKNOWN, idempotent=True))

    def test_typed_retryable_reason_retries_idempotent_operation(self) -> None:
        policy = RetryPolicy(max_attempts=3)
        self.assertTrue(policy.should_retry(RetryReason.PROVIDER_BUSY, idempotent=True))


if __name__ == "__main__":
    unittest.main()
