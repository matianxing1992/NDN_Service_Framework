#!/usr/bin/env python3
"""Deployment-faithful MiniNDN smoke contract for Spec 170 T006.

The test is deliberately opt-in for normal unit runs because it starts NFD and
Mininet.  The gate command sets ``SPEC170_RUN_REAL_MININDN=1`` and therefore
verifies the complete real topology rather than a fixture-only harness.
"""

from __future__ import annotations

import csv
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from Experiments.spec170_dependency_evidence import (
    collect_dependency_execution_evidence,
)


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
NATIVE_TRACER_RUNNER = ROOT / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
SPEC170_EVIDENCE = ROOT / "specs/170-reusable-layer-artifacts/evidence"
DEFAULT_APPTAINER = "/opt/apptainer/1.5.3/bin/apptainer"


def _configured_ort_library_path() -> str:
    """Resolve the ORT library used by the current native build.

    The native executable is linked against the configured ORT ABI.  Relying
    on the host default can therefore start the process successfully but fail
    later with a versioned-symbol error.  Prefer an explicit gate override,
    then recover the ``-L`` path from the build log and require the actual
    shared object to exist.
    """
    candidates = []
    explicit = os.environ.get("SPEC170_ORT_LIBRARY_PATH", "").strip()
    if explicit:
        candidates.append(explicit)
    config_log = ROOT / "build/config.log"
    if config_log.exists():
        for line in config_log.read_text(errors="replace").splitlines():
            if "onnxruntime" not in line.lower():
                continue
            for match in re.finditer(r"-L([^\s'\"]+)", line):
                candidates.append(match.group(1))
    for candidate in candidates:
        path = Path(candidate).expanduser()
        if any(path.glob("libonnxruntime.so*")):
            return str(path)
    return ""


