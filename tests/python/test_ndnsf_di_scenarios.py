#!/usr/bin/env python3
"""Comprehensive scenario tests for NDNSF-DI main flows and logic.

Covers: deployment lifecycle, Merge Provider auto-ref_count,
placement with preemption, normalized scoring, provisioning ACK parsing,
health tracker, circuit breaker, rate limiting, retry, DISK_RESIDENT fallback.
"""

from __future__ import annotations

import json
import hashlib
import unittest
from dataclasses import replace

from ndnsf.runtime_telemetry import (
    AdmissionLeaseStatus,
    DeploymentStatus,
    PlacementConstraint,
    TokenBucket,
    score_normalized,
    stable_digest,
    now_ms,
    RESIDENCY_READY_COST_MS,
)
from ndnsf.ndnsd_health import NdnsdHealthTracker
from ndnsf_distributed_inference.retry import RetryPolicy, retry_call
from ndnsf.metrics import NdnMetrics
from ndnsf_distributed_inference.runtime_v1 import (
    Deployment,
    DeploymentStatus as DepStatus,
    filter_feasible_providers,
    score_provider_candidates,
    score_runtime_candidate,
    FragmentResidency,
    ModelFragmentKey,
    PlanRole,
    GenericAckMetadata,
    GenericProviderRuntimeHint,
    GenericAdmissionLease,
    AdmissionLeaseStatus,
    DiProviderRuntimeState,
)


class DeploymentLifecycleTest(unittest.TestCase):
    """Deployment metadata is descriptive; providers own eviction authority."""

    def test_deployment_created_in_provisioning(self) -> None:
        dep = Deployment(deployment_id="dep-1", service_name="/Svc")
        self.assertEqual(dep.status, DepStatus.PROVISIONING)
        self.assertEqual(dep.ref_count, 0)

    def test_active_deployment_has_zero_ready_cost(self) -> None:
        dep = Deployment(deployment_id="dep-1", status=DepStatus.ACTIVE)
        self.assertEqual(dep.estimated_ready_ms(), 0.0)

    def test_disk_resident_has_35ms_ready_cost(self) -> None:
        dep = Deployment(deployment_id="dep-1", status=DepStatus.DISK_RESIDENT)
        self.assertAlmostEqual(dep.estimated_ready_ms(), 35.0, delta=0.1)

    def test_provisioning_cannot_evict(self) -> None:
        dep = Deployment(deployment_id="dep-1", status=DepStatus.PROVISIONING)
        ok, reason = dep.can_evict()
        self.assertFalse(ok)
        self.assertIn("NOT_READY", reason)

    def test_descriptive_ref_count_is_not_eviction_authority(self) -> None:
        dep = Deployment(deployment_id="dep-1", status=DepStatus.ACTIVE, ref_count=3)
        ok, reason = dep.can_evict()
        self.assertFalse(ok)
        self.assertEqual(reason, "PROVIDER_EXECUTION_LEASE_CHECK_REQUIRED")

    def test_zero_ref_count_still_requires_provider_lease_check(self) -> None:
        dep = Deployment(deployment_id="dep-1", status=DepStatus.ACTIVE, ref_count=0)
        ok, reason = dep.can_evict()
        self.assertFalse(ok)
        self.assertEqual(reason, "PROVIDER_EXECUTION_LEASE_CHECK_REQUIRED")

    def test_from_dict_round_trip(self) -> None:
        payload = {
            "deploymentId": "dep-abc", "planId": "plan-1",
            "serviceName": "/Svc", "status": "ACTIVE",
            "fragmentMap": {"/Backbone": [{"provider": "/P/a"}]},
            "refCount": 2, "createdAtMs": 1000, "updatedAtMs": 2000,
        }
        dep = Deployment.from_dict(payload)
        self.assertEqual(dep.deployment_id, "dep-abc")
        self.assertEqual(dep.status, DepStatus.ACTIVE)
        self.assertEqual(dep.ref_count, 2)
        self.assertEqual(dep.role_provider("/Backbone"), "/P/a")


