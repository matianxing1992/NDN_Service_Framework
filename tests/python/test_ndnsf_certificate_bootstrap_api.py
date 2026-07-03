#!/usr/bin/env python3
"""Certificate bootstrap Python API/config tests."""

from __future__ import annotations

import unittest

from ndnsf import ControllerConfig, ProviderConfig, UserConfig


class CertificateBootstrapApiTest(unittest.TestCase):
    def test_controller_config_adds_bootstrap_token_file_flag(self) -> None:
        app = ControllerConfig(
            policy_file="examples/hello.policies",
            bootstrap_token_file="examples/hello.bootstrap-tokens",
        ).as_application()

        self.assertEqual(app.args, (
            "--policy-file",
            "examples/hello.policies",
            "--bootstrap-token-file",
            "examples/hello.bootstrap-tokens",
        ))

    def test_provider_config_adds_bootstrap_token_before_existing_args(self) -> None:
        app = ProviderConfig(
            name="provider",
            binary="App_Provider",
            bootstrap_token="prov045A",
            args=("--provider-id", "A"),
        ).as_application()

        self.assertEqual(app.args, (
            "--bootstrap-token",
            "prov045A",
            "--provider-id",
            "A",
        ))

    def test_user_config_adds_bootstrap_token_before_existing_args(self) -> None:
        app = UserConfig(
            name="user",
            binary="App_User",
            bootstrap_token="user045A",
            args=("--service", "/HELLO"),
        ).as_application()

        self.assertEqual(app.args, (
            "--bootstrap-token",
            "user045A",
            "--service",
            "/HELLO",
        ))


if __name__ == "__main__":
    unittest.main()
