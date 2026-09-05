"""Non-privileged Spec179 MiniNDN contract tests.

The actual NFD/Mininet run is a separate release gate.  These tests verify that
the launcher declares the required Cartesian matrix and refuses to fabricate
network evidence when the host lacks the runtime-control or privilege gate.
"""

from __future__ import annotations

import json
import importlib.util
from pathlib import Path
import subprocess
import sys
import time

import pytest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "tests/minindn/run_request_scoped_confidentiality.py"


def _load_runner():
    spec = importlib.util.spec_from_file_location("spec179_runner", RUNNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _run(*extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(RUNNER), *extra],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )


def test_dry_run_declares_controller_revocation_matrix() -> None:
    result = _run()
    assert result.returncode == 0, result.stderr
    value = json.loads(result.stdout)
    assert value["mode"] == "dry-run"
    assert value["status"] == "contract-only"
    assert value["networkEvidence"] is False
    assert set(value["topology"]["nodes"]) == {
        "relay", "controller", "user-a", "user-b", "provider-a", "provider-b", "cache",
    }
    assert len(value["topology"]["users"]) == 2
    assert len(value["topology"]["providers"]) == 2
    assert set(value["matrix"]["revokedSubject"]) == {"identity", "certificate", "service"}
    assert len(value["matrix"]["cutPoint"]) == 6
    assert set(value["matrix"]["mode"]) == {"normal", "large-response", "targeted", "stream"}
    assert "controller-restart" in value["scenarios"]


def test_execute_never_fabricates_evidence_without_gate_prerequisites() -> None:
    result = _run("--execute")
    assert result.returncode in (3, 4), result.stderr
    value = json.loads(result.stdout)
    assert value["networkEvidence"] is False
    assert value["status"] in {"requires-root", "preflight-failed", "not-ready"}
    if value["status"] == "requires-root":
        assert all(value["runtimeControls"].values())
    if value["status"] == "requires-root":
        assert "rerun with sudo" in value["reason"]


def test_each_named_scenario_is_selectable() -> None:
    baseline = json.loads(_run().stdout)
    for scenario in baseline["scenarios"]:
        result = _run("--scenario", scenario)
        assert result.returncode == 0, (scenario, result.stderr)
        value = json.loads(result.stdout)
        assert value["selectedScenario"] == scenario
        assert value["requiredEvidence"]


def test_named_modes_map_to_real_runtime_probes() -> None:
    runner = _load_runner()
    assert runner._scenario_config("large-response-invalidation")["mode"] == "large-response"
    assert runner._scenario_config("targeted-refill-invalidation")["mode"] == "targeted"
    assert runner._scenario_config("stream-invalidation")["mode"] == "stream"
    assert runner._scenario_config("user-identity-revocation")["mode"] == "normal"


def test_execution_evidence_uses_lifecycle_state_when_timestamp_is_zero(tmp_path: Path) -> None:
    runner = _load_runner()
    provider_dir = tmp_path / "provider-A"
    provider_dir.mkdir()
    lifecycle = provider_dir / "provider-lifecycle.csv"
    lifecycle.write_text(
        "request_id,provider_name,state,execution_start_timestamp_us\n"
        "/r1,/provider/A,EXECUTION_DONE,0\n"
        "/r1,/provider/A,RESPONSE_PUBLISHED,0\n",
        encoding="utf-8",
    )
    evidence = runner._collect_runtime_evidence(
        tmp_path,
        "user-identity-revocation",
        {"mode": "normal", "recovery": "none"},
        [],
        time.monotonic(),
    )
    assert evidence["executionCount"] == 1


