from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/spec111/run_non_regression_campaign.py"


def load_campaign_module():
    spec = importlib.util.spec_from_file_location(
        "spec111_non_regression_campaign", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load Spec 111 campaign runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RoleImportPreflightTest(unittest.TestCase):
    def test_commands_cover_real_controller_provider_and_user_entrypoints(self):
        module = load_campaign_module()
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        source_root = Path(temporary.name)
        canonical = source_root / (
            "NDNSF-DistributedInference/ndnsf_distributed_inference/"
            "app_sdk/controller.py")
        canonical.parent.mkdir(parents=True)
        canonical.touch()

        commands = module.role_import_commands(source_root)

        self.assertEqual([role for role, _ in commands], [
            "controller", "provider", "user",
        ])
        rendered = {role: " ".join(command) for role, command in commands}
        self.assertIn(
            "ndnsf_distributed_inference.app_sdk.controller import APPController",
            rendered["controller"],
        )
        self.assertIn("llm_pipeline/provider.py", rendered["provider"])
        self.assertIn("llm_pipeline/user.py", rendered["user"])
        self.assertIn("runpy.run_path", rendered["user"])
        self.assertIn("APPClient", rendered["user"])
        for command in rendered.values():
            self.assertIn("PYTHONNOUSERSITE=1", command)
            self.assertIn(
                f"{source_root}/NDNSF-DistributedRepo/pythonWrapper", command)
            self.assertIn("PYTHONDONTWRITEBYTECODE=1", command)

    def test_pre_separation_source_uses_its_real_root_controller_export(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            commands = dict(module.role_import_commands(Path(directory)))
        rendered = " ".join(commands["controller"])
        self.assertIn(
            "from ndnsf_distributed_inference import APPController", rendered)
        self.assertNotIn("app_sdk.controller", rendered)

    def test_failure_is_reported_before_any_campaign_output_is_created(self):
        module = load_campaign_module()
        calls = []

        def fail_controller(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(
                command, 1, stdout="", stderr="ImportError: APPController")

        output = Path("/tmp/spec111-role-preflight-must-not-exist")
        self.assertFalse(output.exists())
        with self.assertRaisesRegex(
                RuntimeError,
                "SPEC111_ROLE_IMPORT_PREFLIGHT_FAILED:controller"):
            module.run_role_import_preflight(
                Path("/candidate"), runner=fail_controller)
        self.assertEqual(len(calls), 1)
        self.assertFalse(output.exists())

    def test_main_runs_role_preflight_before_creating_campaign_root(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            campaign_root = Path(directory) / "campaign"
            with mock.patch.object(
                    module,
                    "run_role_import_preflight",
                    side_effect=RuntimeError("SPEC111_ROLE_IMPORT_BLOCKED")), \
                    mock.patch.object(sys, "argv", [
                        str(SCRIPT), "--campaign-root", str(campaign_root),
                    ]):
                with self.assertRaisesRegex(
                        RuntimeError, "SPEC111_ROLE_IMPORT_BLOCKED"):
                    module.main()
            self.assertFalse(campaign_root.exists())

    def test_runtime_readiness_matches_the_complete_formal_workload(self):
        module = load_campaign_module()
        command = module.readiness_command_for(
            Path("/candidate"), "treatment", Path("/readiness/treatment"))
        rendered = " ".join(command)
        self.assertIn("NDNSF_DI_LlmPipeline_Minindn.py", rendered)
        self.assertIn("--warmup-requests 10", rendered)
        self.assertIn("--measured-requests 60", rendered)
        self.assertIn("--measured-duration-s 60", rendered)
        self.assertIn("--campaign-id spec111-readiness-treatment", rendered)

    def test_baseline_patch_is_limited_to_measurement_protocol_compatibility(self):
        module = load_campaign_module()
        self.assertEqual(module.BASELINE_MEASUREMENT_PATCH_PATHS, (
            "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py",
            "pythonWrapper/src/ndnsf/_ndnsf.cpp",
            "ndn-service-framework/NDNSFMessages.cpp",
            "ndn-service-framework/NDNSFMessages.hpp",
            "ndn-service-framework/ServiceUser.cpp",
            "ndn-service-framework/ServiceUser.hpp",
            "ndn-service-framework/ServiceProvider.cpp",
            "ndn-service-framework/ServiceProvider.hpp",
        ))

    def test_runtime_readiness_failure_prevents_formal_campaign_root(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            campaign_root = Path(directory) / "campaign"
            with mock.patch.object(
                    module, "run_role_import_preflight",
                    return_value={"status": "PASS"}), \
                    mock.patch.object(
                        module, "run_runtime_readiness_preflight",
                        side_effect=RuntimeError("SPEC111_RUNTIME_READINESS_FAILED")), \
                    mock.patch.object(sys, "argv", [
                        str(SCRIPT), "--campaign-root", str(campaign_root),
                    ]):
                with self.assertRaisesRegex(
                        RuntimeError, "SPEC111_RUNTIME_READINESS_FAILED"):
                    module.main()
            self.assertFalse(campaign_root.exists())

    def test_runtime_readiness_requires_warmup_and_measured_success(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "readiness" / "treatment"

            def run_probe(command, **kwargs):
                output.mkdir(parents=True, exist_ok=True)
                rows = ["phase,index,distributed_ms,status,error"]
                rows.extend(
                    f"warmup,{index},12.0,ok," for index in range(10))
                rows.extend(
                    f"measured,{index + 10},10.0,ok," for index in range(60))
                (output / "llm-pipeline-user-measured.csv").write_text(
                    "\n".join(rows) + "\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            result = module.run_runtime_readiness_preflight(
                Path("/candidate"),
                "treatment",
                output,
                runner=run_probe,
                cleanup_runner=lambda command, **kwargs: subprocess.CompletedProcess(
                    command, 0, stdout="", stderr=""),
                preflight_fn=lambda path: {"status": "PASS"},
                active_fn=lambda: [],
            )

            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["requestCount"], 70)
            self.assertTrue((output / "readiness-result.json").is_file())

    def test_runtime_readiness_rejects_provider_protocol_failures(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "readiness" / "treatment"

            def run_probe(command, **kwargs):
                output.mkdir(parents=True, exist_ok=True)
                rows = ["phase,index,distributed_ms,status,error"]
                rows.extend(
                    f"warmup,{index},12.0,ok," for index in range(10))
                rows.extend(
                    f"measured,{index + 10},10.0,ok," for index in range(60))
                (output / "llm-pipeline-user-measured.csv").write_text(
                    "\n".join(rows) + "\n", encoding="utf-8")
                (output / "stage2-provider.log").write_text(
                    "Validator/policy did not invoke success or failure callback\n",
                    encoding="utf-8",
                )
                return subprocess.CompletedProcess(command, 0)

            with self.assertRaisesRegex(
                    RuntimeError, "SPEC111_RUNTIME_READINESS_FATAL_LOG"):
                module.run_runtime_readiness_preflight(
                    Path("/candidate"),
                    "treatment",
                    output,
                    runner=run_probe,
                    cleanup_runner=lambda command, **kwargs: subprocess.CompletedProcess(
                        command, 0, stdout="", stderr=""),
                    preflight_fn=lambda path: {"status": "PASS"},
                    active_fn=lambda: [],
                )

    def test_formal_campaign_stops_immediately_after_failed_cell(self):
        module = load_campaign_module()
        failed = {
            "cellId": "pair-01-treatment",
            "status": "FAILED_OBSERVED",
            "completedRequests": 0,
        }

        with self.assertRaisesRegex(
                RuntimeError,
                "SPEC111_FORMAL_CELL_FAILED:pair-01-treatment"):
            module.require_formal_cell_passed(failed)

        passed = {"cellId": "pair-01-baseline", "status": "PASS"}
        self.assertIs(module.require_formal_cell_passed(passed), passed)

    def test_diagnostic_continuation_preserves_failure_and_moves_forward(self):
        module = load_campaign_module()
        failed = {
            "cellId": "pair-03-treatment",
            "status": "FAILED_OBSERVED",
            "completedRequests": 51,
        }

        self.assertIs(
            module.accept_terminal_cell(
                failed, continue_diagnostic_after_failure=True),
            failed,
        )
        with self.assertRaisesRegex(
                RuntimeError,
                "SPEC111_FORMAL_CELL_FAILED:pair-03-treatment"):
            module.accept_terminal_cell(
                failed, continue_diagnostic_after_failure=False)

    def test_existing_passing_readiness_is_reused_without_rerun(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "readiness" / "treatment"
            output.mkdir(parents=True)
            expected = {
                "schema": "ndnsf-di-spec111-runtime-readiness-v1",
                "variant": "treatment",
                "status": "PASS",
                "cleanup": {"survivors": []},
            }
            module.write_json(output / "readiness-result.json", expected)

            with mock.patch.object(
                    module, "run_runtime_readiness_preflight") as rerun:
                actual = module.ensure_runtime_readiness_preflight(
                    Path("/candidate"), "treatment", output)

            self.assertEqual(actual, expected)
            rerun.assert_not_called()

    def test_existing_failed_readiness_cannot_be_reused(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "readiness" / "treatment"
            output.mkdir(parents=True)
            module.write_json(output / "readiness-result.json", {
                "variant": "treatment",
                "status": "FAIL",
                "cleanup": {"survivors": []},
            })

            with self.assertRaisesRegex(
                    RuntimeError, "SPEC111_EXISTING_READINESS_NOT_PASSING"):
                module.ensure_runtime_readiness_preflight(
                    Path("/candidate"), "treatment", output)

    def test_formal_cell_rejects_fatal_logs_even_with_sixty_successes(self):
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "cell"
            output.mkdir()
            rows = ["phase,index,distributed_ms,status,error"]
            rows.extend(
                f"measured,{index},10.0,ok," for index in range(60))
            (output / "llm-pipeline-user-measured.csv").write_text(
                "\n".join(rows) + "\n", encoding="utf-8")
            (output / "stage2-provider.log").write_text(
                "Collaboration handler failed: protocol error\n",
                encoding="utf-8",
            )

            result = module.parse_cell(output, 0, 1000, 60.0)

            self.assertEqual(result["status"], "FAILED_OBSERVED")
            self.assertEqual(len(result["fatalLogFindings"]), 1)

    def test_success_records_each_role_without_starting_network_runtime(self):
        module = load_campaign_module()

        def pass_role(command, **kwargs):
            return subprocess.CompletedProcess(
                command, 0, stdout="role import ok\n", stderr="")

        result = module.run_role_import_preflight(
            Path("/candidate"), runner=pass_role)

        self.assertEqual(result["status"], "PASS")
        self.assertEqual([item["role"] for item in result["roles"]], [
            "controller", "provider", "user",
        ])
        self.assertTrue(all(item["returncode"] == 0 for item in result["roles"]))
        self.assertEqual(result["networkRuntimeStarts"], 0)


if __name__ == "__main__":
    unittest.main()
