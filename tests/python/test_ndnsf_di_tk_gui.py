#!/usr/bin/env python3
"""Non-display tests for the NDNSF-DI Tk GUI helpers."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ndnsf_distributed_inference.gui import (
    ControllerTabConfig,
    FakeRuntimeFactory,
    NdnsfSvsEnvConfig,
    ProviderTabConfig,
    RoleRuntimeController,
    RoleProcessState,
    ThreeRoleGuiProfile,
    UserRequestConfig,
    UserTabConfig,
    apply_role_config_file,
    build_arg_parser,
    build_qwen_minindn_command,
    format_core_envelope_summary,
    load_three_role_profile,
    payload_from_request,
    redact_mapping,
    run_headless,
    run_headless_qwen_minindn,
    split_extra_args,
    write_three_role_profile,
)


class TkGuiHelperTests(unittest.TestCase):
    def test_split_extra_args_preserves_quoted_value(self) -> None:
        self.assertEqual(split_extra_args('--payload "hello world"'), [
            "--payload",
            "hello world",
        ])

    def test_role_process_state_transitions(self) -> None:
        state = RoleProcessState("provider")
        self.assertEqual(state.mark_starting(), "starting")
        self.assertEqual(state.mark_running(1234), "running pid=1234")
        self.assertEqual(state.mark_stopping(), "stopping")
        self.assertEqual(state.mark_exited(0), "exited rc=0")
        self.assertIsNone(state.pid)
        self.assertEqual(state.mark_running(5678), "running pid=5678")
        self.assertEqual(state.mark_exited(2), "failed rc=2")

    def test_three_role_profile_roundtrip_redacts_tokens_by_default(self) -> None:
        profile = ThreeRoleGuiProfile(
            env=NdnsfSvsEnvConfig(publication_fetch_window="32"),
            controller=ControllerTabConfig(
                controller_prefix="/demo/controller",
                bootstrap_token_file="tokens.txt",
            ),
            provider=ProviderTabConfig(
                provider_id="P1",
                bootstrap_token="provider-secret",
                service_name="/HELLO",
            ),
            user=UserTabConfig(
                user="/demo/user",
                bootstrap_token="user-secret",
                request=UserRequestConfig(service_name="/HELLO", payload="hi"),
            ),
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "profile.json"
            write_three_role_profile(path, profile)
            raw = path.read_text(encoding="utf-8")
            self.assertNotIn("provider-secret", raw)
            self.assertNotIn("user-secret", raw)
            loaded = load_three_role_profile(path)
        self.assertEqual(loaded.provider.provider_id, "P1")
        self.assertEqual(loaded.user.request.payload, "hi")
        self.assertEqual(loaded.env.publication_fetch_window, "32")

    def test_payload_codecs(self) -> None:
        self.assertEqual(payload_from_request(UserRequestConfig(payload="hello")), b"hello")
        self.assertEqual(
            payload_from_request(UserRequestConfig(payload_encoding="json", payload='{"b": 2, "a": 1}')),
            b'{"a": 1, "b": 2}',
        )
        self.assertEqual(
            payload_from_request(UserRequestConfig(payload_encoding="hex", payload="4849")),
            b"HI",
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "payload.bin"
            path.write_bytes(b"file-data")
            self.assertEqual(
                payload_from_request(UserRequestConfig(payload_encoding="file", payload=str(path))),
                b"file-data",
            )

    def test_fake_runtime_controller_starts_only_when_run_is_called(self) -> None:
        factory = FakeRuntimeFactory()
        statuses: list[str] = []
        controller = RoleRuntimeController(
            "provider",
            factory=factory,
            status_callback=statuses.append,
        )
        self.assertNotIn("provider", factory.created)
        controller.run(ProviderTabConfig(provider_id="P3"))
        controller.thread.join(timeout=2)
        self.assertIn("provider", factory.created)
        self.assertTrue(factory.created["provider"].started)
        self.assertEqual(controller.status, "running")
        controller.stop()
        self.assertEqual(controller.status, "stopped")

    def test_fake_user_request_dispatch(self) -> None:
        factory = FakeRuntimeFactory()
        controller = RoleRuntimeController("user", factory=factory)
        controller.run(UserTabConfig(user="/demo/user"))
        controller.thread.join(timeout=2)
        response = controller.request_user(UserRequestConfig(
            service_name="/HELLO",
            payload="HELLO",
            ack_timeout_ms=100,
            timeout_ms=1000,
        ))
        self.assertTrue(response.status)
        self.assertEqual(response.payload, b"HELLO")
        self.assertEqual(factory.created["user"].requests, [("/HELLO", b"HELLO")])

    def test_redact_mapping_hides_secret_values(self) -> None:
        redacted = redact_mapping({
            "bootstrap_token": "abcdef123456",
            "nested": {"password": "secret"},
            "plain": "visible",
        })
        self.assertNotIn("abcdef123456", str(redacted))
        self.assertNotIn("secret", str(redacted))
        self.assertEqual(redacted["plain"], "visible")

    def test_role_config_file_can_override_user_only(self) -> None:
        profile = ThreeRoleGuiProfile()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "user.config"
            path.write_text(
                '{"user": "/demo/user1", "request": {"service_name": "/HELLO", "payload": "ping"}}',
                encoding="utf-8",
            )
            apply_role_config_file(profile, "user", path)
        self.assertEqual(profile.user.user, "/demo/user1")
        self.assertEqual(profile.user.request.service_name, "/HELLO")
        self.assertEqual(profile.user.request.payload, "ping")
        self.assertEqual(profile.provider.provider_id, "A")

    def test_headless_arg_parser_accepts_single_dash_underscore_flags(self) -> None:
        args = build_arg_parser().parse_args([
            "-headless",
            "-user_auto_run",
            "-user_config=user1.config",
            "--runtime-mode",
            "fake",
        ])
        self.assertTrue(args.headless)
        self.assertTrue(args.user_auto_run)
        self.assertEqual(args.user_config, "user1.config")
        self.assertEqual(args.runtime_mode, "fake")

    def test_headless_arg_parser_accepts_qwen_minindn_mode(self) -> None:
        args = build_arg_parser().parse_args([
            "--headless",
            "--headless-experiment",
            "qwen-minindn",
            "--experiment-runtime-profile",
            "examples/di-native-tracer.runtime.json",
            "--experiment-requests",
            "2",
            "--experiment-concurrency",
            "1",
            "--experiment-provider-check-timeout",
            "60",
            "--experiment-dry-run",
        ])
        self.assertEqual(args.headless_experiment, "qwen-minindn")
        self.assertEqual(args.experiment_runtime_profile, "examples/di-native-tracer.runtime.json")
        self.assertEqual(args.experiment_requests, 2)
        self.assertTrue(args.experiment_dry_run)

    def test_qwen_minindn_command_uses_full_network_runtime_profile(self) -> None:
        profile = ThreeRoleGuiProfile(
            provider=ProviderTabConfig(
                runtime_profile="examples/di-native-tracer.runtime.json",
            ),
        )
        args = build_arg_parser().parse_args([
            "--headless",
            "--headless-experiment",
            "qwen-minindn",
            "--experiment-out",
            "/tmp/qwen-gui-headless",
            "--experiment-requests",
            "3",
            "--experiment-concurrency",
            "2",
            "--experiment-provider-check-timeout",
            "45",
            "--experiment-target-rps",
            "0.5",
            "--experiment-open-loop-duration-s",
            "10",
            "--experiment-open-loop-driver-mode",
            "threaded",
            "--experiment-dry-run",
        ])
        command, out_dir = build_qwen_minindn_command(profile, args)
        self.assertEqual(out_dir, Path("/tmp/qwen-gui-headless"))
        self.assertIn("Experiments/NDNSF_DI_NativeTracer_Minindn.py", command)
        self.assertIn("--runtime-profile", command)
        self.assertIn("examples/di-native-tracer.runtime.json", command)
        self.assertIn("--assignment", command)
        self.assertIn("llm-proportional", command)
        self.assertIn("--policy-bundle", command)
        self.assertIn("--llm-planner-mode", command)
        self.assertIn("--no-local-execution-only", command)
        self.assertIn("--full-network", command)
        self.assertIn("--requests", command)
        self.assertIn("3", command)
        self.assertIn("--concurrency", command)
        self.assertIn("2", command)
        self.assertIn("--provider-check-timeout", command)
        self.assertIn("45", command)
        self.assertIn("--target-rps", command)
        self.assertIn("0.5", command)
        self.assertIn("--open-loop-duration-s", command)
        self.assertIn("--open-loop-driver-mode", command)
        self.assertIn("threaded", command)
        self.assertNotIn("--dependency-envelope-mode", command)
        self.assertIn("10.0", command)
        self.assertIn("--dry-run", command)

    def test_qwen_minindn_headless_exposes_core_envelope_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "qwen"
            summary_path = out_dir / "summary.json"
            out_dir.mkdir()
            summary_path.write_text(json.dumps({
                "status": "SUCCESS",
                "miniNDNRun": "completed",
                "runnerClassification": "onnxruntime-cpu",
                "executionEvidence": [{"providerName": "/P/backbone"}],
                "userExecution": {"status": "executed"},
                "dependencyExecution": {"status": "executed"},
                "coreEnvelopeSummary": {
                    "eventCount": 2,
                    "envelopeCounts": {"providerCapabilityHint": 2},
                    "providerReadiness": {"ready": 2},
                },
                "providerAckRuntimeHints": {
                    "eventCount": 2,
                    "providers": {"/P/backbone": {"ackEvents": 2}},
                },
            }), encoding="utf-8")
            args = build_arg_parser().parse_args([
                "--headless",
                "--headless-experiment",
                "qwen-minindn",
                "--experiment-out",
                str(out_dir),
            ])
            with mock.patch(
                "ndnsf_distributed_inference.gui.subprocess.run",
                return_value=mock.Mock(returncode=0, stdout="ok"),
            ):
                summary = run_headless_qwen_minindn(args)

        self.assertTrue(summary["ok"])
        self.assertEqual(summary["runnerClassification"], "onnxruntime-cpu")
        self.assertEqual(summary["executionEvidence"][0]["providerName"],
                         "/P/backbone")
        self.assertEqual(summary["coreEnvelopeSummary"]["eventCount"], 2)
        self.assertEqual(
            summary["coreEnvelopeSummary"]["envelopeCounts"]["providerCapabilityHint"],
            2,
        )
        self.assertEqual(summary["providerAckRuntimeHints"]["eventCount"], 2)

    def test_core_envelope_summary_formatter_reports_gui_fields(self) -> None:
        text = format_core_envelope_summary(
            {
                "eventCount": 2,
                "envelopeCounts": {"providerCapabilityHint": 2, "serviceOperationStatus": 1},
                "providerReadiness": {"ready": 1, "notReady": 1},
                "reasonCodes": {"QUEUE_FULL": 1},
                "servicePayloadSchemas": {"ndnsf-di-capability-v1": 2},
                "operationStates": {"RUNNING": 1},
                "latestProviders": {
                    "/P/backbone": {
                        "ready": True,
                        "queueLength": 3,
                        "activeWorkCount": 1,
                        "reasonCode": "",
                        "servicePayloadSchema": "ndnsf-di-capability-v1",
                    },
                },
            },
            {
                "providers": {
                    "/P/backbone": {
                        "ackEvents": 2,
                        "successfulAckEvents": 1,
                        "negativeAckEvents": 1,
                        "latest": {"queue": 3, "runtimeStatus": "ready"},
                    },
                },
            },
        )
        self.assertIn("ACK events scanned: 2", text)
        self.assertIn("Provider readiness: notReady=1, ready=1", text)
        self.assertIn("Reason codes: QUEUE_FULL=1", text)
        self.assertIn("/P/backbone: ready=True queue=3 active=1", text)
        self.assertIn("Legacy ACK runtime hints", text)

    def test_headless_fake_all_roles_and_request_writes_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "headless.json"
            args = build_arg_parser().parse_args([
                "--headless",
                "--runtime-mode",
                "fake",
                "--controller-auto-run",
                "--provider-auto-run",
                "--user-auto-run",
                "--send-user-request",
                "--output-json",
                str(output),
            ])
            summary = run_headless(args)
            written = output.read_text(encoding="utf-8")
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["auto_run_roles"], ["controller", "provider", "user"])
        self.assertEqual(summary["request"]["payload_text"], "HELLO")
        self.assertIn('"runtime_mode": "fake"', written)

    def test_headless_request_requires_user_auto_run(self) -> None:
        args = build_arg_parser().parse_args([
            "--headless",
            "--runtime-mode",
            "fake",
            "--send-user-request",
        ])
        summary = run_headless(args)
        self.assertFalse(summary["ok"])
        self.assertIn("--send-user-request requires --user-auto-run", summary["errors"])


if __name__ == "__main__":
    unittest.main()