def test_mode_specific_evidence_counts_are_not_inferred_from_csv(tmp_path: Path) -> None:
    runner = _load_runner()
    (tmp_path / "provider-a.log").write_text(
        "NDNSF_PUBLICATION_AUDIT type=REQUEST validated=true\n"
        "NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_PUBLISHED requestId=/r1\n"
        "NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_RESOLVED requestId=/r1\n"
        "NDNSF_REQUEST_SCOPED_RESPONSE_DECRYPTED requestId=/r1\n"
        "[NDNSF_TRACE] role=provider event=TARGETED_TOKEN_BATCH_ATTACHED\n"
        "[NDNSF_TRACE] role=provider event=TARGETED_REQUEST_ACCEPTED\n"
        "SPEC179_STREAM_EVENT_PUBLISHED provider=A sequence=1\n",
        encoding="utf-8",
    )
    (tmp_path / "user-a.log").write_text(
        "[NDNSF_TRACE] role=user event=TARGETED_REQUEST_CREATED fastPath=1\n"
        "[NDNSF_TRACE] role=user event=TARGETED_TOKEN_BATCH_STORED poolDepth=1\n"
        "[NDNSF_TRACE] role=user event=TARGETED_TOKEN_REFILL_REQUESTED\n"
        "SPEC179_STREAM_EVENT_RECEIVED user=A bytes=1\n"
        "SPEC179_STREAM_ERROR user=A code=8\n",
        encoding="utf-8",
    )
    evidence = runner._collect_runtime_evidence(
        tmp_path,
        "targeted-refill-invalidation",
        {"mode": "targeted", "recovery": "targeted-refill"},
        [],
        time.monotonic(),
    )
    assert evidence["targetedRequestCount"] == 1
    assert evidence["targetedFastPathCount"] == 1
    assert evidence["targetedBootstrapCount"] == 1
    assert evidence["targetedBatchStoredCount"] == 1
    assert evidence["targetedRefillCount"] == 1
    assert evidence["largeResponsePublishedCount"] == 1
    assert evidence["streamEventPublishedCount"] == 1
    assert evidence["streamEventReceivedCount"] == 1
    assert evidence["streamErrorCount"] == 1


def test_preflight_uses_one_explicit_build_tree_without_stale_fallback(tmp_path: Path) -> None:
    runner = _load_runner()
    build_dir = tmp_path / "candidate-build"
    candidates = runner._binary_candidates("App_User", build_dir)
    assert candidates == [build_dir.resolve() / "examples" / "App_User"]
    assert runner._find_binary("App_User", build_dir) is None


def test_preflight_records_exact_build_tree() -> None:
    runner = _load_runner()
    report = runner.preflight()
    assert report["buildDir"] == str(runner.DEFAULT_BUILD_DIR.resolve())


def test_minindn_work_dir_keeps_nfd_socket_path_short(tmp_path: Path) -> None:
    runner = _load_runner()
    long_output = tmp_path / ("service-scoped-revocation-with-unaffected-control-" * 4)
    work_dir = runner._minindn_work_dir(long_output)
    socket = work_dir / "controller" / "controller.sock"
    assert len(str(socket)) < 108


def test_grant_evidence_retains_failures_while_providers_are_alive(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(RUNNER.parent))
    runner = _load_runner()
    for name in ("provider-A", "provider-B"):
        (tmp_path / (name + ".log")).write_text(
            "100.0 INFO started\n200.0 INFO still serving\n", encoding="utf-8")
    user = tmp_path / "user-B"
    user.mkdir()
    (user / "request-results.csv").write_text(
        "request_id,success\nok,1\nbroken,0\n", encoding="utf-8")
    (user / "request_lifecycle.csv").write_text(
        "request_id,enqueue_timestamp_us\nok,120000000\nbroken,130000000\n",
        encoding="utf-8")
    evidence = runner._collect_runtime_evidence(
        tmp_path, "grant-only-advance", runner._scenario_config("grant-only-advance"),
        [], time.monotonic())
    assert evidence["grantOnly"]["grantedRows"] == 2
    assert evidence["grantOnly"]["grantedSuccessRows"] == 1


def test_completed_but_failed_network_gate_returns_nonzero(monkeypatch, tmp_path):
    runner = _load_runner()
    monkeypatch.setattr(sys, "argv", [str(RUNNER), "--execute", "--output", str(tmp_path)])
    monkeypatch.setattr(runner, "execute_gate", lambda *args: {
        "status": "completed", "gatePassed": False, "networkEvidence": True})
    assert runner.main() != 0


def test_late_grant_exports_evidence_and_requires_observed_renewal(tmp_path):
    runner = _load_runner()
    name = "grant-after-permission-exhaustion"
    evidence = runner._collect_runtime_evidence(
        tmp_path, name, runner._scenario_config(name), [], time.monotonic())
    assert evidence["grantOnly"]["lateRenewalObserved"] is False
    assert evidence["grantOnly"]["permissionExhaustedTimeUs"] is None


def test_permission_loss_refuses_host_namespace():
    runner = _load_runner()

    class HostNode:
        name = "user-b"
        inNamespace = False

        def pexec(self, _command):
            pytest.fail("must never change the host packet filter")

    with pytest.raises(RuntimeError, match="isolated user-b namespace"):
        runner._set_startup_permission_loss(HostNode(), True)
