#!/usr/bin/env python3

import asyncio
from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import time
import unittest


REPO = Path(__file__).resolve().parents[2]
GRPC_EXPERIMENT = REPO / "Experiments" / "gRPC"
sys.path.insert(0, str(GRPC_EXPERIMENT))

import greeter_failover_client as failover  # noqa: E402
import greeter_server  # noqa: E402


class ThreeProviderFailoverUnitTests(unittest.TestCase):
    def test_targets_require_three_unique_provider_endpoints(self):
        targets = failover.parse_target_specs([
            "ucla=127.0.0.1:50051",
            "wustl=127.0.0.1:50052",
            "uiuc=127.0.0.1:50053",
        ])
        self.assertEqual([item.provider_id for item in targets],
                         ["ucla", "wustl", "uiuc"])
        with self.assertRaisesRegex(ValueError, "exactly three"):
            failover.parse_target_specs(["ucla=127.0.0.1:50051"])
        with self.assertRaisesRegex(ValueError, "duplicate provider id"):
            failover.parse_target_specs([
                "ucla=127.0.0.1:50051",
                "ucla=127.0.0.1:50052",
                "uiuc=127.0.0.1:50053",
            ])

    def test_four_provider_targets_are_supported_without_parallel_retries(self):
        targets = failover.parse_target_specs([
            "ucla=127.0.0.1:50051",
            "wustl=127.0.0.1:50052",
            "uiuc=127.0.0.1:50053",
            "arizona=127.0.0.1:50054",
        ])
        self.assertEqual(len(targets), 4)
        self.assertEqual(failover.rotated([item.provider_id for item in targets], 3),
                         ["arizona", "ucla", "wustl", "uiuc"])

    def test_single_provider_control_requires_explicit_opt_in(self):
        target = ["ucla=127.0.0.1:50051"]
        with self.assertRaisesRegex(ValueError, "exactly three"):
            failover.parse_target_specs(target)
        parsed = failover.parse_target_specs(
            target, allow_single_provider=True)
        self.assertEqual([item.provider_id for item in parsed], ["ucla"])

    def test_request_order_rotates_and_retry_policy_is_explicit(self):
        providers = ["ucla", "wustl", "uiuc"]
        self.assertEqual(failover.rotated(providers, 0), providers)
        self.assertEqual(failover.rotated(providers, 1),
                         ["wustl", "uiuc", "ucla"])
        self.assertEqual(failover.rotated(providers, 2),
                         ["uiuc", "ucla", "wustl"])
        self.assertTrue(failover.is_retryable(failover.grpc.StatusCode.UNAVAILABLE))
        self.assertTrue(failover.is_retryable(
            failover.grpc.StatusCode.DEADLINE_EXCEEDED))
        self.assertFalse(failover.is_retryable(
            failover.grpc.StatusCode.INVALID_ARGUMENT))

    def test_summary_contains_harness_contract(self):
        metrics = failover.Metrics(
            sent=3,
            success=2,
            failures=1,
            attempts=4,
            failovers=2,
            handler_executions=2,
            latencies_ms=[10.0, 20.0],
        )
        metrics.status_counts.update({"OK": 2, "UNAVAILABLE": 2})
        summary = failover.format_summary(
            metrics,
            health_checks=9,
            health_success=8,
            duration_s=1.0,
            offered_rps=3.0,
        )
        for field in (
            "sent=3", "success=2", "failures=1", "attempts=4",
            "failovers=2", "health_checks=9", "health_success=8",
            "handler_executions_observed=2", "p50_ms=10.000",
            "p95_ms=20.000", "p99_ms=20.000",
            "actual_success_rps=2.000", "application_rpc_calls=13",
            "application_messages=26",
            "measurement_start_offset_s=0.000000",
            "measurement_start_lateness_ms=0.000",
            "pre_measurement_health_checks=0",
            "pre_measurement_health_success=0",
            "health_directed_selections=0",
            "health_routing=enabled",
            "message_definition=rpc_request_plus_terminal_event",
        ):
            self.assertIn(field, summary)

    def test_health_event_accounting_uses_half_open_measurement_window(self):
        events = [
            (9.9, True),
            (10.0, True),
            (10.2, False),
            (10.4, True),
        ]
        self.assertEqual(
            failover.count_health_events(events, 10.0, 10.4), (2, 1))


class MeasurementStartBarrierTests(unittest.IsolatedAsyncioTestCase):
    async def test_waits_for_absolute_monotonic_target(self):
        process_started_at = time.monotonic()
        target = process_started_at + 0.05
        actual, offset_s, lateness_s = await failover.wait_for_measurement_start(
            process_started_at=process_started_at,
            delay_s=0.0,
            absolute_monotonic_s=target,
            tolerance_s=0.1,
        )
        self.assertGreaterEqual(actual, target)
        self.assertGreaterEqual(offset_s, 0.05)
        self.assertLess(lateness_s, 0.1)

    async def test_rejects_absolute_and_relative_targets_together(self):
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            await failover.wait_for_measurement_start(
                process_started_at=time.monotonic(),
                delay_s=1.0,
                absolute_monotonic_s=time.monotonic() + 1.0,
                tolerance_s=0.1,
            )


