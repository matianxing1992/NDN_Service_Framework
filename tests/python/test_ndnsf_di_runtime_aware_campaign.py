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
)


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
SWEEP = REPO / "Experiments/NDNSF_DI_RuntimeAware_RpsSweep.py"
STREAM_CHUNK_CAMPAIGN = REPO / "Experiments/NDNSF_DI_StreamChunk_Mode_Campaign.py"
PLAN_TRACER = REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer/plan_tracer.py"
USER_DRIVER = REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py"
ADVISORY_COORDINATOR = (
    REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer/advisory_coordinator.py"
)
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


def load_advisory_coordinator_module():
    spec = importlib.util.spec_from_file_location("native_tracer_advisory", ADVISORY_COORDINATOR)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


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
            "--dependency-envelope-mode", "streamchunk",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["event"], "NDNSF_DI_NATIVE_TRACER_MININDN_DRY_RUN")
        self.assertTrue(payload["runtimeAwareUserPlanner"])
        self.assertEqual(payload["multiUserWorkload"]["requestCount"], 2)
        self.assertEqual(payload["requests"], 2)
        self.assertEqual(payload["runtimeAwareMaxReplans"], 1)
        self.assertEqual(payload["dependencyEnvelopeMode"], "streamchunk")
        self.assertEqual(payload["dependencyPayloadMode"], "streamchunk")
        self.assertEqual(payload["dependencyEnvelopeEnv"], {
            "NDNSF_DI_STREAM_CHUNK_DEPENDENCIES": "1",
            "NDNSF_DI_STREAM_DEPENDENCY_TRACE": "1",
        })
        self.assertEqual(payload["dependencyPayloadEnv"], {
            "NDNSF_DI_STREAM_CHUNK_DEPENDENCIES": "1",
            "NDNSF_DI_STREAM_DEPENDENCY_TRACE": "1",
        })
        self.assertIn("--runtime-aware-max-replans 1", payload["userDriverCommand"])
        self.assertIn("FRAGMENT_EVICTED", payload["userDriverCommand"])

    def test_streamchunk_mode_campaign_dry_run_builds_both_modes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            completed = subprocess.run([
                sys.executable,
                str(STREAM_CHUNK_CAMPAIGN),
                "--out", str(Path(tmp) / "streamchunk-campaign"),
                "--modes", "raw,streamchunk",
                "--repeats", "2",
                "--requests", "1",
                "--concurrency", "1",
                "--dry-run",
                "--",
                "--tracer-deterministic-runner",
            ], cwd=str(REPO), text=True, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        commands = payload["commands"]
        self.assertEqual(payload["status"], "DRY_RUN")
        self.assertEqual(len(commands), 4)
        joined = [" ".join(command) for command in commands]
        self.assertTrue(any("--dependency-envelope-mode raw" in item for item in joined))
        self.assertTrue(any("--dependency-envelope-mode streamchunk" in item for item in joined))

    def test_legacy_dependency_payload_mode_alias_still_works(self) -> None:
        env = os.environ.copy()
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(REPO),
            env.get("PYTHONPATH", ""),
        ])
        completed = subprocess.run([
            sys.executable,
            str(HARNESS),
            "--dry-run",
            "--requests", "1",
            "--concurrency", "1",
            "--dependency-payload-mode", "streamchunk",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["dependencyEnvelopeMode"], "streamchunk")
        self.assertEqual(payload["dependencyPayloadMode"], "streamchunk")

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

    def test_user_driver_loads_role_assignments_from_csv(self) -> None:
        user_driver = load_user_driver_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "assignment.csv"
            path.write_text(
                "assignment,role,provider\n"
                "default,/Backbone,/P/backbone\n"
                "default,/Merge,/P/merge\n",
                encoding="utf-8")
            assignments = user_driver.load_role_assignments(str(path))
        self.assertEqual(assignments["/Backbone"]["provider"], "/P/backbone")
        self.assertEqual(assignments["/Merge"]["assignment"], "default")

    def test_user_driver_builds_role_provider_preference_from_advisory(self) -> None:
        user_driver = load_user_driver_module()
        preference = user_driver.role_provider_preference_from_advisory(
            {
                "enabled": True,
                "status": "executed",
                "suggestions": [{
                    "roleAssignments": {
                        "/Backbone": {"provider": "/P/backbone"},
                        "/Merge": {"providerName": "/P/merge"},
                        "/NotRequested": {"provider": "/P/ignored"},
                    },
                }],
            },
            [{"role": "/Backbone"}, {"role": "/Merge"}],
        )
        self.assertIn("/Backbone=>/P/backbone;", preference)
        self.assertIn("/Merge=>/P/merge;", preference)
        self.assertNotIn("/NotRequested", preference)

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

    def test_wire_advisory_coordinator_balances_role_candidates(self) -> None:
        from ndnsf import CoordinationIntent, CoordinationRequest

        module = load_advisory_coordinator_module()
        args = type("Args", (), {
            "provider": "/coord",
            "service": "/NDNSF/Coordination/Advisory",
            "suggestion_ttl_ms": 5000,
            "proof_secret": "",
            "default_role_duration_ms": 500.0,
            "fairness_penalty_ms": 25.0,
        })()
        handler = module.make_handler(args)
        request = CoordinationRequest((
            CoordinationIntent(
                intent_id="i1",
                request_id="r1",
                service_name="/Inference/NativeTracer",
                expires_at_ms=4102444800000,
                payload={
                    "templateId": "native-tracer-plan",
                    "roleCandidates": {
                        "/Backbone": [
                            {"provider": "/P/a", "assignment": "a"},
                            {"provider": "/P/b", "assignment": "b"},
                        ],
                    },
                },
            ),
            CoordinationIntent(
                intent_id="i2",
                request_id="r2",
                service_name="/Inference/NativeTracer",
                expires_at_ms=4102444800000,
                payload={
                    "templateId": "native-tracer-plan",
                    "roleCandidates": {
                        "/Backbone": [
                            {"provider": "/P/a", "assignment": "a"},
                            {"provider": "/P/b", "assignment": "b"},
                        ],
                    },
                },
            ),
        ), metadata={"windowId": "w1"})

        response = handler(request)
        providers = [
            item.payload["roleAssignments"]["/Backbone"]["provider"]
            for item in response.suggestions
        ]
        self.assertEqual(set(providers), {"/P/a", "/P/b"})
        self.assertEqual(response.suggestions[0].payload["windowVersion"], 1)
        self.assertEqual(
            response.suggestions[0].payload["advisoryMode"],
            "lease-aware-rolling-window-with-fragments",
        )

    def test_wire_advisory_coordinator_scores_lease_and_runtime_hints(self) -> None:
        from ndnsf import CoordinationIntent, CoordinationRequest

        module = load_advisory_coordinator_module()
        args = type("Args", (), {
            "provider": "/coord",
            "service": "/NDNSF/Coordination/Advisory",
            "suggestion_ttl_ms": 5000,
            "proof_secret": "",
            "default_role_duration_ms": 500.0,
            "fairness_penalty_ms": 25.0,
        })()
        handler = module.make_handler(args)
        request = CoordinationRequest((
            CoordinationIntent(
                intent_id="i1",
                request_id="r1",
                service_name="/Inference/NativeTracer",
                expires_at_ms=4102444800000,
                payload={
                    "templateId": "native-tracer-plan",
                    "roleCandidates": {
                        "/Backbone": [
                            {
                                "provider": "/P/rejected",
                                "leaseOffers": [{"status": "REJECTED", "reasonCode": "QUEUE_FULL"}],
                            },
                            {
                                "provider": "/P/slow",
                                "runtimeHint": {"estimatedQueueWaitMs": 2000},
                                "leaseOffers": [{"status": "GRANTED", "estimatedStartMs": 0}],
                            },
                            {
                                "provider": "/P/fast",
                                "runtimeHint": {"estimatedQueueWaitMs": 10},
                                "leaseOffers": [{"status": "GRANTED", "estimatedStartMs": 0}],
                            },
                        ],
                    },
                },
            ),
        ), metadata={"windowId": "w-lease"})

        response = handler(request)
        suggestion = response.suggestions[0]
        assignment = suggestion.payload["roleAssignments"]["/Backbone"]
        self.assertEqual(assignment["provider"], "/P/fast")
        self.assertEqual(assignment["leaseReason"], "LEASE_GRANTED")
        rejected = suggestion.score_breakdown["rejectedCandidates"]
        self.assertEqual(rejected[0]["provider"], "/P/rejected")
        self.assertEqual(rejected[0]["reason"], "NO_VALID_LEASE")

    def test_wire_advisory_coordinator_tolerates_malformed_hints(self) -> None:
        from ndnsf import CoordinationIntent, CoordinationRequest

        module = load_advisory_coordinator_module()
        args = type("Args", (), {
            "provider": "/coord",
            "service": "/NDNSF/Coordination/Advisory",
            "suggestion_ttl_ms": 5000,
            "proof_secret": "",
            "default_role_duration_ms": 500.0,
            "fairness_penalty_ms": 25.0,
        })()
        handler = module.make_handler(args)
        request = CoordinationRequest((
            CoordinationIntent(
                intent_id="i-bad-hints",
                request_id="r-bad-hints",
                service_name="/Inference/NativeTracer",
                expires_at_ms=4102444800000,
                payload={
                    "templateId": "native-tracer-plan",
                    "roleCandidates": {
                        "/Backbone": [
                            {
                                "provider": "/P/bad",
                                "runtimeHint": "not-a-dict",
                                "leaseOffers": "not-a-list",
                                "estimatedDurationMs": "not-a-number",
                            },
                            {
                                "provider": "/P/good",
                                "runtimeHint": {"estimatedQueueWaitMs": 5},
                                "leaseOffers": [{"status": "GRANTED"}],
                                "estimatedDurationMs": 100,
                            },
                        ],
                    },
                },
            ),
        ), metadata={"windowId": "w-bad-hints"})

        response = handler(request)
        suggestion = response.suggestions[0]
        self.assertEqual(
            suggestion.payload["roleAssignments"]["/Backbone"]["provider"],
            "/P/good",
        )
        self.assertIn(
            "/Backbone",
            suggestion.score_breakdown["roleScores"],
        )

    def test_user_driver_enriches_role_candidates_with_runtime_hints(self) -> None:
        user_driver = load_user_driver_module()
        candidates = {
            "/Backbone": [
                {"provider": "/P/slow", "assignment": "primary"},
                {"provider": "/P/fast", "assignment": "alternate"},
            ],
        }
        runtime_hints = {
            "schema": "ndnsf-di-runtime-hints-v1",
            "providerRoles": {
                "/P/slow|/Backbone": {
                    "runtimeHint": {"estimatedQueueWaitMs": 500},
                    "leaseOffers": [{"status": "GRANTED", "estimatedStartMs": 100}],
                    "estimatedDurationMs": 50,
                    "readyCostMs": 20,
                    "residency": "DISK_RESIDENT",
                },
                "/P/fast|/Backbone": {
                    "runtimeHint": {"estimatedQueueWaitMs": 0},
                    "leaseOffers": [{"status": "GRANTED", "estimatedStartMs": 0}],
                    "estimatedDurationMs": 20,
                    "readyCostMs": 0,
                    "residency": "GPU_LOADED",
                },
            },
        }

        enriched = user_driver.enrich_role_candidates_with_runtime_hints(
            candidates,
            runtime_hints,
        )

        slow, fast = enriched["/Backbone"]
        self.assertEqual(slow["runtimeHint"]["estimatedQueueWaitMs"], 500)
        self.assertEqual(slow["leaseOffers"][0]["estimatedStartMs"], 100)
        self.assertEqual(slow["residency"], "DISK_RESIDENT")
        self.assertEqual(fast["runtimeHint"]["estimatedQueueWaitMs"], 0)
        self.assertEqual(fast["readyCostMs"], 0)

    def test_user_driver_builds_ack_candidate_snapshot(self) -> None:
        user_driver = load_user_driver_module()

        class Candidate:
            provider_name = "/P/backbone"
            service_name = "/Inference/NativeTracer"
            request_id = "/req/1"
            status = True
            message = "ready"
            payload = (
                b"roles=/Backbone;queue=2;readyQueue=1;waitingInputs=0;"
                b"activeWorkers=1;workers=2;idleWorkers=1;runtimeStatus=ready;"
                b"leaseId=l0;leaseExpiresAtMs=12345;"
            )
            telemetry = {"rtt_ms": 3.0}

        snapshot = user_driver.ack_candidates_snapshot([Candidate()])

        self.assertEqual(snapshot[0]["provider"], "/P/backbone")
        self.assertEqual(snapshot[0]["roles"], "/Backbone")
        self.assertEqual(snapshot[0]["queue"], 2)
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

    def test_harness_refreshes_runtime_hints_from_provider_inventory(self) -> None:
        harness = load_harness_module()
        rows = [{
            "assignment": "capacity-pool",
            "role": "/Backbone",
            "provider": "/P/backbone",
            "node": "memphis",
            "service": harness.SERVICE,
        }]
        inventory = {
            "latestByProviderRole": {
                "/P/backbone|/Backbone": {
                    "provider": "/P/backbone",
                    "role": "/Backbone",
                    "fragmentDigest": "sha256:observed-backbone",
                    "backend": "onnxruntime",
                    "path": "artifacts/observed-backbone.onnx",
                    "residency": "GPU_LOADED",
                    "event": "EXECUTION_OBSERVED",
                    "epochMs": "1000",
                },
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "runtime-hints.json"
            harness.write_runtime_hints_json(
                path,
                rows,
                role_execution_delay_ms=50,
                provider_admission_max_active_workers=1,
                provider_admission_max_queue=1,
            )
            refresh = harness.refresh_runtime_hints_json_from_inventory(path, inventory)
            payload = json.loads(path.read_text(encoding="utf-8"))

        hint = payload["providerRoles"]["/P/backbone|/Backbone"]
        self.assertEqual(refresh["updated"], 1)
        self.assertEqual(payload["source"], "MiniNDN harness provider runtime inventory")
        self.assertEqual(hint["source"], "provider-runtime-inventory")
        self.assertEqual(hint["fragmentDigest"], "sha256:observed-backbone")
        self.assertEqual(hint["runtimeHint"]["fragmentResidency"], "GPU_LOADED")
        self.assertEqual(hint["artifactPath"], "artifacts/observed-backbone.onnx")

    def test_provider_ack_runtime_hints_are_aggregated(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            logs = Path(tmp)
            (logs / "provider.log").write_text(
                "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION "
                "provider=/P/backbone roles=/Backbone status=1 "
                "message=\"native DI provider ready\" "
                "payload=\"roles=/Backbone;queue=3;readyQueue=1;"
                "waitingInputs=1;activeWorkers=1;workers=2;idleWorkers=1;"
                "runtimeStatus=ready;leaseId=l0;leaseExpiresAtMs=12345;\"\n",
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

    def test_rps_sweep_dry_run_builds_runtime_aware_commands(self) -> None:
        completed = subprocess.run([
            sys.executable,
            str(SWEEP),
            "--dry-run",
            "--out", "/tmp/ndnsf-di-rps-sweep-dry-run",
            "--rps", "0.2,0.4",
            "--requests", "2",
            "--concurrency", "2",
            "--",
            "--provider-check-timeout", "60",
        ], cwd=str(REPO), text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["status"], "DRY_RUN")
        self.assertEqual(len(payload["commands"]), 2)
        first = " ".join(payload["commands"][0])
        self.assertIn("--runtime-aware-user-planner", first)
        self.assertIn("--target-rps 0.2", first)
        self.assertIn("--enable-native-admission-lease", first)
        self.assertIn("--provider-check-timeout 60", first)

    def test_rps_sweep_dry_run_builds_advisory_comparison_commands(self) -> None:
        completed = subprocess.run([
            sys.executable,
            str(SWEEP),
            "--dry-run",
            "--compare-advisory-coordinator",
            "--out", "/tmp/ndnsf-di-rps-sweep-advisory-dry-run",
            "--rps", "0.2",
            "--requests", "2",
            "--concurrency", "2",
        ], cwd=str(REPO), text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["status"], "DRY_RUN")
        self.assertEqual(len(payload["commands"]), 2)
        pure = " ".join(payload["commands"][0])
        advisory = " ".join(payload["commands"][1])
        self.assertNotIn("--advisory-coordinator", pure)
        self.assertIn("--advisory-coordinator", advisory)

    def test_rps_sweep_dry_run_can_use_capacity_pool(self) -> None:
        completed = subprocess.run([
            sys.executable,
            str(SWEEP),
            "--dry-run",
            "--compare-advisory-coordinator",
            "--capacity-pool",
            "--out", "/tmp/ndnsf-di-rps-sweep-capacity-pool-dry-run",
            "--rps", "0.2",
            "--requests", "2",
            "--concurrency", "2",
        ], cwd=str(REPO), text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["status"], "DRY_RUN")
        self.assertIn("--assignment capacity-pool", " ".join(payload["commands"][0]))
        self.assertIn("--assignment capacity-pool", " ".join(payload["commands"][1]))

    def test_rps_sweep_dry_run_can_use_overload_fast_fail_timeout(self) -> None:
        completed = subprocess.run([
            sys.executable,
            str(SWEEP),
            "--dry-run",
            "--compare-advisory-coordinator",
            "--overload-fast-fail-timeout-ms", "5000",
            "--out", "/tmp/ndnsf-di-rps-sweep-fast-fail-dry-run",
            "--rps", "0.2",
            "--requests", "2",
            "--concurrency", "2",
        ], cwd=str(REPO), text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertIn("--overload-fast-fail-timeout-ms 5000",
                      " ".join(payload["commands"][0]))
        self.assertIn("--overload-fast-fail-timeout-ms 5000",
                      " ".join(payload["commands"][1]))

    def test_native_tracer_dry_run_passes_advisory_to_user_driver(self) -> None:
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
            "--advisory-coordinator",
            "--requests", "1",
            "--concurrency", "1",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertTrue(payload["advisoryCoordinator"])
        self.assertIn("--assignment-csv", payload["userDriverCommand"])
        self.assertIn("--runtime-hints-json", payload["userDriverCommand"])
        self.assertIn("--coordination-service /NDNSF/Coordination/Advisory",
                      payload["userDriverCommand"])

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
