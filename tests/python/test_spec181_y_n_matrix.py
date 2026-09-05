"""Focused evidence-integrity regressions; no MiniNDN processes are started."""

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

from test_spec180_yolo_minindn import load_runner


ROOT = Path(__file__).resolve().parents[2]
LEGACY = ROOT / "scripts/run_spec181_y_n_matrix_retry.py"


def test_retired_driver_preserves_existing_evidence_and_starts_nothing(tmp_path, monkeypatch):
    module_spec = importlib.util.spec_from_file_location("spec181_legacy_matrix", LEGACY)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    evidence = tmp_path / "Y-N-O" / "attempt-1" / "subcase-result.json"
    evidence.parent.mkdir(parents=True)
    evidence.write_text('{"status":"UNQUALIFIED","reason":"original failure"}\n')
    original = evidence.read_bytes()
    calls = []
    if hasattr(module, "runner"):
        monkeypatch.setattr(module.runner, "validate_inputs", lambda *a: calls.append(a))
    if hasattr(module, "shutil"):
        monkeypatch.setattr(module.shutil, "rmtree", lambda *a, **k: calls.append(a))
    monkeypatch.setattr(sys, "argv", [str(LEGACY), "--output-root", str(tmp_path)])
    assert module.main() == 2
    assert not calls
    assert evidence.read_bytes() == original
    assert not (tmp_path / "retry-driver-result.json").exists()


def test_retired_driver_cli_needs_no_runtime_imports_or_output_directory(tmp_path):
    target = tmp_path / "never-created"
    result = subprocess.run([sys.executable, "-I", str(LEGACY),
                             "--output-root", str(target), "--max-attempts", "3"],
                            text=True, capture_output=True, timeout=10)
    assert result.returncode == 2
    assert "LEGACY_RETRY_DRIVER_DISABLED" in result.stdout
    assert not target.exists()


@pytest.mark.parametrize("failed", ["Y-N-O", "Y-N-P", "Y-N-E"])
def test_matrix_stops_at_first_failure_and_preserves_original_result(tmp_path, monkeypatch, failed):
    module = load_runner()
    calls = []
    original = '{"status":"UNQUALIFIED","reason":"original failure"}\n'

    def live(_case, output, _inputs, *, subcase):
        calls.append(subcase)
        if subcase == failed:
            (output / "subcase-result.json").write_text(original)
            raise module.RunnerError("CHILD_CLEANUP_FAILED")
        module._write_subcase_result(
            output, subcase=subcase, status="PASS",
            outcome="CONTROL" if subcase == "Y-N-O" else "FAIL_CLOSED",
            reason="DIAGNOSTIC_FIXTURE", child_count=7)
        return 0

    def mutations(output, inputs):
        return live("Y-N", output, inputs, subcase="Y-N-E")

    monkeypatch.setattr(module, "_run_live_case_once", live)
    monkeypatch.setattr(module, "_run_y_n_e_variants", mutations)
    with pytest.raises(module.RunnerError, match="Y_N_MATRIX_INCOMPLETE:" + failed):
        module._run_y_n_matrix(tmp_path, {})
    assert calls == list(module.YN_SUBCASES[:module.YN_SUBCASES.index(failed) + 1])
    assert (tmp_path / "subcases" / failed / "subcase-result.json").read_text() == original
    assert not (tmp_path / "y-n-matrix-result.json").exists()
