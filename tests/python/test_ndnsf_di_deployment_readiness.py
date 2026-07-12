#!/usr/bin/env python3
"""Spec 105 deployment-readiness contract tests."""

from __future__ import annotations

import contextlib
from concurrent.futures import Future
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

from ndnsf_distributed_inference.runtime_v1 import (
    ExecutionEvidenceV1,
    classify_execution_evidence,
)
from ndnsf_distributed_inference.deployment import (
    BoundedRecoveryController,
    RecoveryReason,
)
from ndnsf_distributed_inference.release_gate import (
    DIMENSIONS,
    build_release_gate,
    verify_evidence_manifest,
)
from ndnsf_distributed_inference.qwen_pilot import (
    BoundedGenerationScheduler,
    CacheResolution,
    GenerationQueueFull,
    QwenPilotRequest,
    QwenPilotOrchestrator,
    QwenPilotTerminalError,
    compare_token_sequences,
    greedy_decode_fixture,
    resolve_cache_request,
)


def evidence(kind: str, *, real: bool, artifact: str = "sha256:a") -> ExecutionEvidenceV1:
    return ExecutionEvidenceV1.from_dict({
        "schema": "ndnsf-di-execution-evidence-v1",
        "providerName": "/provider/A",
        "providerBootId": "boot-a",
        "evidenceEpoch": 1,
        "runnerKind": kind,
        "realCompute": real,
        "device": {"kind": "cuda" if "cuda" in kind else "cpu",
                   "id": "GPU-1" if "cuda" in kind else "cpu0"},
        "runtimeVersion": "runtime-v1",
        "modelDigest": "sha256:model",
        "planDigest": "sha256:plan",
        "artifactDigests": {"/LLM/Stage/0": artifact},
        "roles": ["/LLM/Stage/0"],
        "createdAtMs": 1,
    })


def evidence_payload(value: ExecutionEvidenceV1) -> dict[str, object]:
    return {
        "schema": value.schema,
        "providerName": value.provider_name,
        "providerBootId": value.provider_boot_id,
        "evidenceEpoch": value.evidence_epoch,
        "runnerKind": value.runner_kind.value,
        "realCompute": value.real_compute,
        "device": {"kind": value.device_kind, "id": value.device_id},
        "runtimeVersion": value.runtime_version,
        "modelDigest": value.model_digest,
        "planDigest": value.plan_digest,
        "artifactDigests": value.artifact_digests,
        "roles": list(value.roles),
        "createdAtMs": value.created_at_ms,
    }


