#!/usr/bin/env python3
"""Spec 107 live-fault evidence schema tests."""

from __future__ import annotations

import copy
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from ndnsf_distributed_inference.runtime_v1_evidence import (  # noqa: E402
    LiveFaultRecordV1,
    OwnedProcessV1,
)
from spec107_fault_controller import (  # noqa: E402
    FaultControllerError,
    OwnedProcessRegistry,
)
from run_spec107_live_faults import (  # noqa: E402
    FAULT_CELLS,
    derive_fault_provider_control,
    LiveFaultOrchestrationError,
    LiveFaultOrchestrator,
    validate_cell_claim,
)
from spec107_identity import build_candidate_identity, build_campaign_identity  # noqa: E402


def digest(character: str) -> str:
    return "sha256:" + character * 64


def process_payload() -> dict[str, object]:
    return {
        "pid": 1234,
        "processGroupId": 1200,
        "procStartTimeTicks": 998877,
        "parentPid": 1000,
        "campaignId": "spec107-c1-fault-r1-aaaaaaaaaaaa",
        "role": "/LLM/Stage/1",
        "providerName": "/provider/1",
        "providerBootId": "boot-1",
        "commandDigest": digest("1"),
        "executableDigest": digest("2"),
    }


def fault_payload() -> dict[str, object]:
    return {
        "schema": "ndnsf-di-spec107-live-fault-v1",
        "candidateId": "spec107-c1-111111111111-222222222222-333333333333-"
                       "444444444444-555555555555-666666666666",
        "campaignId": "spec107-c1-fault-r1-aaaaaaaaaaaa",
        "cellId": "provider-kill-restart",
        "commandDigest": digest("3"),
        "target": process_payload(),
        "trigger": "stage-1-active",
        "triggerMonotonicUs": 1000,
        "injectionMonotonicUs": 1100,
        "injectionApplied": True,
        "networkInjection": True,
        "intendedEffect": "provider-process-exit",
        "observedEffect": "provider-process-exit",
        "attemptEpochBefore": 0,
        "attemptEpochAfter": 1,
        "providerBootBefore": "boot-1",
        "providerBootAfter": "boot-2",
        "replacementCount": 1,
        "originalDeadlineEpochMs": 5000,
        "currentDeadlineEpochMs": 5000,
        "cancelSupersedeAuthenticated": True,
        "authoritativeTerminalCount": 1,
        "terminalReason": "SUCCESS",
        "cleanup": {
            "proven": True,
            "baseline": {"threads": 4, "waits": 0, "leases": 0,
                         "routes": 0, "processes": 0, "attempts": 0},
            "after": {"threads": 4, "waits": 0, "leases": 0,
                      "routes": 0, "processes": 0, "attempts": 0},
        },
        "verdict": "PASS",
    }


