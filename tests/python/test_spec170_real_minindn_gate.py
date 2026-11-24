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
NATIVE_TRACER_RUNNER = ROOT / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
SPEC170_EVIDENCE = ROOT / "specs/170-reusable-layer-artifacts/evidence"


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


    def _run_real_native_tracer(
            self, assignment: str, *, retain_dir: Path | None = None) -> dict:
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
        summary: dict = {}
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
            if retain_dir is not None and workspace.exists():
                retain_dir.mkdir(parents=True, exist_ok=True)
                for relative in (
                        Path("summary.json"),
                        Path("assignment.csv"),
                        Path("policy-bundle/native-execution-plan.json"),
                        Path("policy-bundle/service-manifest.json")):
                    source = workspace / "run" / relative
                    if source.is_file():
                        destination = retain_dir / relative
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(source, destination)
                source_logs = workspace / "run" / "logs"
                if source_logs.is_dir():
                    destination_logs = retain_dir / "logs"
                    destination_logs.mkdir(parents=True, exist_ok=True)
                    for source in source_logs.glob("*.log"):
                        shutil.copy2(source, destination_logs / source.name)
            shutil.rmtree(workspace, ignore_errors=True)
        return summary

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


if __name__ == "__main__":
    unittest.main()
