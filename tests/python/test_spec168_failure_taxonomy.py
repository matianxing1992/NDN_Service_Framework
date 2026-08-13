#!/usr/bin/env python3
"""Spec 168 primary-boundary and progress-checkpoint gate."""

from __future__ import annotations

import unittest

from Experiments.ndnsf_validation.deadlines import (
    DeadlineMonitor,
    DeadlineTerminal,
    ProgressObservation,
)
from ndnsf_distributed_inference.app_sdk.status import (
    RequestFailureStatus,
    RequestState,
)
from ndnsf_distributed_inference.core.state import (
    FailureBoundaryV1,
    FailureRecordV1,
    failure_boundary_for_code,
)


class _Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class Spec168FailureTaxonomyTest(unittest.TestCase):
    def test_every_primary_boundary_has_an_exact_code_family(self) -> None:
        cases = {
            "ENV_ALLOCATION_FAILED": FailureBoundaryV1.ENVIRONMENT,
            "BOOT_CERTIFICATE_FETCH_FAILED": FailureBoundaryV1.BOOTSTRAP,
            "ROUTE_PROVIDER_UNREACHABLE": FailureBoundaryV1.ROUTING,
            "ACK_WRONG_REQUEST": FailureBoundaryV1.ACK,
            "PLAN_CAPACITY_OVERFLOW": FailureBoundaryV1.PLAN,
            "REPO_PUBLISH_DIGEST_MISMATCH": FailureBoundaryV1.REPO_PUBLISH,
            "REPO_FETCH_PROGRESS_STALLED": FailureBoundaryV1.REPO_FETCH,
            "PREP_CUDA_WARMUP_FAILED": FailureBoundaryV1.PREP,
            "DEPENDENCY_INPUT_MISSING": FailureBoundaryV1.DEPENDENCY,
            "EXEC_BACKEND_REJECTED": FailureBoundaryV1.EXEC,
            "TOKEN_ORDER_INVALID": FailureBoundaryV1.TOKEN,
            "RESPONSE_AUTH_FAILED": FailureBoundaryV1.RESPONSE,
            "CLEANUP_RESOURCE_LEAK": FailureBoundaryV1.CLEANUP,
            "ANALYZER_SCHEDULE_MISMATCH": FailureBoundaryV1.ANALYZER,
            "UNRESOLVED_EVIDENCE_GAP": FailureBoundaryV1.UNRESOLVED,
        }
        for code, boundary in cases.items():
            self.assertIs(failure_boundary_for_code(code), boundary)
            record = FailureRecordV1(
                request_id="request-1",
                attempt_epoch=1,
                component="provider" if boundary not in {
                    FailureBoundaryV1.ENVIRONMENT, FailureBoundaryV1.ANALYZER,
                } else "operator",
                failure_code=code,
                boundary=boundary,
                provider="/provider/0" if boundary not in {
                    FailureBoundaryV1.ENVIRONMENT, FailureBoundaryV1.ANALYZER,
                } else "",
                role="stage-0" if boundary not in {
                    FailureBoundaryV1.ENVIRONMENT, FailureBoundaryV1.ANALYZER,
                } else "",
                operation_id="op-1",
                artifact_range="segment=4" if boundary is FailureBoundaryV1.REPO_FETCH else "",
                last_checkpoint="FETCHING segment=3 bytes=4096",
                terminal_reason="classified failure",
                evidence_paths=("evidence/request.jsonl",),
            )
            self.assertEqual(FailureRecordV1.from_dict(record.to_dict()), record)

    def test_generic_timeout_and_boundary_mismatch_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "exact lifecycle boundary"):
            failure_boundary_for_code("TIMEOUT")
        with self.assertRaisesRegex(ValueError, "disagree"):
            FailureRecordV1(
                request_id="request-1", attempt_epoch=1, component="repo",
                failure_code="REPO_FETCH_RANGE_STALLED",
                boundary=FailureBoundaryV1.PREP,
                last_checkpoint="range=2", terminal_reason="stalled",
            )

    def test_stall_retains_last_authenticated_checkpoint(self) -> None:
        clock = _Clock()
        monitor = DeadlineMonitor(
            request_id="request-1", operation_id="fetch-1",
            provider="/provider/0", role="stage-0", attempt=1,
            idle_budget=5, hard_budget=20, clock=clock,
        )
        observation = ProgressObservation(
            request_id="request-1", operation_id="fetch-1",
            provider="/provider/0", role="stage-0", attempt=1,
            epoch=0, sequence=1, phase="FETCHING", completed_work=4096,
            total_work=8192, authenticated=True, observed_at=0,
            checkpoint="range=0-4095 segment=0",
        )
        self.assertTrue(monitor.admit(observation).admitted)
        clock.now = 5
        self.assertEqual(monitor.poll(), DeadlineTerminal.STALLED)
        evidence = monitor.terminal_evidence()
        self.assertEqual(evidence["lastCheckpoint"], "range=0-4095 segment=0")
        self.assertEqual(evidence["terminal"], "STALLED")
        self.assertEqual(
            monitor.admit(observation).last_checkpoint,
            "range=0-4095 segment=0",
        )

    def test_hard_deadline_and_first_terminal_state_are_distinct(self) -> None:
        clock = _Clock()
        monitor = DeadlineMonitor(
            request_id="request-1", operation_id="stage-1",
            provider="/provider/1", role="stage-1", attempt=1,
            idle_budget=5, hard_budget=10, clock=clock,
        )
        self.assertTrue(monitor.finish(DeadlineTerminal.CANCELLED))
        clock.now = 10
        self.assertEqual(monitor.poll(), DeadlineTerminal.CANCELLED)
        self.assertFalse(monitor.finish(DeadlineTerminal.HARD_TIMEOUT))
        status = RequestFailureStatus(
            request_id="request-1", state=RequestState.CANCELLED,
            failure_code="EXEC_CANCELLED", last_checkpoint="stage-1 assigned",
            terminal_reason="operator cancellation",
        )
        self.assertEqual(status.boundary, FailureBoundaryV1.EXEC)


if __name__ == "__main__":
    unittest.main()
