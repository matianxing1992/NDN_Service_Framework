#!/usr/bin/env python3
"""Focused Spec 168 lifecycle and campaign-evidence gate."""

from __future__ import annotations

from dataclasses import replace
import tempfile
import unittest

from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
from ndnsf_distributed_inference.core.contracts import (
    InvocationSummaryV1,
    LifecycleEventV1,
)
from ndnsf_distributed_inference.runtime_v1_evidence import (
    reconcile_frozen_schedule,
)


PLAN = "sha256:" + "a" * 64
ASSIGNMENT = "sha256:" + "b" * 64
ANSWER = "sha256:" + "c" * 64


def event(sequence: int, event_type: str, **changes: object) -> LifecycleEventV1:
    values = {
        "experiment_id": "spec168-local",
        "request_id": "request-1",
        "attempt_epoch": 1,
        "event_id": f"event-{sequence}-{event_type}",
        "event_type": event_type,
        "component": "user",
        "provider": None,
        "provider_boot_epoch": None,
        "role": None,
        "plan_digest": None,
        "operation_id": None,
        "epoch": 0,
        "sequence": sequence,
        "monotonic_ns": sequence * 1_000,
        "wall_time_utc": "2026-08-03T00:00:00Z",
        "authenticated": True,
        "details_schema": "spec168.test.v1",
        "details": {},
    }
    values.update(changes)
    return LifecycleEventV1(**values)


def summary(row: int, terminal_kind: str = "RESPONSE", **changes: object) -> InvocationSummaryV1:
    values = {
        "experiment_id": "spec168-local",
        "schedule_row": row,
        "prompt_id": f"prompt-{row}",
        "request_id": f"request-{row}",
        "attempt_epoch": 1,
        "model_identity": "Qwen/Qwen3-0.6B@sha256:model",
        "plan_digest": PLAN,
        "assignment_digest": ASSIGNMENT,
        "cache_class": "cold",
        "terminal_kind": terminal_kind,
        "terminal_reason": "EOS",
        "accepted": True,
        "answer": "complete answer",
        "answer_digest": ANSWER,
        "token_count": 2,
        "request_to_ack_close_ms": 1.0,
        "planning_ms": 1.0,
        "publication_ms": 0.0,
        "artifact_fetch_ms": 1.0,
        "disk_to_ram_ms": 1.0,
        "ram_to_gpu_ms": 1.0,
        "dependency_wait_ms": 0.0,
        "execution_ms": 2.0,
        "response_ms": 1.0,
        "total_ms": 8.0,
        "ttft_ms": 4.0,
        "inter_token_latency_ms": (1.0,),
        "tokens_per_second": 2.0,
        "repo_unique_bytes": 100,
        "repo_wire_bytes": 100,
        "duplicate_model_payload_bytes": 0,
        "device_load_count": 1,
        "cpu_fallback_count": 0,
        "security_verdict": "PASS",
        "failure_class": "",
        "failure_code": "",
        "evidence_path": "evidence/request.jsonl",
    }
    values.update(changes)
    return InvocationSummaryV1(**values)


