"""Process-level Python oracle for the native Spec175 terminal contract.

This test deliberately uses only the standard library.  It must be runnable
without importing the host ``ndnsf._ndnsf`` extension, because the host
interpreter is not the deployment ABI authority.  The integration binary is
the production path; its bounded Boost.Test messages expose the same token,
delta, and terminal fields asserted by the native test cases.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build/integration-tests"


def _run_native_case(test_name: str) -> str:
    if not BINARY.is_file():
        pytest.fail(f"native integration binary is unavailable: {BINARY}")
    environment = os.environ.copy()
    environment.setdefault(
        "NDN_LOG",
        "*=WARN:ndn_service_framework.TimelineTrace=WARN:"
        "ndnsf.di.RuntimeEvidence=WARN",
    )
    completed = subprocess.run(
        [
            str(BINARY),
            f"--run_test=Spec170NdnsfDiCoreFlow/{test_name}",
            "--log_level=test_suite",
            "--report_level=short",
        ],
        cwd=ROOT,
        env=environment,
        text=True,
        capture_output=True,
        timeout=180,
        check=False,
    )
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, output
    assert "test case" in output and "passed" in output, output
    return output


def test_i01_native_process_matches_python_terminal_oracle() -> None:
    output = _run_native_case("Spec175NativeTinyOnnxI01OneProvider")
    assert '"tokenIds":[4,5,6,7,8,9,10,2]' in output
    assert '"text":"token-4 token-5 token-6 token-7 token-8 token-9 token-10 token-2"' in output
    assert '"textDelta":"token-4"' in output
    assert '"finishHint":"EOS"' in output


def test_i03_native_four_provider_process_matches_python_terminal_oracle() -> None:
    output = _run_native_case(
        "Spec175NativeTinyOnnxI03FourProviderEpochCoordinator")
    assert "providerCompletions=4" in output
    assert "providerFailures=0" in output
    assert '"tokenIds":[4,5,6,7,8,9,10,2]' in output
    assert '"textDelta":"token-4"' in output
    assert '"finishHint":"EOS"' in output


def test_i16_native_four_provider_process_matches_unicode_stop_oracle() -> None:
    output = _run_native_case(
        "Spec175NativeTinyOnnxI16SeededUnicodeAndSplitStop")
    assert "providerCompletions=4" in output
    assert "providerFailures=0" in output
    assert '"tokenIds":[4,5,6]' in output
    assert '"textDelta":"你"' in output
    assert '"textDelta":"好"' in output
    assert '"textDelta":"🙂"' in output
    assert '"finishHint":"STOP_SEQUENCE"' in output
    assert '"finishReason":"stop_sequence"' in output
    assert '"text":"你好🙂"' in output