class PlacementFilterTest(unittest.TestCase):
    """Placement FILTER: hard constraints + IDLE preemption."""

    def _candidate(self, provider: str, free_gpu: float = 8000.0,
                   backends: tuple = ("onnxruntime",)) -> dict:
        hint = GenericProviderRuntimeHint(provider_name=provider, queue_length=0)
        state = DiProviderRuntimeState(
            provider_name=provider, free_gpu_memory_mb=free_gpu,
            supported_backends=backends,
        )
        return {
            "providerName": provider,
            "genericAckMetadata": GenericAckMetadata(
                provider_runtime_hint=hint,
                service_payload=state.__dict__,
                service_payload_schema="test",
            ),
        }

    def test_provider_with_sufficient_gpu_is_feasible(self) -> None:
        candidates = [self._candidate("/P/a", free_gpu=6000)]
        constraint = {"min_gpu_memory_mb": 4000}
        result = filter_feasible_providers("/Backbone", candidates, constraint)
        self.assertEqual(len(result), 1)

    def test_provider_with_insufficient_gpu_is_excluded(self) -> None:
        candidates = [self._candidate("/P/a", free_gpu=2000)]
        constraint = {"min_gpu_memory_mb": 4000}
        result = filter_feasible_providers("/Backbone", candidates, constraint)
        self.assertEqual(len(result), 0)

    def test_idle_deployment_enables_preemption(self) -> None:
        """Provider GPU full, but has an IDLE deployment → can be preempted."""
        candidates = [self._candidate("/P/a", free_gpu=2000)]
        constraint = {"min_gpu_memory_mb": 4000}
        existing = {
            "dep-old": {
                "status": "ACTIVE", "refCount": 0, "ref_count": 0,
                "fragmentMap": {
                    "/Backbone": [{"provider": "/P/a", "role": "/Backbone"}],
                },
            }
        }
        result = filter_feasible_providers(
            "/Backbone", candidates, constraint, existing_deployments=existing)
        self.assertEqual(len(result), 1)

    def test_active_deployment_cannot_be_preempted(self) -> None:
        """GPU full AND deployment ref_count>0 → cannot preempt."""
        candidates = [self._candidate("/P/a", free_gpu=2000)]
        constraint = {"min_gpu_memory_mb": 4000}
        existing = {
            "dep-old": {"status": "ACTIVE", "refCount": 3, "ref_count": 3,
                        "fragmentMap": {
                            "/Backbone": [{"provider": "/P/a", "role": "/Backbone"}],
                        }},
        }
        result = filter_feasible_providers(
            "/Backbone", candidates, constraint, existing_deployments=existing)
        self.assertEqual(len(result), 0)

    def test_wrong_backend_is_excluded(self) -> None:
        candidates = [self._candidate("/P/a", backends=("onnx-cpu",))]
        constraint = {"required_backend": "onnx-cuda"}
        result = filter_feasible_providers("/Backbone", candidates, constraint)
        self.assertEqual(len(result), 0)


class NormalizedScoringTest(unittest.TestCase):
    """Normalized Filter→Score→Pick tests."""

    def test_fragment_ready_cost_is_normalized(self) -> None:
        """CPU_RESIDENT (8ms) should score higher than DISK_RESIDENT (35ms)."""
        cpu_score = score_normalized(8.0, worst=105, best=0)
        disk_score = score_normalized(35.0, worst=105, best=0)
        self.assertGreater(cpu_score, disk_score)

    def test_normalized_best_is_100(self) -> None:
        self.assertAlmostEqual(score_normalized(0, worst=100, best=0), 100.0, delta=0.1)

    def test_normalized_worst_is_0(self) -> None:
        self.assertAlmostEqual(score_normalized(100, worst=100, best=0), 0.0, delta=0.1)


class TokenBucketTest(unittest.TestCase):
    """Token bucket rate limiter."""

    def test_unlimited_always_consumes(self) -> None:
        tb = TokenBucket(rate_per_second=0)
        for _ in range(100):
            self.assertTrue(tb.consume())

    def test_limited_rate_blocks_excess(self) -> None:
        tb = TokenBucket(rate_per_second=100, burst=10)
        consumed = sum(1 for _ in range(100) if tb.consume())
        self.assertLess(consumed, 100)  # burst limits initial burst
        self.assertGreater(consumed, 0)

    def test_reset_restores_burst(self) -> None:
        tb = TokenBucket(rate_per_second=1, burst=3)
        for _ in range(3):
            tb.consume()
        self.assertFalse(tb.consume())
        tb.reset()
        self.assertTrue(tb.consume())


