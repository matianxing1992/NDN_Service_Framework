#!/usr/bin/env python3
"""Deployment-faithful MiniNDN smoke contract for Spec 170 T006.

The test is deliberately opt-in for normal unit runs because it starts NFD and
Mininet.  The gate command sets ``SPEC170_RUN_REAL_MININDN=1`` and therefore
verifies the complete real topology rather than a fixture-only harness.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"


class RealMiniNdnGateTest(unittest.TestCase):
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
