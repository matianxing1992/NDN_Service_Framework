#!/usr/bin/env python3
"""Xvfb-backed Tk widget tests for the NDNSF-DI GUI."""

from __future__ import annotations

import os
import time
import unittest

from ndnsf_distributed_inference.gui import (
    ControllerTabConfig,
    DistributedInferenceGui,
    FakeRuntimeFactory,
    NdnsfSvsEnvConfig,
    ProviderTabConfig,
    ThreeRoleGuiProfile,
    UserRequestConfig,
    UserTabConfig,
)


def wait_until(app: DistributedInferenceGui, predicate, *, timeout_s: float = 2.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        app.update()
        if predicate():
            return True
        time.sleep(0.02)
    app.update()
    return predicate()


@unittest.skipUnless(os.environ.get("DISPLAY"), "Tk widget tests require DISPLAY/Xvfb")
class DistributedInferenceGuiWidgetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.factory = FakeRuntimeFactory()
        self.app = DistributedInferenceGui(factory=self.factory)
        self.app.withdraw()
        self.app.update()

    def tearDown(self) -> None:
        try:
            self.app.stop_all_roles()
            self.app.update()
            self.app.destroy()
        except Exception:
            pass

    def test_primary_tabs_are_user_provider_controller(self) -> None:
        tab_names = [
            self.app.notebook.tab(index, "text")
            for index in range(self.app.notebook.index("end"))
        ]
        self.assertEqual(tab_names[:3], ["User", "Provider", "Controller"])

    def test_apply_profile_updates_editable_fields_and_roundtrips(self) -> None:
        profile = ThreeRoleGuiProfile(
            env=NdnsfSvsEnvConfig(
                expected_rps="25",
                publication_fetch_window="32",
                max_suppression_ms="2",
            ),
            controller=ControllerTabConfig(
                controller_prefix="/demo/controller",
                policy_file="examples/hello.policies",
            ),
            provider=ProviderTabConfig(
                provider_id="P9",
                provider_prefix="/demo/provider",
                service_name="/HELLO",
                ack_message="ready-from-widget-test",
            ),
            user=UserTabConfig(
                user="/demo/user",
                request=UserRequestConfig(
                    service_name="/HELLO",
                    payload="WIDGET-HELLO",
                    ack_timeout_ms=777,
                    timeout_ms=8888,
                ),
            ),
        )
        self.app.apply_profile(profile)
        self.app.update()

        self.assertEqual(self.app.controller_tab.value("controller_prefix"), "/demo/controller")
        self.assertEqual(self.app.provider_tab.value("provider_id"), "P9")
        self.assertEqual(self.app.provider_tab.value("ack_message"), "ready-from-widget-test")
        self.assertEqual(self.app.user_tab.value("user"), "/demo/user")
        self.assertEqual(self.app.user_tab.payload_pane.get(), "WIDGET-HELLO")
        self.assertEqual(self.app.user_tab.value("env_expected_rps"), "25")

        roundtrip = self.app.profile()
        self.assertEqual(roundtrip.provider.provider_id, "P9")
        self.assertEqual(roundtrip.user.request.payload, "WIDGET-HELLO")
        self.assertEqual(roundtrip.env.publication_fetch_window, "32")

    def test_run_all_buttons_start_fake_role_runtimes(self) -> None:
        self.app.run_all_roles()
        for tab in (self.app.controller_tab, self.app.provider_tab, self.app.user_tab):
            if tab.controller.thread is not None:
                tab.controller.thread.join(timeout=2)
        self.assertTrue(wait_until(
            self.app,
            lambda: (
                self.app.controller_tab.status_var.get() == "running"
                and self.app.provider_tab.status_var.get() == "running"
                and self.app.user_tab.status_var.get() == "running"
            ),
        ))
        self.assertTrue(self.factory.created["controller"].started)
        self.assertTrue(self.factory.created["provider"].started)
        self.assertTrue(self.factory.created["user"].started)

    def test_send_request_button_updates_response_pane(self) -> None:
        self.app.user_tab.run_role()
        self.app.user_tab.controller.thread.join(timeout=2)
        self.assertTrue(wait_until(
            self.app,
            lambda: self.app.user_tab.status_var.get() == "running",
        ))

        self.app.user_tab.payload_pane.set("HELLO-FROM-TK")
        self.app.user_tab.send_request()
        self.assertTrue(wait_until(
            self.app,
            lambda: "Response" in self.app.user_tab.response_pane.get(),
        ))
        response_text = self.app.user_tab.response_pane.get()
        self.assertIn("status: True", response_text)
        self.assertIn("payload:\nHELLO", response_text)
        self.assertEqual(
            self.factory.created["user"].requests,
            [("/HELLO", b"HELLO-FROM-TK")],
        )


if __name__ == "__main__":
    unittest.main()