class Spec168LifecycleEvidenceTest(unittest.TestCase):
    def test_contract_round_trip_is_canonical_and_rejects_generic_failure(self):
        original = event(1, "REQUEST_CREATED")
        decoded = LifecycleEventV1.from_bytes(original.to_bytes())
        self.assertEqual(decoded, original)
        with self.assertRaisesRegex(ValueError, "failure_code"):
            event(2, "FAILURE_PUBLISHED", details={
                "terminal_kind": "FAILURE",
                "terminal_reason": "timeout",
                "failure_code": "TIMEOUT",
            })

    def test_journal_retains_rejected_evidence_without_mutating_live_state(self):
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "spec168")
            self.assertTrue(journal.append_lifecycle_event(
                event(1, "REQUEST_CREATED"))["accepted"])
            self.assertTrue(journal.append_lifecycle_event(event(
                2, "PLAN_COMMITTED", plan_digest=PLAN))["accepted"])
            self.assertTrue(journal.append_lifecycle_event(event(
                3, "ROLE_ASSIGNED", component="provider",
                provider="/provider/0", provider_boot_epoch="boot-1",
                role="stage-0", plan_digest=PLAN))["accepted"])
            unassigned = journal.append_lifecycle_event(event(
                30, "GPU_RESIDENT", component="adapter",
                provider="/provider/1", provider_boot_epoch="boot-2",
                role="stage-1", plan_digest=PLAN))
            self.assertFalse(unassigned["accepted"])
            self.assertEqual(unassigned["rejection_code"],
                             "ROLE_NOT_ASSIGNED")
            self.assertTrue(journal.append_lifecycle_event(event(
                4, "ARTIFACT_FETCHED", component="repo",
                provider="/provider/0", provider_boot_epoch="boot-1",
                role="stage-0", plan_digest=PLAN,
                operation_id="fetch-stage-0", epoch=1))["accepted"])

            stale = journal.append_lifecycle_event(event(
                4, "ARTIFACT_FETCHED", event_id="stale-progress",
                component="repo", provider="/provider/0",
                provider_boot_epoch="boot-1", role="stage-0",
                plan_digest=PLAN, operation_id="fetch-stage-0", epoch=1))
            self.assertFalse(stale["accepted"])
            self.assertEqual(stale["rejection_code"],
                             "NON_MONOTONIC_PROGRESS")

            unauthenticated = journal.append_lifecycle_event(replace(
                event(5, "GPU_RESIDENT", component="adapter",
                      provider="/provider/0", provider_boot_epoch="boot-1",
                      role="stage-0", plan_digest=PLAN),
                event_id="unauthenticated", authenticated=False))
            self.assertFalse(unauthenticated["accepted"])
            self.assertEqual(unauthenticated["rejection_code"],
                             "UNAUTHENTICATED_EVENT")

            wrong_plan = journal.append_lifecycle_event(event(
                6, "GPU_RESIDENT", component="adapter",
                provider="/provider/0", provider_boot_epoch="boot-1",
                role="stage-0", plan_digest="sha256:" + "d" * 64))
            self.assertFalse(wrong_plan["accepted"])
            self.assertEqual(wrong_plan["rejection_code"],
                             "PLAN_BINDING_MISMATCH")

            terminal = journal.append_lifecycle_event(event(
                7, "RESPONSE_PUBLISHED", plan_digest=PLAN,
                details={"terminal_kind": "RESPONSE",
                         "terminal_reason": "EOS",
                         "answer_digest": ANSWER, "token_count": 2}))
            self.assertTrue(terminal["accepted"])
            late = journal.append_lifecycle_event(event(
                8, "FAILURE_PUBLISHED", plan_digest=PLAN,
                details={"terminal_kind": "FAILURE",
                         "terminal_reason": "late timeout",
                         "failure_code": "RESPONSE_TIMEOUT"}))
            self.assertFalse(late["accepted"])
            self.assertEqual(late["rejection_code"], "TERMINAL_ALREADY_SET")

            records = journal.lifecycle_records()
            self.assertEqual(len(records), 10)
            self.assertEqual(sum(item["accepted"] for item in records), 5)

    def test_schedule_reconciliation_requires_exactly_one_terminal_row(self):
        schedule = [
            {"experiment_id": "spec168-local", "schedule_row": 1,
             "prompt_id": "prompt-1"},
            {"experiment_id": "spec168-local", "schedule_row": 2,
             "prompt_id": "prompt-2"},
        ]
        failed = summary(
            2, "FAILURE", terminal_reason="fetch stalled", answer="",
            answer_digest="", token_count=0, ttft_ms=None,
            failure_class="REPO_FETCH",
            failure_code="REPO_FETCH_PROGRESS_STALLED")
        result = reconcile_frozen_schedule(schedule, [summary(1), failed])
        self.assertTrue(result["complete"])
        self.assertEqual(result["counts"], {
            "success": 1, "classified_failure": 1, "canceled": 0,
            "scheduler_not_admitted": 0, "environmental_failure": 0,
        })

        with self.assertRaisesRegex(ValueError, "missing schedule rows"):
            reconcile_frozen_schedule(schedule, [summary(1)])
        with self.assertRaisesRegex(ValueError, "duplicate summary"):
            reconcile_frozen_schedule(
                schedule, [summary(1), summary(1), failed])


if __name__ == "__main__":
    unittest.main()
