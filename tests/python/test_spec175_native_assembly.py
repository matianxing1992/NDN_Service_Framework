"""Fresh-process oracle for the Spec175 post-Selection native assembly path."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build/integration-tests"


@pytest.mark.parametrize(
    "case_name, provider_count",
    (
        ("RegisteredOneProviderAssemblyLoadsOrt", 1),
        ("RegisteredTwoProviderAssemblyLoadsOrt", 2),
        ("RegisteredFourProviderAssemblyLoadsOrt", 4),
    ),
)
def test_registered_provider_assembly_is_fresh_and_ort_loaded(
    case_name: str, provider_count: int,
) -> None:
    """Each provider-count case must run in a new process, not reuse state."""
    if not BINARY.is_file():
        pytest.fail(f"native integration binary is unavailable: {BINARY}")
    environment = os.environ.copy()
    environment.setdefault("NDN_LOG", "*=WARN:ndnsf.di.RuntimeEvidence=WARN")
    completed = subprocess.run(
        [
            str(BINARY),
            f"--run_test=Spec175NativeAssembly/{case_name}",
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
    assert "1 test case out of 1 passed" in output, output
    assert case_name in output, output
    assert provider_count in (1, 2, 4)
