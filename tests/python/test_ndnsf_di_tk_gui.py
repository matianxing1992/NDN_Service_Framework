#!/usr/bin/env python3
"""Non-display tests for the NDNSF-DI Tk GUI helpers."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from ndnsf_distributed_inference.gui import (
    ControllerTabConfig,
    FakeRuntimeFactory,
    NdnsfSvsEnvConfig,
    ProviderTabConfig,
    RoleRuntimeController,
    RoleProcessState,
    RuntimeGuiProfile,
    RuntimeRoleProfile,
    ThreeRoleGuiProfile,
    UserRequestConfig,
    UserTabConfig,
    apply_role_config_file,
    build_arg_parser,
    build_role_command,
    load_runtime_profile,
    load_three_role_profile,
    payload_from_request,
    redact_mapping,
    run_headless,
    split_extra_args,
    write_runtime_profile,
    write_three_role_profile,
)


class TkGuiHelperTests(unittest.TestCase):
    def test_runtime_profile_roundtrip(self) -> None:
        profile = RuntimeGuiProfile(
            controller=RuntimeRoleProfile(
                role="controller",
                config="policy.yaml",
                generated_policy_dir="/tmp/policy",
            ),
            provider=RuntimeRoleProfile(
                role="provider",
                config="policy.yaml",
                provider_id="P2",
                roles="/Stage/0,/Stage/1",
                extra_args="--queue-depth 4",
            ),
            user=RuntimeRoleProfile(
                role="user",
                config="policy.yaml",
                ack_timeout_ms="700",
                timeout_ms="45000",
                extra_args='--payload "hello world"',
            ),
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "profile.json"
            write_runtime_profile(path, profile)
            loaded = load_runtime_profile(path)
        self.assertEqual(loaded.provider.provider_id, "P2")
        self.assertEqual(loaded.provider.roles, "/Stage/0,/Stage/1")
        self.assertEqual(loaded.user.extra_args, '--payload "hello world"')

    def test_build_provider_command_uses_shell_style_extra_args(self) -> None:
        command = build_role_command(
            role="provider",
            script_path="/repo/provider.py",
            config="policy.yaml",
            generated_policy_dir="/tmp/generated",
            group="/group",
            provider_id="P1",
            roles="/Stage/0",
            extra_args='--name "provider one"',
            python_executable="python3",
        )
        self.assertEqual(command[:5], [
            "python3",
            "/repo/provider.py",
            "--config",
            "policy.yaml",
            "--generated-policy-dir",
        ])
        self.assertIn("--provider-id", command)
        self.assertIn("P1", command)
        self.assertEqual(command[-2:], ["--name", "provider one"])

    def test_build_controller_command_does_not_pass_group(self) -> None:
        command = build_role_command(
            role="controller",
            script_path="/repo/controller.py",
            config="policy.yaml",
            generated_policy_dir="/tmp/generated",
            group="/group",
            python_executable="python3",
        )
        self.assertNotIn("--group", command)

    def test_build_user_command_includes_timeouts(self) -> None:
        command = build_role_command(
            role="user",
            script_path="/repo/user.py",
            config="policy.yaml",
            generated_policy_dir="/tmp/generated",
            ack_timeout_ms="900",
            timeout_ms="60000",
            python_executable="python3",
        )
        self.assertIn("--ack-timeout-ms", command)
        self.assertIn("900", command)
        self.assertIn("--timeout-ms", command)
        self.assertIn("60000", command)

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

    def test_three_role_profile_can_migrate_legacy_profile(self) -> None:
        legacy = RuntimeGuiProfile(
            controller=RuntimeRoleProfile(role="controller"),
            provider=RuntimeRoleProfile(
                role="provider",
                group="/legacy/group",
                provider_id="P2",
                service="/Legacy/Service",
                roles="/Stage/0",
            ),
            user=RuntimeRoleProfile(
                role="user",
                group="/legacy/group",
                service="/Legacy/Service",
                ack_timeout_ms="700",
                timeout_ms="9000",
            ),
        )
        migrated = ThreeRoleGuiProfile.from_legacy(legacy)
        self.assertEqual(migrated.provider.group, "/legacy/group")
        self.assertEqual(migrated.provider.service_name, "/Legacy/Service")
        self.assertEqual(migrated.user.request.ack_timeout_ms, 700)

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
