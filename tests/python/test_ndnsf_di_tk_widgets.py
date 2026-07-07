#!/usr/bin/env python3
"""Xvfb-backed Tk widget tests for the NDNSF-DI GUI."""

from __future__ import annotations

import os
import csv
import json
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

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
        self.assertIn("Qwen MiniNDN", tab_names)

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

    def test_qwen_minindn_tab_builds_same_full_network_command(self) -> None:
        tab = self.app.qwen_minindn
        tab.fields["use_sudo"].set(False)  # type: ignore[union-attr]
        tab.fields["dry_run"].set(True)  # type: ignore[union-attr]
        command, out_dir = tab.experiment_command()
        self.assertEqual(out_dir, Path("/tmp/ndnsf-di-gui-qwen-minindn"))
        self.assertIn("Experiments/NDNSF_DI_NativeTracer_Minindn.py", command)
        self.assertIn("--full-network", command)
        self.assertIn("--no-local-execution-only", command)
        self.assertIn("--assignment", command)
        self.assertIn("llm-proportional", command)
        self.assertIn("--dry-run", command)

    def test_qwen_minindn_tab_builds_sweep_commands(self) -> None:
        tab = self.app.qwen_minindn
        tab.fields["use_sudo"].set(False)  # type: ignore[union-attr]
        tab.fields["dry_run"].set(True)  # type: ignore[union-attr]
        tab.fields["target_rps_list"].set("0.2,0.4")  # type: ignore[union-attr]
        tab.fields["sweep_repeats"].set("2")  # type: ignore[union-attr]
        commands = tab.sweep_commands()
        self.assertEqual(len(commands), 4)
        labels = [label for label, _, _ in commands]
        self.assertEqual(labels, [
            "rps=0.2 run=1",
            "rps=0.2 run=2",
            "rps=0.4 run=1",
            "rps=0.4 run=2",
        ])
        self.assertTrue(all("--target-rps" in command for _, command, _ in commands))
        self.assertTrue(all("--dry-run" in command for _, command, _ in commands))

    def test_qwen_minindn_button_runs_dry_run_without_blocking_gui(self) -> None:
        tab = self.app.qwen_minindn
        with tempfile.TemporaryDirectory() as tmp:
            tab.fields["use_sudo"].set(False)  # type: ignore[union-attr]
            tab.fields["dry_run"].set(True)  # type: ignore[union-attr]
            tab.fields["out_dir"].set(str(Path(tmp) / "qwen-dry-run"))  # type: ignore[union-attr]
            tab.fields["output_json"].set(str(Path(tmp) / "qwen-gui-summary.json"))  # type: ignore[union-attr]
            tab.run_experiment()
            self.assertTrue(wait_until(
                self.app,
                lambda: tab.status_var.get().startswith("completed rc=0"),
                timeout_s=4.0,
            ))
            self.assertIn("NDNSF_DI_NATIVE_TRACER_MININDN_DRY_RUN", tab.log_pane.get())
            self.assertEqual(str(tab.run_button["state"]), "normal")
            self.assertEqual(str(tab.sweep_button["state"]), "normal")
            self.assertEqual(str(tab.stop_button["state"]), "disabled")

    def test_qwen_minindn_sweep_button_runs_dry_run_sequence(self) -> None:
        tab = self.app.qwen_minindn
        with tempfile.TemporaryDirectory() as tmp:
            tab.fields["use_sudo"].set(False)  # type: ignore[union-attr]
            tab.fields["dry_run"].set(True)  # type: ignore[union-attr]
            tab.fields["out_dir"].set(str(Path(tmp) / "qwen-sweep"))  # type: ignore[union-attr]
            tab.fields["output_json"].set(str(Path(tmp) / "qwen-sweep-summary.json"))  # type: ignore[union-attr]
            tab.fields["target_rps_list"].set("0.2,0.4")  # type: ignore[union-attr]
            tab.run_sweep()
            self.assertTrue(wait_until(
                self.app,
                lambda: tab.status_var.get().startswith("completed rc=0"),
                timeout_s=8.0,
            ))
            log_text = tab.log_pane.get()
            self.assertIn("=== Qwen MiniNDN rps=0.2 run=1 ===", log_text)
            self.assertIn("=== Qwen MiniNDN rps=0.4 run=1 ===", log_text)
            self.assertIn("NDNSF_DI_NATIVE_TRACER_MININDN_DRY_RUN", log_text)
            self.assertEqual(str(tab.run_button["state"]), "normal")
            self.assertEqual(str(tab.sweep_button["state"]), "normal")

    def test_qwen_minindn_summary_writes_report_csv(self) -> None:
        tab = self.app.qwen_minindn
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run1 = root / "rps-0_2-run-1"
            run2 = root / "rps-0_4-run-1"
            run1.mkdir()
            run2.mkdir()
            template = {
                "status": "SUCCESS",
                "runnerMode": "qwen-onnx-native",
                "miniNDNRun": "started",
                "dependencyExecution": {
                    "status": "executed",
                    "roles": ["/LLM/Stage/0", "/LLM/Stage/1"],
                },
                "providerUtilization": {
                    "/provider/A": {
                        "estimatedUtilization": 0.25,
                        "busyHandlerMs": 10.0,
                    },
                    "/provider/B": {
                        "estimatedUtilization": 0.75,
                        "busyHandlerMs": 30.0,
                    },
                },
            }
            for path, target_rps, p50, p95 in (
                (run1, 0.2, 11.0, 22.0),
                (run2, 0.4, 33.0, 44.0),
            ):
                payload = dict(template)
                payload["userExecution"] = {
                    "targetRps": target_rps,
                    "requestCount": 2,
                    "successCount": 2,
                    "failureCount": 0,
                    "p50Ms": p50,
                    "p95Ms": p95,
                    "meanMs": p50,
                    "makespanMs": p95,
                    "throughputRps": 3.5,
                }
                (path / "summary.json").write_text(json.dumps(payload), encoding="utf-8")
            output_json = root / "gui-summary.json"
            tab.fields["output_json"].set(str(output_json))  # type: ignore[union-attr]
            tab._append_summary([
                {"label": "rps=0.2 run=1", "out": str(run1), "returncode": 0},
                {"label": "rps=0.4 run=1", "out": str(run2), "returncode": 0},
            ])
            aggregate = json.loads(output_json.read_text(encoding="utf-8"))
            csv_path = Path(aggregate["csv"])
            report_path = Path(aggregate["report"])
            plot_path = Path(aggregate["plot"])
            self.assertTrue(csv_path.exists())
            self.assertTrue(report_path.exists())
            self.assertTrue(plot_path.exists())
            with csv_path.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["label"], "rps=0.2 run=1")
            self.assertEqual(rows[0]["status"], "SUCCESS")
            self.assertEqual(rows[0]["successRate"], "1.0")
            self.assertEqual(rows[0]["p50Ms"], "11.0")
            self.assertEqual(rows[0]["providerCount"], "2")
            self.assertEqual(rows[0]["providerMeanUtilization"], "0.5")
            self.assertEqual(rows[0]["providerBusyHandlerMs"], "40.0")
            report_text = report_path.read_text(encoding="utf-8")
            self.assertIn("# Qwen MiniNDN Sweep Report", report_text)
            self.assertIn("![Qwen MiniNDN sweep plot](gui-summary.svg)", report_text)
            self.assertIn("Best p50: 11 ms (rps=0.2 run=1)", report_text)
            self.assertIn("Best throughput: 3.5 RPS", report_text)
            self.assertIn("Mean provider utilization across runs: 0.5", report_text)
            self.assertIn("| rps=0.4 run=1 | 0.4 | SUCCESS | 1.0 | 33.0 | 44.0 |", report_text)
            self.assertIn("No failed runs.", report_text)
            plot_text = plot_path.read_text(encoding="utf-8")
            self.assertIn("<svg", plot_text)
            self.assertIn("Qwen MiniNDN Sweep Metrics", plot_text)
            self.assertIn("Latency (ms)", plot_text)
            self.assertIn("Throughput (RPS)", plot_text)
            self.assertIn("Provider utilization", plot_text)
            ET.parse(plot_path)


if __name__ == "__main__":
    unittest.main()