class SchedulingCleanupTests(unittest.IsolatedAsyncioTestCase):
    async def test_request_exception_cancels_and_gathers_siblings(self):
        class FaultingClient:
            def __init__(self):
                self.started = 0
                self.all_started = asyncio.Event()
                self.sibling_cancelled = asyncio.Event()

            async def execute_request(self, request_id, metrics, measured=True):
                self.started += 1
                if self.started == 2:
                    self.all_started.set()
                await self.all_started.wait()
                if request_id == 0:
                    raise RuntimeError("injected request failure")
                try:
                    await asyncio.Event().wait()
                except asyncio.CancelledError:
                    self.sibling_cancelled.set()
                    raise

        client = FaultingClient()
        with self.assertRaisesRegex(RuntimeError, "injected request failure"):
            await failover.schedule_requests(
                client,
                count=2,
                interval_s=0.0,
                first_request_id=0,
                metrics=failover.Metrics(),
                measured=True,
            )
        self.assertTrue(client.sibling_cancelled.is_set())
        self.assertFalse(any(
            task.get_name().startswith("grpc-request-")
            for task in asyncio.all_tasks()
            if task is not asyncio.current_task()))


class NoResponseFailoverTests(unittest.IsolatedAsyncioTestCase):
    async def test_none_reply_is_accounted_as_failed_attempt(self):
        class NullCall:
            def __await__(self):
                async def resolve():
                    return None
                return resolve().__await__()

            async def trailing_metadata(self):
                return None

        class NullEndpoint:
            def __init__(self, provider_id):
                self.provider_id = provider_id
                self.healthy = False
                self.last_success_at = 0.0
                self.last_status = "UNKNOWN"
                self.last_checked_at = 0.0

            def is_fresh_healthy(self, now, stale_s):
                return False

            def say_hello(self, request, *, timeout, metadata):
                return NullCall()

        client = object.__new__(failover.ThreeProviderFailoverClient)
        client.endpoints = [
            NullEndpoint(provider_id)
            for provider_id in ("ucla", "wustl", "uiuc", "arizona")
        ]
        client.global_deadline_s = 1.0
        client.attempt_timeout_s = 0.2
        client.health_stale_s = 1.0
        metrics = failover.Metrics()

        with redirect_stdout(io.StringIO()):
            outcome = await client.execute_request(0, metrics)

        self.assertFalse(outcome.success)
        self.assertEqual(outcome.status, "NO_RESPONSE")
        self.assertEqual(metrics.sent, 1)
        self.assertEqual(metrics.attempts, 4)
        self.assertEqual(metrics.failovers, 3)
        self.assertEqual(metrics.failures, 1)
        self.assertEqual(metrics.status_counts["NO_RESPONSE"], 4)


class ThreeProviderFailoverIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.servers = []
        self.greeters = []
        self.targets = []
        for provider_id in ("ucla", "wustl", "uiuc"):
            server, greeter, port = greeter_server.build_server(
                bind="127.0.0.1:0",
                delay_ms=1,
                quiet=True,
                provider_id=provider_id,
            )
            server.start()
            self.servers.append(server)
            self.greeters.append(greeter)
            self.targets.append(failover.TargetSpec(
                provider_id, f"127.0.0.1:{port}"))
        self.client = failover.ThreeProviderFailoverClient(
            self.targets,
            global_deadline_s=1.0,
            attempt_timeout_s=0.2,
            health_interval_s=1.0,
            health_timeout_s=0.2,
            health_stale_s=1.0,
            prewarm_timeout_s=1.0,
            quiet=True,
        )

    async def asyncTearDown(self):
        await self.client.close()
        for server in self.servers:
            server.stop(0).wait(timeout=1.0)

    async def test_prewarm_health_and_retry_to_next_provider(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            health = await self.client.probe_all()
        self.assertEqual(health, [True, True, True])
        self.assertTrue(all(endpoint.channel_ready
                            for endpoint in self.client.endpoints))
        self.assertTrue(all(endpoint.last_success_at > 0
                            for endpoint in self.client.endpoints))

        self.servers[0].stop(0).wait(timeout=1.0)
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)

        self.assertTrue(outcome.success)
        self.assertEqual(outcome.provider_id, "wustl")
        self.assertEqual(outcome.response_message, "Hello, Test-0")
        self.assertEqual(metrics.sent, 1)
        self.assertEqual(metrics.success, 1)
        self.assertEqual(metrics.attempts, 2)
        self.assertEqual(metrics.failovers, 1)
        self.assertEqual(metrics.status_counts["UNAVAILABLE"], 1)
        self.assertEqual(metrics.status_counts["OK"], 1)
        self.assertEqual(metrics.handler_executions, 1)
        self.assertEqual(self.greeters[1].counters()["handler_executions"], 1)
        self.assertEqual(self.greeters[1].counters()["health_checks"], 1)

    async def test_strict_sequential_mode_does_not_probe_or_reorder(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
        self.servers[0].stop(0).wait(timeout=1.0)
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)
        self.assertTrue(outcome.success)
        self.assertEqual(outcome.provider_id, "wustl")
        self.assertEqual(metrics.attempts, 2)
        self.assertEqual(metrics.failovers, 1)
        self.assertEqual(metrics.health_directed_selections, 0)
        self.assertEqual(self.greeters[1].counters()["health_checks"], 0)

    async def test_parallel_first_success_fans_out_without_serial_failovers(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
        self.client.parallel = True
        self.greeters[0].service_error_code = failover.grpc.StatusCode.UNAVAILABLE
        self.greeters[2].service_error_code = failover.grpc.StatusCode.UNAVAILABLE
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)
        self.assertTrue(outcome.success)
        self.assertEqual(outcome.provider_id, "wustl")
        self.assertEqual(metrics.sent, 1)
        self.assertEqual(metrics.attempts, 3)
        self.assertEqual(metrics.failovers, 0)
        self.assertEqual(metrics.parallel_issued, 3)
        self.assertEqual(metrics.parallel_winners, 1)
        self.assertEqual(metrics.failures, 0)
        self.assertGreaterEqual(metrics.status_counts["UNAVAILABLE"], 2)

    async def test_two_retryable_failures_issue_two_failovers_before_c(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
        self.greeters[0].service_error_code = failover.grpc.StatusCode.UNAVAILABLE
        self.greeters[1].service_error_code = failover.grpc.StatusCode.UNAVAILABLE
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)
        self.assertTrue(outcome.success)
        self.assertEqual(outcome.provider_id, "uiuc")
        self.assertEqual(metrics.attempts, 3)
        self.assertEqual(metrics.failovers, 2)
        self.assertEqual(metrics.health_directed_selections, 0)
        self.assertEqual(metrics.status_counts["UNAVAILABLE"], 2)
        for attempt, greeter in enumerate(self.greeters, start=1):
            snapshot = greeter.stats_snapshot()
            self.assertEqual(snapshot["handler_executions"], 1)
            self.assertEqual(snapshot["request_id_counts"], {"0": 1})
            self.assertEqual(snapshot["service_events"][0]["attempt"], attempt)

    async def test_health_directed_initial_selection_is_not_a_failover(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
        self.client.endpoints[0].healthy = False
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)
        self.assertTrue(outcome.success)
        self.assertEqual(outcome.provider_id, "wustl")
        self.assertEqual(metrics.attempts, 1)
        self.assertEqual(metrics.failovers, 0)
        self.assertEqual(metrics.health_directed_selections, 1)

    async def test_all_slow_providers_obey_one_strict_global_deadline(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
        for greeter in self.greeters:
            greeter.delay_s = 0.2
        self.client.global_deadline_s = 0.12
        self.client.attempt_timeout_s = 0.08
        metrics = failover.Metrics()
        started = time.monotonic()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)
        elapsed = time.monotonic() - started
        self.assertFalse(outcome.success)
        self.assertLessEqual(elapsed, 0.17)
        self.assertGreaterEqual(metrics.attempts, 1)
        self.assertLessEqual(metrics.attempts, 3)
        self.assertEqual(metrics.failovers, metrics.attempts - 1)
        self.assertEqual(metrics.failures, 1)

    async def test_non_retryable_error_stops_after_first_provider(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
        self.greeters[0].service_error_code = (
            failover.grpc.StatusCode.INVALID_ARGUMENT)
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcome = await self.client.execute_request(0, metrics)
        self.assertFalse(outcome.success)
        self.assertEqual(outcome.status, "INVALID_ARGUMENT")
        self.assertEqual(metrics.attempts, 1)
        self.assertEqual(metrics.failovers, 0)
        self.assertEqual(self.greeters[0].counters()["handler_executions"], 1)
        self.assertEqual(self.greeters[1].counters()["handler_executions"], 0)
        self.assertEqual(self.greeters[2].counters()["handler_executions"], 0)

    async def test_request_id_rotation_changes_actual_first_rpc_provider(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
        metrics = failover.Metrics()
        with redirect_stdout(io.StringIO()):
            outcomes = [
                await self.client.execute_request(request_id, metrics)
                for request_id in range(3)
            ]
        self.assertEqual(
            [outcome.provider_id for outcome in outcomes],
            ["ucla", "wustl", "uiuc"],
        )
        self.assertEqual(metrics.attempts, 3)
        self.assertEqual(metrics.failovers, 0)
        self.assertEqual(
            [greeter.counters()["handler_executions"]
             for greeter in self.greeters],
            [1, 1, 1],
        )

    async def test_require_all_prewarmed_rejects_one_missing_provider(self):
        args = failover.build_parser().parse_args([
            "--target", f"ucla={self.targets[0].address}",
            "--target", f"wustl={self.targets[1].address}",
            "--target", "uiuc=127.0.0.1:1",
            "--require-all-prewarmed",
            "--prewarm-timeout-s", "0.05",
            "--count", "1",
            "--quiet",
        ])
        args._process_started_at = time.monotonic()
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "must prewarm"):
                await failover.run_from_args(args)
        self.assertFalse(any(
            task.get_name().startswith("grpc-health-")
            for task in asyncio.all_tasks()
            if task is not asyncio.current_task()))

    async def test_close_cancels_health_loops_without_task_leak(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
            self.client.start_health_monitoring()
            await asyncio.sleep(0.02)
        tasks = list(self.client._health_tasks)
        await self.client.close()
        self.assertTrue(all(task.done() for task in tasks))
        self.assertEqual(self.client._health_tasks, [])
        self.assertFalse(any(
            task.get_name().startswith("grpc-health-")
            for task in asyncio.all_tasks()
            if task is not asyncio.current_task()))

    async def test_server_owned_stats_snapshot_and_reset_are_exact(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
            await self.client.execute_request(0, failover.Metrics())
            snapshot = await self.client.stats_snapshot_all(timeout_s=1.0)
        ucla = snapshot["providers"]["ucla"]
        self.assertEqual(ucla["handler_executions"], 1)
        self.assertEqual(ucla["health_checks"], 1)
        self.assertEqual(ucla["health_success"], 1)
        self.assertEqual(ucla["request_id_counts"], {"0": 1})
        self.assertEqual(ucla["service_events"][0]["status"], "OK")
        with redirect_stdout(io.StringIO()):
            reset = await self.client.reset_stats_all(timeout_s=1.0)
        for provider in reset["providers"].values():
            self.assertEqual(provider["handler_executions"], 0)
            self.assertEqual(provider["health_checks"], 0)
            self.assertEqual(provider["service_events"], [])
            self.assertEqual(provider["health_events"], [])
            self.assertEqual(provider["stats_epoch"], 1)

    async def test_cli_smoke_emits_marker_and_summary(self):
        command = [
            sys.executable,
            str(GRPC_EXPERIMENT / "greeter_failover_client.py"),
        ]
        for target in self.targets:
            command.extend([
                "--target", f"{target.provider_id}={target.address}"])
        command.extend([
            "--smoke",
            "--quiet",
            "--prewarm-timeout-s", "1",
            "--global-deadline-s", "1",
            "--attempt-timeout-s", "0.2",
        ])
        process = await asyncio.create_subprocess_exec(
            *command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        output_bytes, _ = await asyncio.wait_for(
            process.communicate(), timeout=5.0)
        output = output_bytes.decode("utf-8", errors="replace")
        self.assertEqual(process.returncode, 0, output)
        self.assertIn("GRPC_FAILOVER_RATE sent=3 success=3", output)
        self.assertIn("application_messages=", output)
        self.assertIn("\nSMOKE_OK\n", output)

    async def test_cli_stats_only_returns_all_provider_json_snapshots(self):
        with redirect_stdout(io.StringIO()):
            await self.client.prewarm_all()
            await self.client.probe_all()
            await self.client.execute_request(0, failover.Metrics())
        command = [
            sys.executable,
            str(GRPC_EXPERIMENT / "greeter_failover_client.py"),
        ]
        for target in self.targets:
            command.extend([
                "--target", f"{target.provider_id}={target.address}"])
        command.extend(["--stats-only", "--stats-timeout-s", "1"])
        process = await asyncio.create_subprocess_exec(
            *command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        output_bytes, _ = await asyncio.wait_for(
            process.communicate(), timeout=5.0)
        output = output_bytes.decode("utf-8", errors="replace")
        self.assertEqual(process.returncode, 0, output)
        provider_lines = [
            line for line in output.splitlines()
            if line.startswith("GRPC_PROVIDER_STATS_JSON ")]
        self.assertEqual(len(provider_lines), 3, output)
        self.assertEqual(
            sum(line.startswith("GRPC_STATS_SNAPSHOT_JSON ")
                for line in output.splitlines()),
            1,
        )
        ucla = next(line for line in provider_lines
                    if '"provider_id":"ucla"' in line)
        self.assertIn('"handler_executions":1', ucla)
        self.assertIn('"request_id_counts":{"0":1}', ucla)


if __name__ == "__main__":
    unittest.main()