class RetryPolicyTest(unittest.TestCase):
    """Retry with exponential backoff."""

    def test_retry_on_timeout(self) -> None:
        p = RetryPolicy(max_attempts=3)
        self.assertTrue(p.should_retry("timeout: /req-1", 1000))

    def test_retry_on_lease_rejected(self) -> None:
        p = RetryPolicy(max_attempts=3)
        self.assertTrue(p.should_retry("LEASE_EXPIRED", 500))

    def test_retry_on_provider_busy(self) -> None:
        p = RetryPolicy(max_attempts=3)
        self.assertTrue(p.should_retry("provider busy queue full", 200))

    def test_no_retry_when_max_exceeded(self) -> None:
        p = RetryPolicy(max_attempts=1)
        p.attempts = 1
        self.assertFalse(p.should_retry("timeout", 1000))

    def test_no_retry_on_non_retryable_error(self) -> None:
        p = RetryPolicy(max_attempts=3)
        self.assertFalse(p.should_retry("invalid request payload", 100))

    def test_backoff_increases(self) -> None:
        p = RetryPolicy(base_backoff_ms=100, multiplier=2.0, jitter=0)
        b0 = p.next_backoff_ms()
        p.attempts = 1
        b1 = p.next_backoff_ms()
        self.assertGreater(b1, b0)

    def test_retry_call_success(self) -> None:
        calls = [0]
        def fn():
            calls[0] += 1
            return {"status": "executed", "elapsedMs": 10}
        result = retry_call(fn, RetryPolicy(max_attempts=3))
        self.assertEqual(result["status"], "executed")
        self.assertEqual(calls[0], 1)

    def test_retry_call_with_failures(self) -> None:
        calls = [0]
        def fn():
            calls[0] += 1
            if calls[0] < 3:
                return {"status": "failed", "error": "timeout", "elapsedMs": 100}
            return {"status": "executed", "elapsedMs": 10}
        result = retry_call(fn, RetryPolicy(max_attempts=3, jitter=0))
        self.assertEqual(result["status"], "executed")
        self.assertEqual(calls[0], 3)


class HealthTrackerTest(unittest.TestCase):
    """NDNSD health tracker + circuit breaker."""

    def test_unknown_provider_has_health_1(self) -> None:
        ht = NdnsdHealthTracker()
        self.assertAlmostEqual(ht.health_score("/P/unknown"), 1.0, delta=0.01)

    def test_fresh_provider_has_high_score(self) -> None:
        ht = NdnsdHealthTracker()
        ht.update_from_ndnsd([{
            "provider": "/P/a", "serviceLifetime": 30,
            "publishTimestamp": int(__import__("time").time()),
            "serviceMetaInfo": {"idleWorkers": "1", "workers": "1", "runtimeStatus": "ready"},
        }])
        self.assertGreater(ht.health_score("/P/a"), 0.8)

    def test_circuit_breaker_starts_closed(self) -> None:
        ht = NdnsdHealthTracker()
        self.assertEqual(ht.circuit_breaker_state("/P/x"), "CLOSED")

    def test_provider_is_available_by_default(self) -> None:
        ht = NdnsdHealthTracker()
        self.assertTrue(ht.is_available("/P/x"))


class MetricsTest(unittest.TestCase):
    """Prometheus metrics."""

    def test_counter_increment(self) -> None:
        m = NdnMetrics()
        m.request_total.labels(service="/Svc", status="success").inc()
        m.request_total.labels(service="/Svc", status="success").inc()
        m.request_total.labels(service="/Svc", status="failed").inc()
        output = m.dumps()
        self.assertIn("ndnsf_requests_total", output)

    def test_gauge_set(self) -> None:
        m = NdnMetrics()
        m.ack_queue_depth.labels(provider="/P/a").set(5)
        output = m.dumps()
        self.assertIn("ndnsf_ack_queue_depth", output)

    def test_histogram_observe(self) -> None:
        m = NdnMetrics()
        m.request_duration_ms.observe(150, service="/Svc")
        output = m.dumps()
        self.assertIn("ndnsf_request_duration_ms", output)


