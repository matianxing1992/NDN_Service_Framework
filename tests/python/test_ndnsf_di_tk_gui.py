#!/usr/bin/env python3
"""Non-display tests for the NDNSF-DI Tk GUI helpers."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from ndnsf_distributed_inference.gui import (
    RoleProcessState,
    RuntimeGuiProfile,
    RuntimeRoleProfile,
    build_role_command,
    load_runtime_profile,
    split_extra_args,
    write_runtime_profile,
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


if __name__ == "__main__":
    unittest.main()