class RealMiniNdnGateTest(unittest.TestCase):
    @staticmethod
    def _exact_sif_apptainer() -> str:
        """Return the explicitly selected Apptainer binary for SIF gates.

        A bare ``apptainer`` lookup is unsafe on this host: ``/usr/local/bin``
        currently exposes 1.3.4 while the candidate was built and qualified
        with 1.5.3.  Exact-SIF tests therefore require an explicit path, with
        an environment override for a target installation.
        """
        configured = os.environ.get(
            "SPEC170_APPTAINER", DEFAULT_APPTAINER).strip()
        path = Path(configured).expanduser()
        if not path.is_file() or not os.access(path, os.X_OK):
            raise AssertionError(
                "SPEC170_APPTAINER must name an executable Apptainer binary: "
                f"{path}")
        return str(path)

    def _assert_exact_sif_apptainer_version(self) -> str:
        apptainer = self._exact_sif_apptainer()
        expected = os.environ.get(
            "SPEC170_APPTAINER_VERSION", "1.5.3").strip()
        result = subprocess.run(
            [apptainer, "version"], cwd=ROOT, text=True,
            capture_output=True, timeout=15, check=False,
        )
        self.assertEqual(result.returncode, 0,
                         result.stdout + "\n" + result.stderr)
        reported = (result.stdout + "\n" + result.stderr).strip()
        self.assertRegex(
            reported, rf"(?m)(^|\s){re.escape(expected)}(?:\s|$)",
            f"expected Apptainer {expected} from {apptainer}, got: {reported}",
        )
        return apptainer

    def test_runner_has_real_topology_contract(self) -> None:
        help_result = subprocess.run(
            ["python3", str(RUNNER), "--help"],
            cwd=ROOT, text=True, capture_output=True, check=True,
        )
        self.assertIn("--runtime", help_result.stdout)
        self.assertIn("--test-only-allow-ephemeral-app-state", help_result.stdout)

    def test_runner_sanitizes_host_process_environment(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('os.environ.pop("NDN_LOG", None)', source)
        self.assertIn('os.environ.setdefault("SHELL", "/bin/bash")', source)

    def test_spec170_workloads_exec_provider_child_for_cleanup(self) -> None:
        """Provider PIDs must be directly killable during workload cleanup."""
        for workload_name in (
                "spec170-d0-current-sif-workload.sh",
                "spec170-d1-current-sif-workload.sh"):
            source = (SPEC170_EVIDENCE / workload_name).read_text(
                encoding="utf-8")
            self.assertIn('cd "$BUNDLE"', source)
            self.assertIn('exec env "$provider_pib" "$provider_tpm"', source)
            if workload_name.startswith("spec170-d0-"):
                self.assertIn(
                    '  cd "$BUNDLE"\n  exec env "$provider_pib"', source)
                self.assertNotIn(
                    '  (\n    cd "$BUNDLE"\n    exec env', source)

    @unittest.skipUnless(
        os.environ.get("SPEC170_EXACT_SIF"),
        "set SPEC170_EXACT_SIF to run the candidate-SIF CLI closure gate",
    )
    def test_exact_sif_uses_recorded_apptainer(self) -> None:
        self._assert_exact_sif_apptainer_version()

    @unittest.skipUnless(
        os.environ.get("SPEC170_EXACT_SIF"),
        "set SPEC170_EXACT_SIF to run the candidate-SIF CLI closure gate",
    )
    def test_exact_sif_provider_cli_matches_workload_contract(self) -> None:
        """Reject a SIF whose Provider predates the workload command line."""
        sif = Path(os.environ["SPEC170_EXACT_SIF"]).expanduser()
        self.assertTrue(sif.is_file(), f"missing exact SIF: {sif}")
        apptainer = self._assert_exact_sif_apptainer_version()
        result = subprocess.run(
            [apptainer, "exec", "--cleanenv", str(sif),
             "/opt/ndnsf-di/current/bin/di-native-provider"],
            cwd=ROOT, text=True, capture_output=True, timeout=30,
        )
        usage = result.stdout + "\n" + result.stderr
        # No --plan is intentional: the executable should print its complete
        # usage contract without starting a network process.
        self.assertNotEqual(result.returncode, 0, usage)
        self.assertIn("--bootstrap-token", usage)
        self.assertIn("--execution-policy", usage)

    def _run_exact_sif_dependency_workload(
            self, workload_name: str, evidence_name: str,
            expected_runner: str = "onnxruntime-cpu") -> None:
        """Run the real Controller/User/Provider chain inside the exact SIF."""
        sif = Path(os.environ["SPEC170_EXACT_SIF"]).expanduser().resolve()
        source_bundle = Path(
            os.environ["SPEC170_EXACT_SIF_BUNDLE"]).expanduser().resolve()
        self.assertTrue(sif.is_file(), f"missing exact SIF: {sif}")
        self.assertTrue(source_bundle.is_dir(),
                        f"missing exact SIF bundle: {source_bundle}")
        apptainer = self._assert_exact_sif_apptainer_version()
        workload = SPEC170_EVIDENCE / workload_name
        self.assertTrue(workload.is_file(), f"missing workload: {workload}")

        with tempfile.TemporaryDirectory(
                prefix="spec170-exact-sif-network-") as directory:
            workspace = Path(directory)
            bundle = workspace / "bundle"
            scratch = workspace / "scratch"
            evidence = workspace / "evidence"
            shutil.copytree(source_bundle, bundle)
            shutil.copy2(workload, bundle / workload_name)
            scratch.mkdir()
            evidence.mkdir()
            shell_command = f"exec /bin/bash /bundle/{workload_name}"
            if workload_name.startswith("spec170-d1-"):
                shell_command = (
                    f"SPEC170_EXPECTED_RUNNER_KIND={expected_runner} "
                    + shell_command)
            result = subprocess.run(
                [
                    "timeout", "180s", apptainer, "exec", "--cleanenv",
                    "--bind",
                    f"{bundle}:/bundle:ro,{scratch}:/scratch,{evidence}:/evidence",
                    str(sif), "/bin/bash", "-c", shell_command,
                ],
                cwd=ROOT, text=True, capture_output=True, timeout=190,
            )
            evidence_dir = evidence / "spec170" / evidence_name
            logs_dir = evidence_dir / "log"
            log_tail = "\n".join(
                f"--- {path.name}\n" + "\n".join(
                    path.read_text(errors="replace").splitlines()[-30:])
                for path in sorted(logs_dir.glob("*.log"))
            )
            combined = result.stdout + "\n" + result.stderr + "\n" + log_tail
            self.assertEqual(result.returncode, 0, combined[-16000:])

            # The workload is responsible for terminating every Provider
            # before Apptainer exits.  Scope the census to this workload's
            # native plan so an unrelated diagnostic process cannot make the
            # gate nondeterministic.
            process_census = subprocess.run(
                ["pgrep", "-af", "/opt/ndnsf-di/current/bin/di-native-provider"],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            leaked = [
                line for line in process_census.stdout.splitlines()
                if "--plan /bundle/native-execution-plan.json" in line
            ]
            self.assertEqual([], leaked,
                             "Provider processes survived workload cleanup:\n" +
                             "\n".join(leaked))

            terminal = (
                evidence_dir / "status" / "terminal-check.txt"
            ).read_text(encoding="utf-8")
            self.assertIn("selection_ok=1", terminal)
            self.assertIn("dependency_ok=1", terminal)
            self.assertIn("response_ok=1", terminal)

            dependency = collect_dependency_execution_evidence(
                sorted(logs_dir.glob("*.log")),
                bundle / "native-execution-plan.json")
            if dependency["status"] != "executed":
                failure_dir = Path(tempfile.mkdtemp(
                    prefix="spec170-exact-sif-dependency-failure-"))
                (failure_dir / "dependency.json").write_text(
                    json.dumps(dependency, indent=2, sort_keys=True),
                    encoding="utf-8")
                (failure_dir / "log-tail.txt").write_text(
                    log_tail, encoding="utf-8")
                full_logs_dir = failure_dir / "logs"
                full_logs_dir.mkdir()
                for source_log in sorted(logs_dir.glob("*.log")):
                    (full_logs_dir / source_log.name).write_text(
                        source_log.read_text(errors="replace"),
                        encoding="utf-8")
                raise AssertionError(
                    json.dumps(dependency, sort_keys=True) +
                    f"\nretained diagnostic: {failure_dir}\n" +
                    log_tail[-12000:])
            self.assertEqual(dependency["completeEdgeCount"], 4)
            self.assertEqual(dependency["expectedEdgeCount"], 4)
            self.assertEqual(dependency["missingPublications"], [])
            self.assertEqual(dependency["missingFetches"], [])
            all_provider_logs = "\n".join(
                path.read_text(errors="replace")
                for path in sorted(logs_dir.glob("*.log"))
                if path.name not in {"controller.log", "nfd.log", "user.log"}
            )
            self.assertIn(expected_runner, all_provider_logs)

    @unittest.skipUnless(
        os.environ.get("SPEC170_EXACT_SIF") and
        os.environ.get("SPEC170_EXACT_SIF_BUNDLE"),
        "set SPEC170_EXACT_SIF and SPEC170_EXACT_SIF_BUNDLE for the exact-SIF network gate",
    )
    def test_exact_sif_d0_four_provider_dependency_chain(self) -> None:
        self._run_exact_sif_dependency_workload(
            "spec170-d0-current-sif-workload.sh", "d0-current-incontainer")

    @unittest.skipUnless(
        os.environ.get("SPEC170_EXACT_SIF") and
        os.environ.get("SPEC170_EXACT_SIF_BUNDLE"),
        "set SPEC170_EXACT_SIF and SPEC170_EXACT_SIF_BUNDLE for the exact-SIF network gate",
    )
    def test_exact_sif_d1_single_provider_dependency_chain(self) -> None:
        self._run_exact_sif_dependency_workload(
            "spec170-d1-current-sif-workload.sh", "d1-current-incontainer")

    def _run_real_native_tracer(self, assignment: str) -> None:
        """Run the production NativeTracer path, not a fixture-only harness.

        This gate intentionally uses the real ORT CPU runner.  The
        deterministic runner is retained as a negative/control test elsewhere;
        it must not be used as evidence that runtime assignment validation and
        post-Selection execution succeeded.
        """
        ort_library = _configured_ort_library_path()
        self.assertTrue(
            ort_library,
            "set SPEC170_ORT_LIBRARY_PATH or configure an ORT -L path before "
            "running the real NativeTracer gate",
        )
        workspace = Path(tempfile.mkdtemp(prefix="spec170-native-minindn-"))
        output_dir = workspace / "run"
        inherited_ld = os.environ.get("LD_LIBRARY_PATH", "")
        ld_library_path = ":".join(
            value for value in (ort_library, inherited_ld) if value)
        is_hybrid = assignment.startswith("hybrid-")
        command = [
            "sudo", "-n", "timeout", "150s" if is_hybrid else "90s", "env",
            f"LD_LIBRARY_PATH={ld_library_path}",
            "python3", str(NATIVE_TRACER_RUNNER),
            "--full-network", "--core-trace",
            "--assignment", assignment,
            "--requests", "1", "--concurrency", "1",
            "--provider-check-timeout", "30",
            "--skip-provider-pair-telemetry-probe",
        ]
        if not is_hybrid:
            command.extend([
                "--overload-fast-fail-timeout-ms", "15000",
            ])
        command.extend(["--out", str(output_dir)])
        try:
            result = subprocess.run(
                command, cwd=ROOT, text=True, capture_output=True,
                timeout=165 if is_hybrid else 100,
            )
            logs = sorted((output_dir / "logs").glob("*.log"))
            log_tail = "\n".join(
                f"--- {path.name}\n" + "".join(
                    path.read_text(errors="replace").splitlines(True)[-20:])
                for path in logs
            )
            combined = (result.stdout + "\n" + result.stderr + "\n" +
                        log_tail)
            self.assertEqual(result.returncode, 0, combined[-12000:])
            summary_path = output_dir / "summary.json"
            self.assertTrue(summary_path.exists(), combined[-12000:])
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary.get("status"), "SUCCESS")
            self.assertEqual(summary.get("assignmentResolved"), assignment)
            self.assertEqual(summary.get("runnerClassification"),
                             "onnxruntime-cpu")
            self.assertEqual(summary.get("runnerMode"), "onnxruntime-cpu")
            if is_hybrid:
                numerical_oracle = summary.get("numericalOracle", {})
                self.assertEqual(numerical_oracle.get("status"), "PASS")
                self.assertEqual(numerical_oracle.get("checkedRequestCount"), 1)
                self.assertLessEqual(
                    float(numerical_oracle.get("maxAbsoluteError", 1.0)),
                    float(numerical_oracle.get("absoluteTolerance", 0.0)),
                )
            user_execution = summary.get("userExecution", {})
            self.assertEqual(user_execution.get("successCount"), 1)
            self.assertEqual(user_execution.get("failureCount"), 0)
            dependency_execution = summary.get("dependencyExecution", {})
            self.assertEqual(dependency_execution.get("status"), "executed")
            expected_scopes = {
                "default": {
                    "backbone-to-head0", "backbone-to-head1",
                    "head0-to-merge", "head1-to-merge",
                },
                "single-provider": {
                    "backbone-to-head0", "backbone-to-head1",
                    "head0-to-merge", "head1-to-merge",
                },
                "hybrid-121": {"boundary-0", "boundary-1"},
                "hybrid-212": {"boundary-0", "boundary-1", "boundary-2"},
            }[assignment]
            plan = json.loads(
                (output_dir / "policy-bundle/native-execution-plan.json").read_text(
                    encoding="utf-8"))
            service_plan = next(
                item for item in plan["services"]
                if item["service"] == "/Inference/NativeTracer")
            expected_edge_count = sum(
                len(dependency.get("producers", [])) *
                len(dependency.get("consumers", []))
                for dependency in service_plan.get("dependencies", []))
            self.assertEqual(
                dependency_execution.get("expectedEdgeCount"),
                expected_edge_count)
            self.assertEqual(
                dependency_execution.get("completeEdgeCount"),
                expected_edge_count)
            self.assertEqual(dependency_execution.get("missingPublications"), [])
            self.assertEqual(dependency_execution.get("missingFetches"), [])
            self.assertEqual(dependency_execution.get("nameMismatches"), [])
            self.assertEqual(dependency_execution.get("emptyPayloads"), [])
            self.assertEqual(
                {
                    item.get("transportScope", item["scope"])
                    for item in dependency_execution.get("edges", [])
                },
                expected_scopes,
            )

            assignment_rows = list(csv.DictReader(
                (output_dir / "assignment.csv").open(encoding="utf-8")))
            expected_roles = {
                "default": {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"},
                "single-provider": {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"},
                "hybrid-121": {"S0R0", "S1R0", "S1R1", "S2R0"},
                "hybrid-212": {"S0R0", "S0R1", "S1R0", "S2R0", "S2R1"},
            }[assignment]
            self.assertEqual(
                {row["role"] for row in assignment_rows}, expected_roles)
            self.assertNotIn("--tracer-deterministic-runner", result.stdout)
            user_log = (output_dir / "logs/user-driver.log").read_text(
                encoding="utf-8", errors="replace")
            self.assertGreaterEqual(
                user_log.count("ACK_MATCHED_PENDING_CALL"), 1)
            self.assertEqual(
                user_log.count("NDNSF_COLLAB_ASSIGNMENT_SELECTED"),
                len(expected_roles))
            self.assertGreaterEqual(user_log.count("SELECTION_PUBLISHED"), 1)
            self.assertIn("RESPONSE_RECEIVED", user_log)

            provider_logs = sorted((output_dir / "logs").glob(
                "provider-serve-*.log"))
            self.assertTrue(provider_logs)
            # The multi-Provider and hybrid paths launch one process per
            # complete role (121 -> 4, 212 -> 5).  D1 is the explicit
            # single-Provider baseline and intentionally hosts the pipeline
            # roles in one process.
            expected_provider_count = (
                1 if assignment == "single-provider" else len(expected_roles))
            self.assertEqual(len(provider_logs), expected_provider_count)
            for provider_log in provider_logs:
                text = provider_log.read_text(encoding="utf-8",
                                              errors="replace")
                self.assertIn("REQUEST_RECEIVED", text)
                self.assertIn("SELECTION_RECEIVED", text)
                self.assertIn("PROVIDER_EXECUTE_START", text)
            self.assertIn("RESPONSE_PUBLISHED", "\n".join(
                path.read_text(encoding="utf-8", errors="replace")
                for path in provider_logs))
        finally:
            # MiniNDN is launched through sudo and may leave root-owned files.
            # Reclaim only this test's private directory.
            subprocess.run([
                "sudo", "-n", "chown", "-R",
                f"{os.getuid()}:{os.getgid()}", str(workspace),
            ], check=False, capture_output=True)
            shutil.rmtree(workspace, ignore_errors=True)

    @unittest.skipUnless(
        os.environ.get("SPEC170_RUN_REAL_NATIVE_MININDN") == "1",
        "set SPEC170_RUN_REAL_NATIVE_MININDN=1 to execute the real NativeTracer gate",
    )
    def test_real_native_tracer_d0_four_provider_post_selection_path(self) -> None:
        self._run_real_native_tracer("default")

    @unittest.skipUnless(
        os.environ.get("SPEC170_RUN_REAL_NATIVE_MININDN") == "1",
        "set SPEC170_RUN_REAL_NATIVE_MININDN=1 to execute the real NativeTracer gate",
    )
    def test_real_native_tracer_d1_single_provider_post_selection_path(self) -> None:
        self._run_real_native_tracer("single-provider")

    @unittest.skipUnless(
        os.environ.get("SPEC170_RUN_REAL_NATIVE_MININDN") == "1",
        "set SPEC170_RUN_REAL_NATIVE_MININDN=1 to execute the real NativeTracer gate",
    )
    def test_real_native_tracer_hybrid_121_post_selection_path(self) -> None:
        self._run_real_native_tracer("hybrid-121")

    @unittest.skipUnless(
        os.environ.get("SPEC170_RUN_REAL_NATIVE_MININDN") == "1",
        "set SPEC170_RUN_REAL_NATIVE_MININDN=1 to execute the real NativeTracer gate",
    )
    def test_real_native_tracer_hybrid_212_post_selection_path(self) -> None:
        self._run_real_native_tracer("hybrid-212")

    @unittest.skipUnless(
        os.environ.get("SPEC170_RUN_REAL_MININDN") == "1",
        "set SPEC170_RUN_REAL_MININDN=1 to execute the real MiniNDN gate",
    )
    def test_real_three_provider_request_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spec170-minindn-test-") as temp:
            output_dir = Path(temp)
            command = [
                "sudo", "-n", "timeout", "90s", "python3", str(RUNNER),
                "--runtime", "fake", "--static-routing-only",
                "--nlsr-wait-s", "0", "--provider-start-timeout-s", "10",
                "--ack-timeout-ms", "500", "--timeout-ms", "15000",
                "--warmup-requests", "0", "--measured-requests", "1",
                "--max-new-tokens", "2",
                "--test-only-allow-ephemeral-app-state",
                "--output-dir", str(output_dir), "--prompt", "spec170-smoke",
            ]
            result = subprocess.run(
                command, cwd=ROOT, text=True, capture_output=True, timeout=100,
            )
            log_text = "\n".join(
                path.read_text(encoding="utf-8", errors="replace")
                for path in output_dir.glob("*.log")
            )
            combined = result.stdout + "\n" + result.stderr + "\n" + log_text
            self.assertEqual(result.returncode, 0, combined[-6000:])
            self.assertIn("LLM_PIPELINE_MININDN_OK", combined)
            # The current real smoke uses the generic collaboration facade;
            # its durable equivalents are assignment selection and provider
            # projection markers (the automatic-planner markers are emitted
            # by the app-SDK request-first path).
            self.assertIn("NDNSF_COLLAB_ASSIGNMENT_SELECTED", combined)
            self.assertIn("NDNSF_SELECTION_PROVIDER_PROJECTION", combined)
            self.assertIn("LLM_PIPELINE_USER_RESPONSE", combined)
            self.assertTrue((output_dir / "llm-pipeline-user.log").exists())

    @unittest.skipUnless(
        os.environ.get("SPEC170_RUN_REAL_QWEN_MULTI") == "1",
        "set SPEC170_RUN_REAL_QWEN_MULTI=1 for the cached Qwen multi-request gate",
    )
    def test_real_cached_qwen_multi_request_reuses_content_store(self) -> None:
        workspace = Path(tempfile.mkdtemp(prefix="spec170-qwen-multi-"))
        try:
            output_dir = workspace / "run"
            content_store = workspace / "content-store"
            command = [
                "sudo", "-n", "timeout", "240s", "env",
                "HF_HUB_OFFLINE=1", "TRANSFORMERS_OFFLINE=1", "python3",
                str(RUNNER), "--runtime", "qwen-transformers",
                "--qwen-model", "Qwen/Qwen2.5-0.5B-Instruct",
                "--qwen-revision", "main", "--qwen-dtype", "float32",
                "--qwen-content-store", str(content_store),
                "--static-routing-only", "--nlsr-wait-s", "0",
                "--provider-start-timeout-s", "45", "--ack-timeout-ms", "1500",
                "--timeout-ms", "120000", "--warmup-requests", "1",
                "--measured-requests", "3", "--max-new-tokens", "2",
                "--test-only-allow-ephemeral-app-state", "--output-dir",
                str(output_dir), "--prompt", "What is NDN?",
            ]
            result = subprocess.run(
                command, cwd=ROOT, text=True, capture_output=True, timeout=250,
            )
            combined = result.stdout + "\n" + result.stderr
            self.assertEqual(result.returncode, 0, combined[-8000:])
            self.assertIn("LLM_PIPELINE_MININDN_OK", combined)
            self.assertIn("LLM_PIPELINE_USER_SUMMARY count=3", combined)
            stage_root = output_dir / "qwen-transformers-stage-artifacts"
            self.assertTrue(stage_root.is_dir())
            self.assertEqual(
                sorted(path.name for path in stage_root.iterdir()),
                [
                    "stage-0-qwen-transformers.pt",
                    "stage-1-qwen-transformers.pt",
                    "stage-2-qwen-transformers.pt",
                ],
            )
            self.assertTrue(all(path.is_symlink() for path in stage_root.iterdir()))
            self.assertEqual(len(tuple((content_store / "sha256").glob("*.pt"))), 3)
        finally:
            # MiniNDN is launched through sudo and may leave root-owned socket
            # files. Reclaim only this test's private temporary directory so
            # the host does not accumulate failed-gate artifacts.
            subprocess.run([
                "sudo", "-n", "chown", "-R",
                f"{os.getuid()}:{os.getgid()}", str(workspace),
            ], check=False, capture_output=True)
            shutil.rmtree(workspace, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