class ProvisioningAckParsingTest(unittest.TestCase):
    """Provisioning negative-ACK parsing."""

    def test_parse_deployment_id_from_ack_fields(self) -> None:
        """Simulate parsing the ACK payload from a provisioning provider."""
        payload = (
            "roles=/Backbone;queue=0;readyQueue=0;waitingInputs=0;"
            "activeWorkers=0;workers=1;idleWorkers=1;hasModel=0;"
            "runtimeStatus=installing;negativeAckReason=ModelUnavailable;"
            "deploymentId=dep-abc;provisioningRole=/Backbone;expectedReadyMs=5000;"
        )
        fields = {}
        for item in payload.split(";"):
            if "=" not in item:
                continue
            k, v = item.split("=", 1)
            fields[k.strip()] = v.strip()
        self.assertEqual(fields.get("deploymentId"), "dep-abc")
        self.assertEqual(fields.get("provisioningRole"), "/Backbone")
        self.assertEqual(fields.get("expectedReadyMs"), "5000")
        self.assertEqual(fields.get("negativeAckReason"), "ModelUnavailable")

    def test_ready_provider_has_no_provisioning_fields(self) -> None:
        """Ready providers don't include provisioning context."""
        payload = (
            "roles=/Backbone;queue=0;runtimeStatus=ready;hasModel=1;"
        )
        self.assertNotIn("deploymentId=", payload)
        self.assertNotIn("provisioningRole=", payload)


class PlanSecurityTest(unittest.TestCase):
    """Plan namespace, signing, validation."""

    def test_plan_id_is_in_creator_namespace(self) -> None:
        from ndnsf_distributed_inference.plan_security import make_plan_id, validate_plan_namespace
        pid = make_plan_id("/NDNSF-DI/Tracer/user", "my-plan")
        self.assertTrue(validate_plan_namespace(pid, "/NDNSF-DI/Tracer/user"))
        self.assertFalse(validate_plan_namespace(pid, "/NDNSF-DI/Tracer/other"))

    def test_plan_content_digest_is_stable(self) -> None:
        from ndnsf_distributed_inference.plan_security import plan_content_digest
        plan = {"planId": "test", "roles": ["/Backbone"], "dependencies": []}
        d1 = plan_content_digest(plan)
        d2 = plan_content_digest(plan)
        self.assertEqual(d1, d2)  # deterministic

    def test_modified_plan_yields_different_digest(self) -> None:
        from ndnsf_distributed_inference.plan_security import plan_content_digest
        plan1 = {"planId": "test", "roles": ["/Backbone"]}
        plan2 = {"planId": "test", "roles": ["/Backbone", "/Extra"]}
        self.assertNotEqual(plan_content_digest(plan1), plan_content_digest(plan2))

    def test_plan_signature_round_trip(self) -> None:
        from ndnsf_distributed_inference.plan_security import PlanSignature
        plan = {"planId": "test-plan", "roles": ["/Backbone"]}
        sig = PlanSignature(plan_id="test-plan", creator="/user/a")
        sig.sign(plan, lambda p: hashlib.sha256(p).digest())
        secret = sig.signature_bytes
        ok, reason = sig.verify(plan, lambda p, s: (True, "OK") if s == secret else (False, "MISMATCH"))
        self.assertTrue(ok, reason)

    def test_revoked_plan_is_invalid(self) -> None:
        from ndnsf_distributed_inference.plan_security import PlanState
        ps = PlanState(plan_id="test", creator="/u")
        ps.revoke()
        self.assertFalse(ps.is_valid)

    def test_expired_plan_is_invalid(self) -> None:
        from ndnsf_distributed_inference.plan_security import PlanState
        ps = PlanState(plan_id="test", creator="/u", expires_at_ms=1000)
        self.assertFalse(ps.is_valid)

    def test_superseded_plan_has_pointer(self) -> None:
        from ndnsf_distributed_inference.plan_security import PlanState
        ps = PlanState(plan_id="test", creator="/u")
        ps.supersede("test-v2")
        self.assertEqual(ps.status, "SUPERSEDED")
        self.assertEqual(ps.superseded_by, "test-v2")


if __name__ == "__main__":
    unittest.main()