class Spec107FaultSchemaTest(unittest.TestCase):
    def _orchestrator(self, root: Path) -> LiveFaultOrchestrator:
        digests = {
            "source": digest("1"), "profile": digest("2"), "model": digest("3"),
            "plan": digest("4"), "artifact": digest("5"), "lineage": digest("6"),
            "workload": digest("7"), "tokenizer": digest("8"),
            "trustPolicy": digest("9"), "command": digest("a"),
        }
        candidate = build_candidate_identity(digests, created_at="2026-07-12T00:00:00Z")
        campaign = build_campaign_identity(
            candidate, kind="fault", ordinal=1, command_digest=digest("b"),
            output_root="results/spec107-c1-fault-matrix-r1")
        return LiveFaultOrchestrator(
            candidate=candidate, campaign=campaign, repo_root=root)

    def test_owned_process_round_trip_requires_full_process_identity(self) -> None:
        process = OwnedProcessV1.from_dict(process_payload())
        self.assertEqual(process.to_dict(), process_payload())
        for field, invalid in (
            ("pid", 0), ("processGroupId", -1), ("procStartTimeTicks", 0),
            ("providerBootId", ""), ("commandDigest", "sha256:short"),
        ):
            with self.subTest(field=field):
                payload = process_payload()
                payload[field] = invalid
                with self.assertRaises(ValueError):
                    OwnedProcessV1.from_dict(payload)

    def test_live_fault_record_round_trip_preserves_authority_and_cleanup(self) -> None:
        value = LiveFaultRecordV1.from_dict(fault_payload())
        self.assertEqual(value.to_dict(), fault_payload())
        self.assertEqual(value.target.provider_boot_id, "boot-1")
        self.assertTrue(value.cleanup.proven)

    def test_executed_fault_requires_real_network_injection_and_observed_effect(self) -> None:
        for field, invalid, reason in (
            ("networkInjection", False, "FAULT_NETWORK_INJECTION_REQUIRED"),
            ("injectionApplied", False, "FAULT_INJECTION_NOT_APPLIED"),
            ("observedEffect", "", "FAULT_OBSERVED_EFFECT_MISSING"),
        ):
            payload = fault_payload()
            payload[field] = invalid
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, reason):
                    LiveFaultRecordV1.from_dict(payload)

    def test_rejects_second_replacement_extended_deadline_and_multiple_authorities(self) -> None:
        cases = (
            ("replacementCount", 2, "FAULT_REPLACEMENT_BOUND_EXCEEDED"),
            ("currentDeadlineEpochMs", 5001, "FAULT_DEADLINE_CHANGED"),
            ("authoritativeTerminalCount", 0, "FAULT_TERMINAL_AUTHORITY_INVALID"),
            ("authoritativeTerminalCount", 2, "FAULT_TERMINAL_AUTHORITY_INVALID"),
            ("cancelSupersedeAuthenticated", False, "FAULT_SUPERSEDE_UNAUTHENTICATED"),
        )
        for field, invalid, reason in cases:
            payload = fault_payload()
            payload[field] = invalid
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, reason):
                    LiveFaultRecordV1.from_dict(payload)

    def test_rejects_stale_attempt_and_wrong_boot_authority(self) -> None:
        stale = fault_payload()
        stale["attemptEpochBefore"] = 2
        stale["attemptEpochAfter"] = 1
        with self.assertRaisesRegex(ValueError, "FAULT_ATTEMPT_EPOCH_STALE"):
            LiveFaultRecordV1.from_dict(stale)

        wrong_boot = fault_payload()
        wrong_boot["providerBootBefore"] = "boot-other"
        with self.assertRaisesRegex(ValueError, "FAULT_TARGET_BOOT_MISMATCH"):
            LiveFaultRecordV1.from_dict(wrong_boot)

    def test_cleanup_must_be_proven_and_return_to_declared_bounds(self) -> None:
        unproven = fault_payload()
        unproven["cleanup"]["proven"] = False  # type: ignore[index]
        with self.assertRaisesRegex(ValueError, "FAULT_CLEANUP_UNPROVEN"):
            LiveFaultRecordV1.from_dict(unproven)

        leaked = fault_payload()
        leaked["cleanup"]["after"]["waits"] = 1  # type: ignore[index]
        with self.assertRaisesRegex(ValueError, "FAULT_CLEANUP_BOUND_EXCEEDED:waits"):
            LiveFaultRecordV1.from_dict(leaked)

    def test_forbidden_payload_token_tensor_kv_and_secret_fields_are_rejected(self) -> None:
        for field in ("payload", "token", "tensor", "kv", "privateKey", "secret"):
            payload = copy.deepcopy(fault_payload())
            payload[field] = "forbidden"
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, "EVIDENCE_FORBIDDEN_FIELD"):
                    LiveFaultRecordV1.from_dict(payload)

    def test_registry_requires_exact_live_proc_identity_before_signal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            registry = OwnedProcessRegistry(
                campaign_id="spec107-c1-fault-r1-aaaaaaaaaaaa",
                registry_path=Path(tmp) / "owned-processes.json",
            )
            owned = registry.launch(
                ["/usr/bin/sleep", "30"], role="/LLM/Stage/1",
                provider_name="/provider/1", provider_boot_id="boot-1",
            )
            try:
                for field, value in (
                    ("procStartTimeTicks", owned.proc_start_time_ticks + 1),
                    ("processGroupId", owned.process_group_id + 1),
                    ("providerBootId", "boot-other"),
                    ("commandDigest", digest("f")),
                ):
                    payload = owned.to_dict()
                    payload[field] = value
                    forged = OwnedProcessV1.from_dict(payload)
                    with self.subTest(field=field):
                        with self.assertRaisesRegex(
                            FaultControllerError, "OWNED_PROCESS_.*MISMATCH"
                        ):
                            registry.guarded_signal(forged, 0)
                self.assertIsNone(registry.guarded_signal(owned, 0))
                self.assertTrue(Path(f"/proc/{owned.pid}").exists())
            finally:
                registry.cleanup()

    def test_restart_changes_boot_and_is_bounded_to_one_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            registry = OwnedProcessRegistry(
                campaign_id="spec107-c1-fault-r1-aaaaaaaaaaaa",
                registry_path=Path(tmp) / "owned-processes.json",
            )
            original = registry.launch(
                ["/usr/bin/sleep", "30"], role="/LLM/Stage/1",
                provider_name="/provider/1", provider_boot_id="boot-1",
            )
            replacement = registry.restart(original, provider_boot_id="boot-2")
            try:
                self.assertNotEqual(replacement.pid, original.pid)
                self.assertEqual(replacement.provider_boot_id, "boot-2")
                with self.assertRaisesRegex(
                    FaultControllerError, "OWNED_PROCESS_REPLACEMENT_BOUND"
                ):
                    registry.restart(replacement, provider_boot_id="boot-3")
            finally:
                proof = registry.cleanup()
            self.assertTrue(proof["proven"])
            self.assertEqual(proof["remainingPids"], [])

    def test_trigger_and_effect_must_be_observed_on_owned_process(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "provider.log"
            registry = OwnedProcessRegistry(
                campaign_id="spec107-c1-fault-r1-aaaaaaaaaaaa",
                registry_path=root / "owned-processes.json",
            )
            owned = registry.launch(
                ["/usr/bin/sleep", "30"], role="/LLM/Stage/1",
                provider_name="/provider/1", provider_boot_id="boot-1",
            )
            try:
                log.write_text("STAGE1_ACTIVE\n", encoding="utf-8")
                trigger_us = registry.wait_for_log_trigger(
                    owned, log_path=log, marker="STAGE1_ACTIVE", timeout_seconds=1)
                self.assertGreater(trigger_us, 0)
                registry.guarded_signal(owned, 15)
                effect = registry.observe_process_exit(owned, timeout_seconds=2)
                self.assertTrue(effect["observed"])
                self.assertEqual(effect["effect"], "provider-process-exit")
            finally:
                registry.cleanup()

    def test_adoption_rejects_nonexclusive_process_group(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            registry = OwnedProcessRegistry(
                campaign_id="spec107-c1-fault-r1-aaaaaaaaaaaa",
                registry_path=Path(tmp) / "owned-processes.json",
            )
            process = subprocess.Popen(["/usr/bin/sleep", "30"])
            try:
                with self.assertRaisesRegex(
                    FaultControllerError, "OWNED_PROCESS_GROUP_NOT_EXCLUSIVE"):
                    registry.adopt(
                        process, role="/LLM/Stage/1", provider_name="/provider/1",
                        provider_boot_id="boot-1")
            finally:
                process.terminate()
                process.wait(timeout=2)

    def test_normal_provider_source_exposes_no_fault_option(self) -> None:
        source = (REPO / "examples" / "DI_NativeProviderExecutable.cpp").read_text(
            encoding="utf-8")
        self.assertNotIn("--fault-", source)
        self.assertNotIn("NDNSF_DI_EXPERIMENT_FAULT", source)

    def test_live_matrix_is_fixed_exclusive_and_cleanup_stopped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            orchestrator = self._orchestrator(root)
            matrix = orchestrator.preregistration()
            self.assertEqual(
                [row["cellId"] for row in matrix["orderedCells"]], list(FAULT_CELLS))
            lock = orchestrator.lock(
                root / "specs/107-ndnsf-di-minindn-gate-recovery/evidence/fault.json")
            self.assertTrue(lock.is_file())
            with self.assertRaisesRegex(
                LiveFaultOrchestrationError, "FAULT_MATRIX_ALREADY_LOCKED"):
                orchestrator.lock(lock)
            cell = orchestrator.claim_cell("positive-control")
            with self.assertRaisesRegex(
                LiveFaultOrchestrationError, "FAULT_CELL_ALREADY_CONSUMED"):
                orchestrator.claim_cell("positive-control")
            self.assertTrue(cell.with_name(cell.name + ".claim.json").is_file())
            self.assertFalse(cell.exists())
            claim = validate_cell_claim(
                cell_id="positive-control",
                candidate_id=orchestrator.candidate["candidateId"],
                campaign_id=orchestrator.campaign["campaignId"],
                output_root=cell)
            self.assertEqual(claim["cellId"], "positive-control")
            with self.assertRaisesRegex(
                LiveFaultOrchestrationError, "FAULT_CELL_CLAIM_IDENTITY_MISMATCH"):
                validate_cell_claim(
                    cell_id="positive-control", candidate_id="spec107-c1-forged",
                    campaign_id=orchestrator.campaign["campaignId"],
                    output_root=cell)

    def test_fault_control_requires_observed_provider_marker(self) -> None:
        with self.assertRaisesRegex(
            LiveFaultOrchestrationError, "FAULT_PROVIDER_MARKER_NOT_OBSERVED"):
            derive_fault_provider_control(
                cell_id="missing-segment", marker_observed=False)
        applied = derive_fault_provider_control(
            cell_id="missing-segment", marker_observed=True)
        self.assertTrue(applied["injectionApplied"])
        self.assertTrue(applied["networkInjection"])
        control = derive_fault_provider_control(
            cell_id="positive-control", marker_observed=False)
        self.assertFalse(control["injectionApplied"])


if __name__ == "__main__":
    unittest.main()
