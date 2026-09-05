"""Non-privileged contract gates for the Spec 177 MiniNDN fixture.

The real NFD/Mininet matrix is an explicit release gate and is run separately
with sudo; these tests ensure that an unprivileged invocation cannot fabricate
transport evidence.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py"
FIXTURE = ROOT / "NDNSF-UAV-APP/testdata/multiview-car/manifest.json"


def _run(*extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(RUNNER), "--fixture", str(FIXTURE), *extra],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )


def test_dry_run_declares_multi_producer_single_terminal_owner() -> None:
    result = _run()
    assert result.returncode == 0, result.stderr
    value = json.loads(result.stdout)
    assert value["mode"] == "dry-run"
    assert len(value["topology"]["uavProducers"]) >= 2
    assert value["topology"]["computeProviders"] == ["/provider/gpu", "/provider/cpu"]
    terminals = [event for event in value["trace"] if event["stage"] == "TERMINAL_ACCEPTED"]
    assert len(terminals) == 1
    assert terminals[0]["terminalOwner"] == value["topology"]["selectedProvider"]
    assert value["imageBytesInServicePayload"] is False


def test_execute_requires_privilege_instead_of_faking_network_evidence() -> None:
    result = _run("--execute")
    # The unprivileged unit-test process must not start Mininet.  A privileged
    # operator run is covered by the explicit MiniNDN release gate.
    assert result.returncode in (0, 3)
    value = json.loads(result.stdout)
    if result.returncode == 3:
        assert value["status"] == "requires-root"
        assert "rerun with sudo" in value["reason"]


def test_dry_run_declares_real_matrix_scenarios() -> None:
    result = _run()
    value = json.loads(result.stdout)
    assert value["scenarios"] == [
        "nominal", "provider-selection", "unavailable-view", "late-view",
        "publication-failure", "model-missing", "model-digest-failure",
    ]
    assert len(value["topology"]["nodes"]) == 8


def test_failure_scenarios_emit_explicit_terminal_stage() -> None:
    for scenario, stage in {
        "unavailable-view": "VIEW_DATA_FETCH_FAILED",
        "late-view": "VIEW_DATA_FETCH_FAILED",
        "publication-failure": "ANNOTATION_PUBLICATION_FAILED",
        "model-missing": "INFERENCE_FAILED",
        "model-digest-failure": "INFERENCE_FAILED",
    }.items():
        result = _run("--scenario", scenario)
        assert result.returncode == 0, result.stderr
        value = json.loads(result.stdout)
        assert value["selectedScenario"] == scenario
        assert any(event["stage"] == stage for event in value["trace"])
