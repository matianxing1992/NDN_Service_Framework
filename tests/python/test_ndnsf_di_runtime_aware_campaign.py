#!/usr/bin/env python3
"""Runtime-aware NativeTracer campaign contract tests."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ndnsf import (
    GenericProviderRuntimeHint,
    ProviderNetworkMatrix,
    ProviderCapabilityHint,
    ServiceOperationState,
    ServiceOperationStatus,
    encode_ack_metadata,
    encode_provider_capability_ack,
)


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
PLAN_TRACER = REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer/plan_tracer.py"
USER_DRIVER = REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py"
FIXTURE = (
    REPO /
    "examples/python/NDNSF-DistributedInference/native_di_tracer/runtime_aware_fixtures/multi_user_requests.json"
)


def load_harness_module():
    spec = importlib.util.spec_from_file_location("native_tracer_minindn", HARNESS)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def load_user_driver_module():
    spec = importlib.util.spec_from_file_location("native_tracer_user_driver", USER_DRIVER)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def _typed_native_ack_payload(provider: str = "/P/backbone") -> bytes:
    return encode_provider_capability_ack(ProviderCapabilityHint(
        provider_name=provider,
        service_name="/Inference/NativeTracer",
        ready=True,
        runtime_hint=GenericProviderRuntimeHint(
            provider_name=provider,
            queue_length=3,
            active_work_count=1,
        ),
        service_payload_schema="ndnsf-di-capability-v1",
        service_payload={
            "roles": "/Backbone",
            "queue": 3,
            "readyQueue": 1,
            "waitingInputs": 1,
            "activeWorkers": 1,
            "workers": 2,
            "idleWorkers": 1,
            "runtimeStatus": "ready",
            "leaseId": "l0",
            "leaseExpiresAtMs": "12345",
        },
    ))


def load_plan_tracer_module():
    tracer_dir = str(PLAN_TRACER.parent)
    old_path = list(sys.path)
    sys.path.insert(0, tracer_dir)
    try:
        spec = importlib.util.spec_from_file_location("native_tracer_plan_tracer", PLAN_TRACER)
        module = importlib.util.module_from_spec(spec)
        assert spec and spec.loader
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path[:] = old_path


class RuntimeAwareCampaignTest(unittest.TestCase):
    def test_dry_run_accepts_multi_user_runtime_aware_arguments(self) -> None:
        env = dict(os.environ)
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "Experiments"),
            env.get("PYTHONPATH", ""),
        ])
        completed = subprocess.run([
            sys.executable,
            str(HARNESS),
            "--dry-run",
            "--runtime-aware-user-planner",
            "--multi-user-workload", str(FIXTURE),
            "--runtime-aware-max-replans", "1",
            "--runtime-aware-replan-reasons", "FRAGMENT_EVICTED",
            "--requests", "1",
            "--concurrency", "1",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["event"], "NDNSF_DI_NATIVE_TRACER_MININDN_DRY_RUN")
        self.assertTrue(payload["runtimeAwareUserPlanner"])
        self.assertEqual(payload["multiUserWorkload"]["requestCount"], 2)
        self.assertEqual(payload["requests"], 2)
        self.assertEqual(payload["runtimeAwareMaxReplans"], 1)
        self.assertIn("--runtime-aware-max-replans 1", payload["userDriverCommand"])
        self.assertIn("FRAGMENT_EVICTED", payload["userDriverCommand"])

    def test_dry_run_accepts_provider_network_matrix_json(self) -> None:
        env = os.environ.copy()
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(REPO),
            env.get("PYTHONPATH", ""),
        ])
        matrix_path = "/tmp/ndnsf-provider-network-matrix.json"
        completed = subprocess.run([
            sys.executable,
            str(HARNESS),
            "--dry-run",
            "--runtime-aware-user-planner",
            "--provider-network-matrix-json", matrix_path,
            "--requests", "1",
            "--concurrency", "1",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["providerNetworkMatrixJson"], matrix_path)
        self.assertTrue(payload["runtimeAwareUserPlanner"])
        self.assertTrue(payload["providerPairTelemetryProbeEnabled"])

        skipped = subprocess.run([
            sys.executable,
            str(HARNESS),
            "--dry-run",
            "--skip-provider-pair-telemetry-probe",
            "--requests", "1",
            "--concurrency", "1",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        skipped_payload = json.loads(skipped.stdout)
        self.assertFalse(skipped_payload["providerPairTelemetryProbeEnabled"])

    def test_plan_tracer_loads_provider_pair_telemetry_summary_matrix(self) -> None:
        plan_tracer = load_plan_tracer_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "previous-summary.json"
            path.write_text(
                json.dumps({
                    "providerPairTelemetry": {
                        "status": "collected",
                        "matrix": {
                            "defaultRttMs": 70,
                            "unknownPenaltyMs": 123,
                            "metrics": [{
                                "srcPeer": "/P/backbone",
                                "dstPeer": "/P/merge",
                                "rttMs": 12,
                                "bandwidthMbps": 100,
                                "updatedAtMs": 4102444800000,
                            }],
                        },
                    },
                }),
                encoding="utf-8")
            matrix = plan_tracer.load_provider_network_matrix_input(str(path))

        cost, detail = matrix.transfer_cost_ms(
            "/P/backbone",
            "/P/merge",
            4096,
            now_ms_value=4102444800000,
        )
        self.assertEqual(detail["rttMs"], 12.0)
        self.assertFalse(detail["unknown"])
        self.assertGreater(cost, 12.0)

    def test_plan_tracer_uses_provider_profiles_for_runtime_metadata(self) -> None:
        plan_tracer = load_plan_tracer_module()
        service_plan = {
            "roles": ["/Backbone", "/Merge"],
            "dependencies": [],
        }
        template = plan_tracer.build_runtime_plan_template(service_plan)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "provider-profiles.json"
            path.write_text(
                json.dumps({
                    "roles": {
                        "/Backbone": {
                            "provider": "/NDNSF-DI/Tracer/provider/backbone",
                            "node": "ucla",
                            "roleComputeMs": 4.0,
                        },
                        "/Merge": {
                            "provider": "/NDNSF-DI/Tracer/provider/merge",
                            "node": "neu",
                            "roleComputeMs": 1.5,
                        },
                    },
                }),
                encoding="utf-8")
            metadata = plan_tracer.load_provider_ack_metadata_input(str(path), template)

        self.assertEqual(
            sorted(metadata),
            ["/NDNSF-DI/Tracer/provider/backbone", "/NDNSF-DI/Tracer/provider/merge"],
        )
        backbone = metadata["/NDNSF-DI/Tracer/provider/backbone"]
        self.assertEqual(backbone.provider_runtime_hint.provider_name,
                         "/NDNSF-DI/Tracer/provider/backbone")
        self.assertEqual(
            backbone.service_payload["fragmentStates"][0]["fragmentKey"]["fragment_digest"],
            "sha256:native-tracer:Backbone",
        )

    def test_user_driver_loads_role_assignments_from_csv(self) -> None:
        user_driver = load_user_driver_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "assignment.csv"
            path.write_text(
                "assignment,role,provider\n"
                "default,/Backbone,/P/backbone\n"
                "default,/Merge,/P/merge\n",
                encoding="utf-8")
            assignments = user_driver.load_role_assignment_candidates(str(path))
        self.assertEqual(assignments["/Backbone"][0]["provider"], "/P/backbone")
        self.assertEqual(assignments["/Merge"][0]["assignment"], "default")

    def test_user_driver_role_preference_env_is_scoped(self) -> None:
        user_driver = load_user_driver_module()
        original = os.environ.get(user_driver.ROLE_PROVIDER_PREFERENCE_ENV)
        os.environ[user_driver.ROLE_PROVIDER_PREFERENCE_ENV] = "previous"
        try:
            with user_driver.role_provider_preference_env("/Backbone=>/P/backbone;"):
                self.assertEqual(
                    os.environ[user_driver.ROLE_PROVIDER_PREFERENCE_ENV],
                    "/Backbone=>/P/backbone;",
                )
            self.assertEqual(os.environ[user_driver.ROLE_PROVIDER_PREFERENCE_ENV], "previous")
        finally:
            if original is None:
                os.environ.pop(user_driver.ROLE_PROVIDER_PREFERENCE_ENV, None)
            else:
                os.environ[user_driver.ROLE_PROVIDER_PREFERENCE_ENV] = original

    def test_user_driver_keeps_base_publisher_started_for_workload(self) -> None:
        user_driver = load_user_driver_module()

        class FakeUser:
            def __init__(self) -> None:
                self.events = []

            def start(self) -> None:
                self.events.append("start")

            def stop(self) -> None:
                self.events.append("stop")

        user = FakeUser()
        result = user_driver.run_with_started_user(
            user,
            lambda: user.events.append("work") or "completed",
        )

        self.assertEqual(result, "completed")
        self.assertEqual(user.events, ["start", "work", "stop"])

    def test_user_driver_stops_base_publisher_when_workload_raises(self) -> None:
        user_driver = load_user_driver_module()

        class FakeUser:
            def __init__(self) -> None:
                self.events = []

            def start(self) -> None:
                self.events.append("start")

            def stop(self) -> None:
                self.events.append("stop")

        user = FakeUser()

        def fail() -> None:
            user.events.append("work")
            raise RuntimeError("expected")

        with self.assertRaisesRegex(RuntimeError, "expected"):
            user_driver.run_with_started_user(user, fail)
        self.assertEqual(user.events, ["start", "work", "stop"])

    def test_threaded_driver_reports_measurement_interval_before_cleanup(self) -> None:
        user_driver = load_user_driver_module()
        args = type("Args", (), {
            "requests": 2,
            "concurrency": 1,
            "target_rps": 1000.0,
            "open_loop_duration_s": 0.002,
            "service": "/Inference/NativeTracer",
        })()
        original = user_driver.run_one_request
        user_driver.run_one_request = lambda *_args, **_kwargs: {
            "status": "executed",
            "elapsedMs": 0.1,
            "payloadBytes": 1,
        }
        try:
            results, metadata = user_driver.run_threaded_open_loop_requests(
                [object()], args, {}, [], {}, [], {}, {})
        finally:
            user_driver.run_one_request = original

        self.assertEqual(len(results), 2)
        self.assertGreater(metadata["measurementElapsedMs"], 0.0)
        self.assertLess(metadata["measurementElapsedMs"], 100.0)

    def test_user_driver_overload_fast_fail_metadata_uses_shorter_timeout(self) -> None:
        user_driver = load_user_driver_module()
        args = type("Args", (), {
            "timeout_ms": 60000,
            "overload_fast_fail_timeout_ms": 5000,
        })()

        self.assertEqual(user_driver.effective_timeout_ms(args), 5000)
        metadata = user_driver.overload_fast_fail_metadata(args)
        self.assertTrue(metadata["overloadFastFail"]["enabled"])
        self.assertEqual(metadata["overloadFastFail"]["effectiveTimeoutMs"], 5000)
        self.assertTrue(
            user_driver.is_overload_fast_fail_error(
                args,
                "timeout: /request",
                5200.0,
            )
        )

    def test_user_driver_overload_fast_fail_timeout_is_disabled_or_clamped(self) -> None:
        user_driver = load_user_driver_module()
        disabled = type("Args", (), {
            "timeout_ms": 60000,
            "overload_fast_fail_timeout_ms": 0,
        })()
        too_large = type("Args", (), {
            "timeout_ms": 60000,
            "overload_fast_fail_timeout_ms": 120000,
        })()

        self.assertEqual(user_driver.effective_timeout_ms(disabled), 60000)
        self.assertFalse(user_driver.overload_fast_fail_metadata(disabled)["overloadFastFail"]["enabled"])
        self.assertEqual(user_driver.effective_timeout_ms(too_large), 60000)

    def test_harness_parses_open_loop_wait_metrics(self) -> None:
        harness = load_harness_module()
        payload = {
            "status": "executed",
            "localBackpressureCount": 0,
            "localBackpressureWaitCount": 69,
            "maxScheduleSlipMs": 7150.519,
        }
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "user-driver.log"
            log_path.write_text(
                "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION "
                + json.dumps(payload, sort_keys=True)
                + "\n",
                encoding="utf-8")

            parsed = harness.parse_user_execution(log_path)

        self.assertEqual(parsed["localBackpressureCount"], 0)
        self.assertEqual(parsed["localBackpressureWaitCount"], 69)
        self.assertEqual(parsed["maxScheduleSlipMs"], 7150.519)

    def test_harness_projects_open_loop_measurement_metadata(self) -> None:
        harness = load_harness_module()
        fields = harness.user_execution_measurement_fields({
            "measurementStartEpoch": 100.0,
            "measurementElapsedMs": 5000.0,
            "maxScheduleSlipMs": 12.5,
        })

        self.assertEqual(fields, {
            "measurementStartEpoch": 100.0,
            "measurementElapsedMs": 5000.0,
            "maxScheduleSlipMs": 12.5,
        })

    def test_provider_ack_runtime_hint_collection_preserves_malformed_log_line(self) -> None:
        harness = load_harness_module()
        payload = _typed_native_ack_payload().decode("utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "provider.log"
            log_path.write_text(
                "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION "
                "provider=/P/backbone roles=/Backbone status=1 "
                f'payload="{payload}"\n'
                "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION provider=\n",
                encoding="utf-8",
            )

            summary = harness.collect_provider_ack_runtime_hints(Path(tmp))

        self.assertEqual(summary["eventCount"], 1)
        self.assertEqual(summary["parseErrorCount"], 1)
        self.assertEqual(len(summary["parseErrors"]), 1)
        self.assertEqual(summary["providers"]["/P/backbone"]["ackEvents"], 1)

    def test_dependency_collection_separates_interleaved_trace_line(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "provider.log"
            log_path.write_text(
                "NDNSF_DI_DEPENDENCY_OBJECT session=/s/1 scope=edge "
                "producer=/A consumer=/B direction=fetch payload_bytes=65 "
                "planned_name=/data/1 status=ok\n"
                "NDNSF_DI_DEPENDENCY_OBJECT session=/s/2 scope=edge "
                "producer=/A consumer=/B direction=fetch payload_bytes=65e "
                "planned_name=/data/2d status=oker\n",
                encoding="utf-8",
            )

            summary = harness.collect_dependency_object_counters(Path(tmp))

        self.assertEqual(summary["observedLineCount"], 2)
        self.assertEqual(summary["eventCount"], 1)
        self.assertEqual(summary["statusCounters"], {"ok": 1})
        self.assertEqual(summary["parseErrorCount"], 1)
        self.assertEqual(len(summary["parseErrors"]), 1)

    def test_user_driver_builds_ack_candidate_snapshot(self) -> None:
        user_driver = load_user_driver_module()

        class Candidate:
            provider_name = "/P/backbone"
            service_name = "/Inference/NativeTracer"
            request_id = "/req/1"
            status = True
            message = "ready"
            payload = _typed_native_ack_payload()
            telemetry = {"rtt_ms": 3.0}

        snapshot = user_driver.ack_candidates_snapshot([Candidate()])

        self.assertEqual(snapshot[0]["provider"], "/P/backbone")
        self.assertEqual(snapshot[0]["roles"], "/Backbone")
        self.assertEqual(snapshot[0]["queue"], 3)
        self.assertEqual(snapshot[0]["activeWorkers"], 1)
        self.assertEqual(snapshot[0]["leaseId"], "l0")
        self.assertEqual(snapshot[0]["telemetry"]["rtt_ms"], 3.0)

    def test_capacity_pool_candidate_rows_add_alternates_for_each_role(self) -> None:
        harness = load_harness_module()
        primary = [
            {
                "assignment": "capacity-pool",
                "role": role,
                "provider": provider,
                "node": node,
                "service": harness.SERVICE,
            }
            for role, (node, provider) in harness.DEFAULT_ASSIGNMENT.items()
        ]

        candidates = harness.capacity_pool_candidate_rows(primary)
        by_role = {}
        for row in candidates:
            by_role.setdefault(row["role"], set()).add(row["provider"])

        self.assertEqual(set(by_role), set(harness.DEFAULT_ASSIGNMENT))
        self.assertTrue(all(len(providers) >= 2 for providers in by_role.values()))
        self.assertEqual(
            len(harness.grouped_provider_rows(candidates)),
            len({
                (row["node"], row["provider"])
                for row in candidates
            }),
        )

    def test_harness_writes_runtime_hints_snapshot(self) -> None:
        harness = load_harness_module()
        rows = [
            {
                "assignment": "capacity-pool",
                "role": "/Backbone",
                "provider": "/P/backbone",
                "node": "memphis",
                "service": harness.SERVICE,
            },
            {
                "assignment": harness.CAPACITY_POOL_ALTERNATE_ASSIGNMENT,
                "role": "/Backbone",
                "provider": "/P/backbone-alt",
                "node": "ucla",
                "service": harness.SERVICE,
            },
        ]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "runtime-hints.json"
            harness.write_runtime_hints_json(
                path,
                rows,
                role_execution_delay_ms=50,
                provider_admission_max_active_workers=1,
                provider_admission_max_queue=1,
            )
            payload = json.loads(path.read_text(encoding="utf-8"))

        self.assertEqual(payload["schema"], "ndnsf-di-runtime-hints-v1")
        primary = payload["providerRoles"]["/P/backbone|/Backbone"]
        alternate = payload["providerRoles"]["/P/backbone-alt|/Backbone"]
        self.assertEqual(primary["leaseOffers"][0]["status"], "GRANTED")
        self.assertEqual(primary["runtimeHint"]["fragmentResidency"], "CPU_RESIDENT")
        self.assertEqual(alternate["runtimeHint"]["fragmentResidency"], "DISK_RESIDENT")
        self.assertGreater(alternate["readyCostMs"], primary["readyCostMs"])

    def test_provider_ack_runtime_hints_are_aggregated(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            logs = Path(tmp)
            ack_payload = _typed_native_ack_payload().decode("utf-8")
            (logs / "provider.log").write_text(
                "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION "
                "provider=/P/backbone roles=/Backbone status=1 "
                "message=\"native DI provider ready\" "
                f"payload=\"{ack_payload}\"\n",
                encoding="utf-8",
            )
            hints = harness.collect_provider_ack_runtime_hints(logs)

        provider = hints["providers"]["/P/backbone"]
        self.assertEqual(hints["eventCount"], 1)
        self.assertEqual(provider["ackEvents"], 1)
        self.assertEqual(provider["successfulAckEvents"], 1)
        self.assertEqual(provider["maxQueue"], 3)
        self.assertEqual(provider["maxActiveWorkers"], 1)
        self.assertEqual(provider["latest"]["leaseId"], "l0")
        self.assertEqual(provider["latest"]["runtimeStatus"], "ready")

    def test_core_envelope_summary_is_aggregated_from_provider_ack_payloads(self) -> None:
        harness = load_harness_module()
        status = ServiceOperationStatus(
            operation_id="op-1",
            operation="native-provider-admission",
            service_name="/Inference/NativeTracer",
            provider_name="/P/backbone",
            state=ServiceOperationState.RUNNING,
            progress=0.5,
        )
        hint = ProviderCapabilityHint(
            provider_name="/P/backbone",
            service_name="/Inference/NativeTracer",
            ready=True,
            message="native DI provider ready",
            runtime_hint=GenericProviderRuntimeHint(
                provider_name="/P/backbone",
                queue_length=3,
                active_work_count=1,
            ),
            operation_status=status,
            service_payload_schema="ndnsf-di-capability-v1",
            service_payload={"roles": ["/Backbone"]},
        )
        payload = encode_ack_metadata(hint.to_ack_fields()).decode("utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            logs = Path(tmp)
            (logs / "provider.log").write_text(
                "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION "
                "provider=/P/backbone roles=/Backbone status=1 "
                "message=\"native DI provider ready\" "
                f"payload=\"{payload}\"\n",
                encoding="utf-8",
            )
            summary = harness.collect_core_envelope_summary(logs)

        self.assertEqual(summary["eventCount"], 1)
        self.assertEqual(summary["envelopeCounts"]["providerCapabilityHint"], 1)
        self.assertEqual(summary["envelopeCounts"]["serviceOperationStatus"], 1)
        self.assertEqual(summary["providerReadiness"]["ready"], 1)
        self.assertEqual(summary["servicePayloadSchemas"]["ndnsf-di-capability-v1"], 1)
        self.assertEqual(summary["operationStates"]["RUNNING"], 1)
        latest = summary["latestProviders"]["/P/backbone"]
        self.assertEqual(latest["queueLength"], 3)
        self.assertEqual(latest["activeWorkCount"], 1)

    def test_execution_summaries_carry_core_operation_status(self) -> None:
        harness = load_harness_module()
        summary = {
            "userExecution": {
                "status": "executed",
                "reason": "ok",
                "requestCount": 4,
                "successCount": 3,
                "failureCount": 1,
                "requests": [{"requestId": "r1"}],
            },
            "dependencyExecution": {
                "status": "failed",
                "reason": "timeout waiting for dependency",
                "roles": ["/Backbone", "/Merge"],
            },
        }

        harness.attach_execution_operation_statuses(summary)

        user_status = ServiceOperationStatus.from_dict(
            summary["userExecution"]["operationStatus"])
        dependency_status = ServiceOperationStatus.from_dict(
            summary["dependencyExecution"]["operationStatus"])

        self.assertEqual(summary["userExecution"]["status"], "executed")
        self.assertEqual(user_status.operation, "DI_USER_EXECUTION")
        self.assertEqual(user_status.state, ServiceOperationState.DONE)
        self.assertAlmostEqual(user_status.progress, 0.75)
        self.assertEqual(user_status.metadata["requestSampleCount"], 1)
        self.assertEqual(dependency_status.operation, "DI_DEPENDENCY_EXECUTION")
        self.assertEqual(dependency_status.state, ServiceOperationState.EXPIRED)
        self.assertEqual(dependency_status.metadata["roles"], ["/Backbone", "/Merge"])

    def test_provider_pair_telemetry_collects_dependency_edge_rtts(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            (out_dir / "dependency-edge-ndnping-rtt-stats.json").write_text(
                json.dumps({
                    "rows": [{
                        "producerRole": "/Backbone",
                        "consumerRole": "/Merge",
                        "scope": "backbone-to-merge",
                        "expectedBytes": 4096,
                        "producerPrefix": "/P/backbone",
                        "consumerPrefix": "/P/merge",
                        "count": 3,
                        "rttsMs": [9.0, 12.0, 15.0],
                        "summaryMs": {
                            "count": 3,
                            "mean": 12.0,
                            "p50": 12.0,
                            "stddev": 3.0,
                        },
                    }],
                }),
                encoding="utf-8")

            telemetry = harness.collect_provider_pair_telemetry(out_dir)

        self.assertEqual(telemetry["status"], "collected")
        self.assertEqual(telemetry["metricCount"], 1)
        metric = telemetry["metrics"][0]
        self.assertEqual(metric["src_peer"], "/P/backbone")
        self.assertEqual(metric["dst_peer"], "/P/merge")
        self.assertEqual(metric["rtt_ms"], 12.0)
        self.assertEqual(metric["bytes_sampled"], 4096)
        self.assertGreater(metric["confidence"], 0.0)
        matrix = ProviderNetworkMatrix.from_dict(telemetry["matrix"])
        cost_ms, detail = matrix.transfer_cost_ms(
            "/P/backbone",
            "/P/merge",
            4096,
            now_ms_value=metric["updated_at_ms"],
        )
        self.assertFalse(detail["unknown"])
        self.assertEqual(detail["rttMs"], 12.0)
        self.assertGreater(cost_ms, 12.0)

    def test_dependency_edge_probe_helpers_parse_plan_and_ping_output(self) -> None:
        harness = load_harness_module()
        self.assertEqual(
            harness.parse_ndnping_rtts(
                "content from /P/ndnping: seq=1 time=1.25 ms\n"
                "content from /P/ndnping: seq=2 time=3 ms\n"),
            [1.25, 3.0],
        )
        rows = [{
            "role": "/Backbone",
            "roles": "/Backbone,/Head/Shard/0",
            "provider": "/P/backbone",
            "node": "ucla",
        }]
        metadata = harness.provider_metadata_from_rows(rows)
        self.assertEqual(metadata["/Backbone"]["providerPrefix"], "/P/backbone")
        self.assertEqual(metadata["/Head/Shard/0"]["providerNode"], "ucla")

        with tempfile.TemporaryDirectory() as tmp:
            plan_path = Path(tmp) / "native-execution-plan.json"
            plan_path.write_text(
                json.dumps({
                    "services": [{
                        "service": harness.SERVICE,
                        "dependencies": [{
                            "producers": ["/Backbone"],
                            "consumers": ["/Merge"],
                            "keyScope": "backbone-to-merge",
                            "expectedSegments": 2,
                            "expectedBytes": 4096,
                        }],
                    }],
                }),
                encoding="utf-8")
            edges = harness.load_native_dependency_edges(plan_path)

        self.assertEqual(len(edges), 1)
        self.assertEqual(edges[0]["producerRole"], "/Backbone")
        self.assertEqual(edges[0]["consumerRole"], "/Merge")
        self.assertEqual(edges[0]["expectedBytes"], 4096)

    def test_planner_metrics_aggregation_reports_campaign_fields(self) -> None:
        harness = load_harness_module()
        metrics = harness.build_campaign_metrics({
            "status": "SUCCESS",
            "requestCount": 3,
            "userExecution": {
                "requestCount": 3,
                "successCount": 2,
                "failureCount": 1,
                "p50Ms": 10.0,
                "p95Ms": 25.0,
                "meanMs": 14.0,
                "makespanMs": 42.0,
                "replanCount": 1,
            },
            "providerUtilization": {
                "/P1": {"estimatedUtilization": 0.5},
                "/P2": {"estimatedUtilization": 0.25},
            },
            "failureBreakdown": {
                "negativeAckEventCount": 1,
            },
            "runtimeAwarePlanner": {
                "selectedProviders": {
                    "/Backbone": "/P1",
                    "/Merge": "/P2",
                },
                "selectedResidencies": {
                    "/Backbone": "GPU_LOADED",
                    "/Merge": "CPU_RESIDENT",
                },
                "nodeCostSummary": {"totalMs": 3.0},
                "edgeCostSummary": {"totalMs": 4.0},
            },
            "leaseCounters": {
                "granted": 3,
                "consumed": 2,
                "expired": 1,
            },
            "providerFragmentInventory": {
                "residencyCounters": {
                    "CPU_RESIDENT": 4,
                    "DISK_RESIDENT": 4,
                },
            },
            "rpsSweep": [
                {"targetRps": 4, "status": "SUCCESS", "successRate": 1.0, "p95Ms": 20.0},
                {"targetRps": 8, "status": "SUCCESS", "successRate": 0.995, "p95Ms": 35.0},
                {"targetRps": 12, "status": "FAILURE", "successRate": 0.90, "failureRate": 0.10},
            ],
        })
        self.assertEqual(metrics["requestCount"], 3)
        self.assertEqual(metrics["successRate"], round(2 / 3, 6))
        self.assertEqual(metrics["latencyMs"]["p95"], 25.0)
        self.assertEqual(metrics["leaseCounters"]["negativeAckEvents"], 1)
        self.assertEqual(metrics["leaseCounters"]["granted"], 3)
        self.assertEqual(metrics["leaseCounters"]["consumed"], 2)
        self.assertEqual(metrics["residencyCounters"]["GPU_LOADED"], 1)
        self.assertEqual(metrics["residencyCounters"]["CPU_RESIDENT"], 1)
        self.assertEqual(metrics["observedResidencyCounters"]["CPU_RESIDENT"], 4)
        self.assertEqual(metrics["observedResidencyCounters"]["DISK_RESIDENT"], 4)
        self.assertEqual(metrics["maxStableRps"], 8.0)
        self.assertEqual(metrics["utilization"]["meanEstimatedUtilization"], 0.375)
        self.assertEqual(metrics["replanCount"], 1)

    def test_admission_lease_log_counters_are_aggregated(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "provider.log"
            log.write_text(
                "NDNSF_DI_NATIVE_PROVIDER_ADMISSION_LEASE_GRANTED "
                "provider=/P service=/S leaseId=l0 proof=role=/Backbone\n"
                "NDNSF_ADMISSION_LEASE_ACCEPTED provider=/P requester=/U "
                "service=/S requestId=/r leaseId=l1\n"
                "NDNSF_ADMISSION_LEASE_REJECTED provider=/P requester=/U "
                "service=/S requestId=/r leaseId=l2 reason=LEASE_EXPIRED\n",
                encoding="utf-8")
            counters = harness.collect_admission_lease_counters(Path(tmp))
        self.assertEqual(counters["granted"], 1)
        self.assertEqual(counters["consumed"], 1)
        self.assertEqual(counters["rejected"], 1)
        self.assertEqual(counters["expired"], 1)
        self.assertEqual(counters["reasons"]["LEASE_EXPIRED"], 1)

    def test_provider_fragment_inventory_events_are_aggregated(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "provider.log"
            log.write_text(
                "NDNSF_DI_FRAGMENT_INVENTORY event=CPU_RESIDENT "
                "provider=unknown role=/Backbone fragmentDigest=sha256:bb "
                "backend=onnx-cpu path=/tmp/backbone.onnx residency=CPU_RESIDENT "
                "epoch_ms=1\n"
                "NDNSF_DI_FRAGMENT_INVENTORY event=EXECUTION_OBSERVED "
                "provider=/P1 role=/Backbone fragmentDigest=sha256:bb "
                "backend=onnx-cpu path=/tmp/backbone.onnx residency=CPU_RESIDENT "
                "epoch_ms=2\n"
                "NDNSF_DI_FRAGMENT_INVENTORY event=EVICTED "
                "provider=unknown role=/Backbone fragmentDigest=sha256:bb "
                "backend=onnx-cpu path=/tmp/backbone.onnx residency=DISK_RESIDENT "
                "epoch_ms=3\n",
                encoding="utf-8")
            inventory = harness.collect_provider_fragment_inventory(Path(tmp))

        self.assertEqual(inventory["eventCount"], 3)
        self.assertEqual(inventory["eventCounters"]["CPU_RESIDENT"], 1)
        self.assertEqual(inventory["eventCounters"]["EXECUTION_OBSERVED"], 1)
        self.assertEqual(inventory["eventCounters"]["EVICTED"], 1)
        self.assertEqual(inventory["residencyCounters"]["CPU_RESIDENT"], 2)
        self.assertEqual(inventory["residencyCounters"]["DISK_RESIDENT"], 1)
        latest = inventory["latestByProviderRole"]["/P1|/Backbone"]
        self.assertEqual(latest["fragmentDigest"], "sha256:bb")
        self.assertEqual(latest["residency"], "CPU_RESIDENT")

    def test_native_tracer_dry_run_passes_fast_fail_timeout_to_user_driver(self) -> None:
        env = dict(os.environ)
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(REPO / "Experiments"),
            env.get("PYTHONPATH", ""),
        ])
        completed = subprocess.run([
            sys.executable,
            str(HARNESS),
            "--dry-run",
            "--overload-fast-fail-timeout-ms", "5000",
            "--requests", "1",
            "--concurrency", "1",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertIn("--overload-fast-fail-timeout-ms 5000",
                      payload["userDriverCommand"])


if __name__ == "__main__":
    unittest.main()