class DeploymentReadinessContractsTest(unittest.TestCase):
    def test_release_gate_manifest_rejects_missing_and_tampered_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            proof = root / "proof.md"
            proof.write_text("immutable proof\n", encoding="utf-8")
            digest = hashlib.sha256(proof.read_bytes()).hexdigest()
            payload = {
                "evidence_root": str(root),
                "evidence_manifest": [{"path": "proof.md", "sha256": "sha256:" + digest}],
                "dimensions": {
                    name: {"status": "PASS", "artifacts": ["proof.md"]}
                    for name in DIMENSIONS
                },
            }
            self.assertEqual(verify_evidence_manifest(payload), [])
            proof.write_text("tampered\n", encoding="utf-8")
            self.assertEqual(
                verify_evidence_manifest(payload),
                ["EVIDENCE_DIGEST_MISMATCH:proof.md", "EVIDENCE_UNBOUND:proof.md"],
            )
            proof.unlink()
            errors = verify_evidence_manifest(payload)
            self.assertIn("EVIDENCE_MISSING:proof.md", errors)

    def test_runtime_cli_separates_real_adapters_from_contract_smoke(self) -> None:
        cli = [sys.executable, "-m", "ndnsf_distributed_inference.runtime_v1"]
        legacy = subprocess.run(
            cli + ["run", "--plan", "/tmp/legacy-plan.json"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual(legacy.returncode, 2)
        self.assertIn("contract-smoke", legacy.stderr)

        smoke = subprocess.run(
            cli + ["contract-smoke", "schema-sample"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual(smoke.returncode, 0, smoke.stderr)
        self.assertEqual(json.loads(smoke.stdout)["status"], "contract-smoke")

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            adapter = Path(__file__).resolve().parents[1] / "fixtures" / "spec105_production_adapter.py"
            profile = root / "deployment.json"
            status_file = root / "status.json"
            metrics_file = root / "metrics.json"
            sampled_at_ms = int(time.time() * 1000)
            status_file.write_text(json.dumps({
                "schema": "ndnsf-di-status-v1", "ready": True,
                "sampledAtMs": sampled_at_ms, "releaseId": "release-r1",
                "planId": "plan-r1", "evidenceEpoch": 1,
            }), encoding="utf-8")
            metrics_file.write_text(json.dumps({
                "schema": "ndnsf-di-metrics-v1", "sampledAtMs": sampled_at_ms,
                "releaseId": "release-r1", "planId": "plan-r1", "evidenceEpoch": 1,
                "counters": {"requests_completed_total": 3},
                "gauges": {"queue_depth": 0},
                "labels": {"provider": "local"},
            }), encoding="utf-8")
            profile.write_text(json.dumps({
                "deployment": {
                    "harness": "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
                    "status_file": str(status_file),
                    "metrics_file": str(metrics_file),
                    "telemetry_max_age_ms": 2000,
                    "release_id": "release-r1", "plan_id": "plan-r1",
                    "evidence_epoch": 1,
                    "provider_command": [
                        sys.executable, str(adapter), "provider",
                        "--profile", "{profile}",
                    ],
                    "run_command": [
                        sys.executable, str(adapter), "run",
                        "--profile", "{profile}", "--plan", "{plan}",
                        "--request", "{request}", "--out", "{out}",
                    ],
                },
            }), encoding="utf-8")
            request = root / "request.json"
            request.write_text("{}", encoding="utf-8")
            plan = root / "plan.json"
            plan.write_text("{}", encoding="utf-8")
            campaign = root / "campaign.json"
            campaign.write_text(json.dumps({
                "command": [
                    sys.executable, str(adapter), "bench",
                    "--campaign", "{campaign}", "--out", "{out}",
                ],
            }), encoding="utf-8")
            cases = [
                (["provider", "--profile", str(profile), "--dry-run"],
                 "spec105_production_adapter.py"),
                (["run", "--profile", str(profile), "--plan", str(plan),
                  "--request", str(request), "--out", str(root / "result.json"),
                  "--dry-run"], "spec105_production_adapter.py"),
                (["bench", "--campaign", str(campaign), "--out", str(root / "bench"),
                  "--dry-run"], "spec105_production_adapter.py"),
            ]
            for args, expected_adapter in cases:
                with self.subTest(command=args[0]):
                    proc = subprocess.run(
                        cli + args, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, check=False)
                    self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
                    payload = json.loads(proc.stdout)
                    self.assertEqual(payload["mode"], "production-adapter")
                    self.assertIn(expected_adapter, " ".join(payload["command"]))

            provider_run = subprocess.run(
                cli + ["provider", "--profile", str(profile)], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(provider_run.returncode, 0, provider_run.stderr)
            self.assertEqual(json.loads(provider_run.stdout)["mode"], "provider")
            result_out = root / "result.json"
            run_result = subprocess.run(
                cli + ["run", "--profile", str(profile), "--plan", str(plan),
                       "--request", str(request), "--out", str(result_out)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)
            self.assertEqual(json.loads(result_out.read_text())["mode"], "run")
            bench_out = root / "bench-result.json"
            bench_result = subprocess.run(
                cli + ["bench", "--campaign", str(campaign), "--out", str(bench_out)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(bench_result.returncode, 0, bench_result.stderr)
            self.assertEqual(json.loads(bench_out.read_text())["mode"], "bench")

            default_campaign = root / "default-campaign.json"
            default_campaign.write_text(json.dumps({
                "runner": "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
                "campaignId": "fixture-campaign",
                "performance": {"measurementSeconds": 60, "offeredRps": 1.0},
            }), encoding="utf-8")
            default_bench = subprocess.run(
                cli + ["bench", "--campaign", str(default_campaign),
                       "--out", str(root / "default-bench"), "--dry-run"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(default_bench.returncode, 0, default_bench.stderr)
            default_command = json.loads(default_bench.stdout)["command"]
            self.assertIn("--runtime", default_command)
            self.assertIn("qwen-onnx-cpu-native", default_command)
            self.assertNotIn("--runner-mode", default_command)

            broken = root / "broken-profile.json"
            broken.write_text(json.dumps({"deployment": {
                "run_command": [sys.executable, str(adapter), "run", "--plan", "{plan}"],
            }}), encoding="utf-8")
            rejected = subprocess.run(
                cli + ["run", "--profile", str(broken), "--plan", str(plan),
                       "--request", str(request), "--out", str(root / "bad.json")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("does not consume", rejected.stderr)

            model = root / "model.json"
            providers = root / "providers.json"
            model.write_text(json.dumps({
                "modelId": "qwen-test", "revision": "r1", "modelFamily": "llm",
                "layers": 2, "memoryPerLayerMb": 1.0, "flopsPerLayerTflop": 0.01,
            }), encoding="utf-8")
            providers.write_text(json.dumps({"providers": [{
                "provider": "local", "gpuMemoryMb": 0, "ramMemoryMb": 8192,
                "flopsTflops": 1.0, "llmStageCapacityMb": 1024,
            }]}), encoding="utf-8")
            explain = root / "plan-explain.json"
            planned = subprocess.run(
                cli + ["plan", "--model", str(model), "--providers", str(providers),
                       "--out", str(root / "planned.json"), "--explain", str(explain)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(planned.returncode, 0, planned.stderr)
            self.assertEqual(json.loads(explain.read_text())["schema"],
                             "ndnsf-di-plan-explain-v1")

            fresh_ms = int(time.time() * 1000)
            fresh_status = json.loads(status_file.read_text())
            fresh_status["sampledAtMs"] = fresh_ms
            status_file.write_text(json.dumps(fresh_status), encoding="utf-8")
            fresh_metrics = json.loads(metrics_file.read_text())
            fresh_metrics["sampledAtMs"] = fresh_ms
            metrics_file.write_text(json.dumps(fresh_metrics), encoding="utf-8")
            status = subprocess.run(
                cli + ["status", "--profile", str(profile), "--json"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(status.returncode, 0, status.stderr)
            self.assertTrue(json.loads(status.stdout)["ready"])
            metrics_out = root / "metrics.prom"
            metrics = subprocess.run(
                cli + ["metrics", "--profile", str(profile),
                       "--format", "prometheus-textfile", "--out", str(metrics_out)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(metrics.returncode, 0, metrics.stderr)
            rendered = metrics_out.read_text(encoding="utf-8")
            self.assertIn("ndnsf_di_requests_completed_total", rendered)
            self.assertIn('provider="local"', rendered)
            self.assertFalse(list(root.glob(".metrics.prom.*")))

            stale_status = json.loads(status_file.read_text())
            stale_status["sampledAtMs"] = 1
            status_file.write_text(json.dumps(stale_status), encoding="utf-8")
            stale = subprocess.run(
                cli + ["status", "--profile", str(profile), "--json"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn("SNAPSHOT_STALE", json.loads(stale.stdout)["errors"])
            metrics_file.unlink()
            missing_metrics = subprocess.run(
                cli + ["metrics", "--profile", str(profile), "--out", str(root / "missing.json")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertNotEqual(missing_metrics.returncode, 0)
            self.assertFalse((root / "missing.json").exists())

    def test_systemd_package_contract_is_hardened_and_reversible(self) -> None:
        root = Path(__file__).resolve().parents[2] / "packaging" / "ndnsf-di-systemd"
        required = [
            "units/ndnsf-di-controller.service",
            "units/ndnsf-di-provider@.service",
            "units/ndnsf-di-repo@.service",
            "units/ndnsf-di-bench.service",
            "units/ndnsf-di-controller.target",
            "units/ndnsf-di-providers.target",
            "config/ndnsf-di.tmpfiles.conf",
            "config/ndnsf-di.logrotate",
            "config/deployment.example.json",
            "install.sh",
            "rollback.sh",
            "create-release.sh",
            "README.md",
            "README_ch.md",
        ]
        missing = [name for name in required if not (root / name).is_file()]
        self.assertEqual(missing, [])
        units = "\n".join(
            path.read_text(encoding="utf-8") for path in (root / "units").glob("*")
            if path.is_file())
        for directive in (
            "User=ndnsf-di", "NoNewPrivileges=true", "ProtectSystem=strict",
            "PrivateTmp=true", "Restart=on-failure", "RestartSec=",
            "TimeoutStopSec=", "After=", "Requires=",
        ):
            self.assertIn(directive, units)
        install = (root / "install.sh").read_text(encoding="utf-8")
        rollback = (root / "rollback.sh").read_text(encoding="utf-8")
        self.assertIn("sha256sum", install)
        self.assertIn("current", install)
        self.assertIn("previous", rollback)
        self.assertIn("authoritative", install.lower())
        self.assertNotIn("rm -rf /var/lib/ndn", install + rollback)
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run(
                [str(root / "validate-staging.sh"), "--work-root",
                 str(Path(tmp) / "stage")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            self.assertIn("STAGING_PASS", proc.stdout)
            self.assertIn("authoritative Repo preserved", proc.stdout)

    def test_bounded_recovery_replaces_provider_once_and_rejects_old_result(self) -> None:
        for reason in (
            RecoveryReason.PROVIDER_LOST,
            RecoveryReason.STRAGGLER_DEADLINE,
            RecoveryReason.TELEMETRY_STALE,
        ):
            with self.subTest(reason=reason):
                recovery = BoundedRecoveryController(
                    "request-1", request_deadline_ms=2_000,
                    started_at_ms=1_000, max_replacements=1)
                first = recovery.start("/provider/A")
                action = recovery.recover(
                    reason, at_ms=1_200, replacement_provider="/provider/B")
                self.assertEqual(action.action, "replace")
                self.assertEqual(action.attempt_epoch, 2)
                self.assertEqual(action.remaining_deadline_ms, 800)
                self.assertEqual(action.provider, "/provider/B")
                self.assertEqual(action.control_payloads[0]["operation"], "CANCEL")
                self.assertEqual(action.control_payloads[1]["operation"], "SUPERSEDE")
                self.assertFalse(recovery.accept_result(first.attempt_epoch, b"old"))
                self.assertTrue(recovery.accept_result(action.attempt_epoch, b"new"))
                self.assertFalse(recovery.accept_result(action.attempt_epoch, b"duplicate"))

    def test_cache_miss_retries_full_context_with_new_attempt_epoch(self) -> None:
        recovery = BoundedRecoveryController(
            "request-cache", request_deadline_ms=5_000,
            started_at_ms=1_000, max_replacements=1)
        recovery.start("/provider/A")
        action = recovery.recover(
            RecoveryReason.CACHE_MISS_FULL_CONTEXT_REQUIRED, at_ms=1_100)
        self.assertEqual(action.action, "retry-full-context")
        self.assertEqual(action.provider, "/provider/A")
        self.assertEqual(action.attempt_epoch, 2)
        self.assertTrue(action.full_context_required)

    def test_recovery_fails_exactly_for_no_replacement_and_deadline(self) -> None:
        no_replacement = BoundedRecoveryController(
            "request-none", request_deadline_ms=2_000, started_at_ms=1_000)
        no_replacement.start("/provider/A")
        terminal = no_replacement.recover(
            RecoveryReason.PROVIDER_LOST, at_ms=1_100)
        self.assertEqual(terminal.action, "fail")
        self.assertEqual(terminal.terminal_reason,
                         RecoveryReason.NO_COMPATIBLE_REPLACEMENT)
        self.assertFalse(no_replacement.accept_result(1, b"late"))

        expired = BoundedRecoveryController(
            "request-expired", request_deadline_ms=2_000, started_at_ms=1_000)
        expired.start("/provider/A")
        deadline = expired.recover(
            RecoveryReason.STRAGGLER_DEADLINE,
            at_ms=2_000, replacement_provider="/provider/B")
        self.assertEqual(deadline.action, "fail")
        self.assertEqual(deadline.terminal_reason,
                         RecoveryReason.REQUEST_DEADLINE)
        self.assertEqual(deadline.remaining_deadline_ms, 0)

    def test_second_replacement_is_bounded_and_control_uses_existing_payload(self) -> None:
        recovery = BoundedRecoveryController(
            "request-bounded", request_deadline_ms=5_000,
            started_at_ms=1_000, max_replacements=1)
        recovery.start("/provider/A")
        first = recovery.recover(
            RecoveryReason.PROVIDER_LOST,
            at_ms=1_100, replacement_provider="/provider/B")
        self.assertEqual(first.control_payloads[0]["schema"],
                         "ndnsf-di-execution-control-v1")
        second = recovery.recover(
            RecoveryReason.PROVIDER_LOST,
            at_ms=1_200, replacement_provider="/provider/C")
        self.assertEqual(second.action, "fail")
        self.assertEqual(second.terminal_reason,
                         RecoveryReason.NO_COMPATIBLE_REPLACEMENT)

    def test_native_open_loop_driver_reports_generation_level_progress(self) -> None:
        pipeline_dir = (
            Path(__file__).resolve().parents[2] /
            "examples/python/NDNSF-DistributedInference/llm_pipeline"
        )
        sys.path.insert(0, str(pipeline_dir))
        try:
            spec = importlib.util.spec_from_file_location(
                "spec105_qwen_user_fixture", pipeline_dir / "user.py")
            self.assertIsNotNone(spec)
            self.assertIsNotNone(spec.loader)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        finally:
            sys.path.pop(0)

        calls: list[tuple[str, int, dict]] = []
        module._native_step_payload = (
            lambda _context, _manifest, token_index, _mode:
            str(token_index).encode("ascii")
        )

        def requirements(_manifest, session_id, token_index, _mode):
            calls.append((session_id, token_index, {}))
            return {}

        module._native_role_requirements = requirements

        class Result:
            status = True
            error = ""
            payload = b"logits"

        module._decode_native_tensor_bundle = (
            lambda _payload: {"logits": [[[0.0, 1.0]]]}
        )

        class Client:
            shutdown_wait = None

            def async_distributed_inference(self, *_args, **kwargs):
                self_outer.assertNotIn("on_result", kwargs)
                self_outer.assertNotIn("on_error", kwargs)
                future = Future()
                future.set_result(Result())
                return future

            def shutdown(self, wait=True):
                self.shutdown_wait = wait

        self_outer = self
        client = Client()
        args = SimpleNamespace(
            request_interval_ms=1.0,
            measured_duration_s=0.002,
            max_new_tokens=2,
            native_first_kv_mode="full-context",
            request_id="scheduler-fixture",
            ack_timeout_ms=1500,
            timeout_ms=120000,
            campaign_id="spec105-r1-scheduler-fixture",
        )
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = module._run_native_open_loop(
                client,
                args,
                {"inputIds": [[7]], "attentionMask": [[1]]},
                {"stages": []},
                [1, 1],
                None,
            )

        self.assertEqual(result, 0)
        self.assertEqual(client.shutdown_wait, False)
        per_session: dict[str, list[int]] = {}
        for session_id, token_index, _ in calls:
            per_session.setdefault(session_id, []).append(token_index)
        self.assertEqual(per_session, {
            "scheduler-fixture-open-0": [0, 1],
            "scheduler-fixture-open-1": [0, 1],
        })
        rendered = output.getvalue()
        self.assertIn("offered=2 completed=2 failed=0 unfinished=0", rendered)
        self.assertIn("generationWorkers=4", rendered)
        self.assertIn("campaignId=spec105-r1-scheduler-fixture", rendered)
        self.assertIn("expectedTokenCount=2", rendered)
        self.assertIn("expectedTokenDigest=", rendered)
        self.assertNotIn("expectedTokens=", rendered)
        self.assertIn(
            'tokenProgress={"scheduler-fixture-open-0":2,'
            '"scheduler-fixture-open-1":2}', rendered)

    def test_generation_scheduler_owns_full_generation_before_next_job(self) -> None:
        observed: list[tuple[str, int]] = []
        scheduler = BoundedGenerationScheduler(max_workers=1, max_queued=1)

        def generation(session_id: str):
            def run(report_progress):
                for token_count in range(1, 4):
                    observed.append((session_id, token_count))
                    report_progress(token_count)
                return session_id
            return run

        first = scheduler.submit("generation-0", generation("generation-0"))
        second = scheduler.submit("generation-1", generation("generation-1"))
        self.assertEqual(first.result(timeout=1), "generation-0")
        self.assertEqual(second.result(timeout=1), "generation-1")
        scheduler.shutdown()

        self.assertEqual(observed, [
            ("generation-0", 1),
            ("generation-0", 2),
            ("generation-0", 3),
            ("generation-1", 1),
            ("generation-1", 2),
            ("generation-1", 3),
        ])

    def test_generation_scheduler_bounds_queue_and_reports_occupancy(self) -> None:
        started = threading.Event()
        release = threading.Event()
        start_lock = threading.Lock()
        start_count = 0
        scheduler = BoundedGenerationScheduler(max_workers=2, max_queued=2)

        def blocked(report_progress):
            nonlocal start_count
            report_progress(1)
            with start_lock:
                start_count += 1
                if start_count == 2:
                    started.set()
            release.wait(timeout=1)
            return "ok"

        futures = [
            scheduler.submit(f"generation-{index}", blocked)
            for index in range(4)
        ]
        self.assertTrue(started.wait(timeout=1))
        snapshot = scheduler.snapshot()
        self.assertEqual(snapshot.active, 2)
        self.assertEqual(snapshot.queued, 2)
        self.assertEqual(snapshot.max_active_observed, 2)
        self.assertEqual(snapshot.max_queued_observed, 2)
        self.assertEqual(snapshot.token_progress["generation-0"], 1)
        self.assertEqual(snapshot.token_progress["generation-1"], 1)
        with self.assertRaises(GenerationQueueFull):
            scheduler.submit("generation-overflow", blocked)

        release.set()
        self.assertEqual([future.result(timeout=1) for future in futures], ["ok"] * 4)
        scheduler.shutdown()

    def test_generation_scheduler_rejects_progress_regression_and_reports_terminals(self) -> None:
        scheduler = BoundedGenerationScheduler(max_workers=1, max_queued=1)

        def regressing(report_progress):
            report_progress(2)
            report_progress(1)

        failed = scheduler.submit("generation-failed", regressing)
        with self.assertRaisesRegex(ValueError, "must increase"):
            failed.result(timeout=1)
        completed = scheduler.submit(
            "generation-complete",
            lambda report_progress: report_progress(3) or "ok",
        )
        self.assertEqual(completed.result(timeout=1), "ok")
        snapshot = scheduler.snapshot()
        scheduler.shutdown()

        self.assertEqual(snapshot.completed, 1)
        self.assertEqual(snapshot.failed, 1)
        self.assertEqual(snapshot.unfinished, 0)
        self.assertEqual(snapshot.token_progress, {
            "generation-failed": 2,
            "generation-complete": 3,
        })

    def test_qwen_pilot_greedy_token_fixtures_1_2_and_32(self) -> None:
        logits = [[-1.0, float(index), 100.0 + index] for index in range(32)]
        self.assertEqual(greedy_decode_fixture(logits, 1), [2])
        self.assertEqual(greedy_decode_fixture(logits, 2), [2, 2])
        self.assertEqual(greedy_decode_fixture(logits, 32), [2] * 32)

    def test_qwen_pilot_admission_enforces_input_and_output_bounds(self) -> None:
        QwenPilotRequest(tuple(range(512)), 32).validate()
        with self.assertRaises(ValueError):
            QwenPilotRequest(tuple(range(513)), 1).validate()
        with self.assertRaises(ValueError):
            QwenPilotRequest((1,), 33).validate()
        with self.assertRaises(ValueError):
            QwenPilotRequest((), 1).validate()

    def test_qwen_pilot_cache_hit_rebuild_and_delta_only_failure(self) -> None:
        self.assertEqual(
            resolve_cache_request(cache_present=True, full_context_present=False,
                                  delta_only=True),
            CacheResolution.HIT,
        )
        self.assertEqual(
            resolve_cache_request(cache_present=False, full_context_present=True,
                                  delta_only=False),
            CacheResolution.FULL_CONTEXT_REBUILD,
        )
        with self.assertRaises(QwenPilotTerminalError) as caught:
            resolve_cache_request(cache_present=False, full_context_present=False,
                                  delta_only=True)
        self.assertEqual(caught.exception.reason,
                         "CACHE_MISS_FULL_CONTEXT_REQUIRED")

    def test_qwen_pilot_tokenization_orchestration_and_exact_comparison(self) -> None:
        contexts: list[tuple[int, ...]] = []
        orchestrator = QwenPilotOrchestrator(
            tokenizer=lambda prompt: [len(prompt), 7],
            staged_logits=lambda context: (
                contexts.append(context) or [0.0, 1.0, float(len(context))]
            ),
        )
        request = orchestrator.request("pilot", 2)
        actual = orchestrator.generate(request)
        self.assertEqual(actual, [2, 2])
        self.assertEqual(contexts, [(5, 7), (5, 7, 2)])
        compare_token_sequences([2, 2], actual)
        with self.assertRaises(QwenPilotTerminalError) as mismatch:
            compare_token_sequences([2, 1], actual)
        self.assertIn("index=1", mismatch.exception.reason)

    def test_release_gate_blocks_missing_synthetic_mixed_and_digest_mismatch(self) -> None:
        dimensions = {name: {"status": "PASS", "artifacts": [f"{name}.json"]}
                      for name in DIMENSIONS}
        real = evidence("onnxruntime-cuda", real=True)
        passing = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions,
            execution_evidence=[evidence_payload(real)])
        self.assertEqual(passing["minindnCandidateOverall"], "PASS")
        self.assertEqual(passing["physicalProductionOverall"], "DEFERRED")
        missing = build_release_gate(release_id="r", source_commit="c",
                                     profile_digest="sha256:p", dimensions=dimensions,
                                     execution_evidence=[])
        self.assertEqual(missing["minindnCandidateOverall"], "BLOCK")
        synthetic = evidence("synthetic-delay", real=False)
        blocked = build_release_gate(release_id="r", source_commit="c",
                                     profile_digest="sha256:p", dimensions=dimensions,
                                     execution_evidence=[evidence_payload(synthetic)])
        self.assertEqual(blocked["minindnCandidateOverall"], "BLOCK")

        mixed = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions,
            execution_evidence=[
                evidence_payload(evidence("onnxruntime-cpu", real=True)),
                evidence_payload(evidence("onnxruntime-cuda", real=True)),
            ],
        )
        self.assertEqual(mixed["minindnCandidateOverall"], "BLOCK")

        digest_mismatch = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions,
            execution_evidence=[
                evidence_payload(evidence("onnxruntime-cuda", real=True,
                                          artifact="sha256:a")),
                evidence_payload(evidence("onnxruntime-cuda", real=True,
                                          artifact="sha256:b")),
            ],
        )
        self.assertEqual(digest_mismatch["minindnCandidateOverall"], "BLOCK")

        contradictory = evidence_payload(evidence("synthetic-delay", real=False))
        contradictory["realCompute"] = True
        contradiction_gate = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions, execution_evidence=[contradictory],
        )
        self.assertEqual(contradiction_gate["minindnCandidateOverall"], "BLOCK")

    def test_release_gate_blocks_missing_dimension_artifact(self) -> None:
        dimensions = {name: {"status": "PASS", "artifacts": ["proof"]}
                      for name in DIMENSIONS}
        dimensions["operations"] = {"status": "PASS", "artifacts": []}
        gate = build_release_gate(release_id="r", source_commit="c",
                                  profile_digest="sha256:p", dimensions=dimensions,
                                  execution_evidence=[])
        self.assertEqual(gate["dimensions"]["operations"]["status"], "BLOCK")
        self.assertEqual(gate["minindnCandidateOverall"], "BLOCK")

    def test_synthetic_wiring_cpu_and_cuda_classify_distinctly(self) -> None:
        self.assertEqual(classify_execution_evidence([evidence("synthetic-delay", real=False)]),
                         "synthetic-delay")
        self.assertEqual(classify_execution_evidence([evidence("wiring-only", real=False)]),
                         "wiring-only")
        self.assertEqual(classify_execution_evidence([evidence("onnxruntime-cpu", real=True)]),
                         "onnxruntime-cpu")
        self.assertEqual(classify_execution_evidence([evidence("onnxruntime-cuda", real=True)]),
                         "onnxruntime-cuda")

    def test_missing_mixed_and_contradictory_evidence_fail(self) -> None:
        self.assertEqual(classify_execution_evidence([]), "invalid-evidence")
        self.assertEqual(classify_execution_evidence([
            evidence("onnxruntime-cpu", real=True),
            evidence("onnxruntime-cuda", real=True),
        ]), "invalid-evidence")
        with self.assertRaises(ValueError):
            evidence("synthetic-delay", real=True)

    def test_artifact_mismatch_is_not_aggregated(self) -> None:
        first = evidence("onnxruntime-cuda", real=True, artifact="sha256:a")
        second = evidence("onnxruntime-cuda", real=True, artifact="sha256:b")
        self.assertEqual(classify_execution_evidence([first, second]), "invalid-evidence")

    def test_disjoint_sharded_artifact_evidence_aggregates(self) -> None:
        first = evidence_payload(evidence("onnxruntime-cpu", real=True))
        second = evidence_payload(evidence("onnxruntime-cpu", real=True))
        second["providerName"] = "/provider/B"
        second["providerBootId"] = "boot-b"
        second["roles"] = ["/LLM/Stage/1"]
        second["artifactDigests"] = {"/LLM/Stage/1": "sha256:b"}
        parsed = [ExecutionEvidenceV1.from_dict(item) for item in (first, second)]
        self.assertEqual(classify_execution_evidence(parsed), "onnxruntime-cpu")


if __name__ == "__main__":
    unittest.main()
    GenerationQueueFull,
