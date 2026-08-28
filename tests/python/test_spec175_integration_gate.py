from __future__ import annotations

import importlib.util
import hashlib
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/run_spec175_integration_gate.py"


def _load_gate():
    spec = importlib.util.spec_from_file_location("spec175_integration_gate", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _source_seal(tmp_path: Path) -> Path:
    seal = tmp_path / "source-seal.json"
    seal.write_text(
        json.dumps({
            "schemaVersion": "spec175-source-seal-v1",
            "sourceRevision": "a" * 40,
            "dirtyFiles": [],
        }),
        encoding="utf-8",
    )
    return seal


def test_case_selector_expands_registered_ranges() -> None:
    gate = _load_gate()
    assert gate.parse_cases("I01,I03-I05") == ("I01", "I03", "I04", "I05")
    with pytest.raises(ValueError):
        gate.parse_cases("I00")


def test_missing_cases_are_blockers_not_skips(tmp_path: Path) -> None:
    gate = _load_gate()
    gate.REGISTERED.pop("I12")
    output = tmp_path / "g2.json"
    manifest = gate.run_gate(
        binary=tmp_path / "missing-binary",
        cases=("I12", "I13"), healthy_repeats=3, seed=1750001,
        output=output, source_seal=_source_seal(tmp_path),
    )
    assert manifest["status"] == "BLOCKED_MISSING_CASES"
    assert manifest["missingCases"] == ["I12"]
    assert manifest["binaryAvailable"] is False
    assert manifest["results"] == []
    assert json.loads(output.read_text()) == manifest


def test_missing_binary_is_a_structured_blocker(tmp_path: Path) -> None:
    gate = _load_gate()
    manifest = gate.run_gate(
        binary=tmp_path / "missing-binary",
        cases=("I01",), healthy_repeats=1, seed=1750001,
        output=tmp_path / "g2.json", source_seal=_source_seal(tmp_path),
    )
    assert manifest["status"] == "BLOCKED_BINARY_UNAVAILABLE"
    assert manifest["binaryAvailable"] is False
    assert manifest["missingCases"] == []
    assert manifest["results"] == []


def test_registered_cases_run_when_other_requested_cases_are_missing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    gate = _load_gate()
    gate.REGISTERED.clear()
    gate.REGISTERED.update({"I01": "healthy-test"})
    observed = []

    def fake_run(_binary, case_id, test_name, *, seed, repetition, log_dir,
                 timeout_seconds):
        observed.append((case_id, test_name, seed))
        return {
            "caseId": case_id, "testName": test_name, "seed": seed,
            "exitCode": 0, "status": "PASS", "stdoutBytes": 0,
            "stderrBytes": 0,
        }

    monkeypatch.setattr(gate, "_run_case", fake_run)
    binary = tmp_path / "integration-tests"
    binary.write_text("#!/bin/sh\nexit 0\n")
    binary.chmod(0o755)
    manifest = gate.run_gate(
        binary=binary, cases=("I01", "I02"),
        healthy_repeats=3, seed=1750001, output=tmp_path / "g2.json",
        source_seal=_source_seal(tmp_path),
    )

    assert manifest["status"] == "BLOCKED_MISSING_CASES"
    assert manifest["missingCases"] == ["I02"]
    assert [item[0] for item in observed] == ["I01", "I01", "I01"]
    assert len(manifest["results"]) == 3


def test_native_tiny_onnx_healthy_paths_are_formal_g2_cases() -> None:
    gate = _load_gate()
    assert set(gate.REGISTERED) == {
        "I01", "I02", "I03", "I04", "I05", "I06", "I07", "I08",
            "I09", "I10", "I11", "I12", "I13", "I14", "I15",
    }
    assert set(gate.PYTHON_REGISTERED) == {"I16", "I17", "I18", "I19", "I20"}
    assert set(gate.ALL_CASES) == set(gate.REGISTERED) | set(gate.PYTHON_REGISTERED)
    assert all(name.startswith("Spec170NdnsfDiCoreFlow/Spec175")
               for name in gate.REGISTERED.values())
    assert gate.HEALTHY_CASES == {
        "I01", "I02", "I03", "I12", "I14", "I15", "I16", "I17", "I18",
    }


def test_conversation_cases_run_through_explicit_python_runner(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    gate = _load_gate()
    observed = []

    def fake_run(case_id, test_name, *, seed, repetition, log_dir,
                 timeout_seconds):
        observed.append((case_id, test_name, seed, repetition))
        return {
            "caseId": case_id, "runner": "python", "testName": test_name,
            "seed": seed, "repetition": repetition, "exitCode": 0,
            "status": "PASS", "stdoutBytes": 0, "stderrBytes": 0,
            "metrics": {"scope": "conversation-scoped"},
            "metricsMissing": False,
        }

    monkeypatch.setattr(gate, "_run_python_case", fake_run)
    manifest = gate.run_gate(
        binary=tmp_path / "missing-integration-binary",
        cases=("I16", "I19"), healthy_repeats=3, seed=1750001,
        output=tmp_path / "g2.json", source_seal=_source_seal(tmp_path),
    )
    assert manifest["status"] == "PASS"
    assert [item[0] for item in observed] == ["I16", "I16", "I16", "I19"]
    assert manifest["runnerByCase"] == {"I16": "python", "I19": "python"}


def test_fault_cases_run_once_while_healthy_cases_repeat(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    gate = _load_gate()
    gate.REGISTERED.update({"I01": "healthy-test", "I04": "fault-test"})
    observed = []

    def fake_run(_binary, case_id, test_name, *, seed, repetition, log_dir,
                 timeout_seconds):
        observed.append((case_id, test_name, seed))
        return {
            "caseId": case_id, "testName": test_name, "seed": seed,
            "exitCode": 0, "status": "PASS", "stdoutBytes": 0,
            "stderrBytes": 0,
        }

    monkeypatch.setattr(gate, "_run_case", fake_run)
    binary = tmp_path / "integration-tests"
    binary.write_text("#!/bin/sh\nexit 0\n")
    binary.chmod(0o755)
    manifest = gate.run_gate(
        binary=binary, cases=("I01", "I04"),
        healthy_repeats=3, seed=1750001, output=tmp_path / "g2.json",
        source_seal=_source_seal(tmp_path),
    )

    assert manifest["status"] == "PASS"
    assert [item[0] for item in observed] == ["I01", "I01", "I01", "I04"]
    assert manifest["faultRepeats"] == 1
    assert manifest["sourceSeal"]["path"].endswith("source-seal.json")
    assert manifest["sourceSeal"]["sha256"].startswith("sha256:")
    assert manifest["binarySha256"] == "sha256:" + hashlib.sha256(
        binary.read_bytes()).hexdigest()


def test_missing_source_seal_is_rejected(tmp_path: Path) -> None:
    gate = _load_gate()
    with pytest.raises(ValueError, match="source seal"):
        gate.run_gate(
            binary=tmp_path / "integration-tests",
            cases=("I01",), healthy_repeats=1, seed=1750001,
            output=tmp_path / "g2.json",
            source_seal=tmp_path / "missing-source-seal.json",
        )


def test_case_runner_preserves_stdout_and_stderr_with_digests(tmp_path: Path) -> None:
    gate = _load_gate()
    binary = tmp_path / "integration-tests"
    binary.write_text(
        "#!/bin/sh\nprintf 'case stdout\\n'\nprintf 'case stderr\\n' >&2\nexit 1\n",
        encoding="utf-8",
    )
    binary.chmod(0o755)
    result = gate._run_case(
        binary, "I12", "suite/case", seed=1750001, repetition=2,
        log_dir=tmp_path / "logs", timeout_seconds=10,
    )
    assert result["status"] == "FAIL"
    assert result["repetition"] == 2
    for stream in ("stdout", "stderr"):
        path = Path(result[f"{stream}Path"])
        assert path.is_file()
        assert result[f"{stream}Sha256"].startswith("sha256:")


def test_case_runner_times_out_and_preserves_partial_evidence(tmp_path: Path) -> None:
    gate = _load_gate()
    binary = tmp_path / "integration-tests"
    binary.write_text(
        "#!/bin/sh\nprintf 'started\\n'\nsleep 2\n",
        encoding="utf-8",
    )
    binary.chmod(0o755)
    result = gate._run_case(
        binary, "I12", "suite/case", seed=1750001, repetition=1,
        log_dir=tmp_path / "logs", timeout_seconds=0.05,
    )
    assert result["status"] == "TIMEOUT"
    assert result["exitCode"] == 124
    assert result["timedOut"] is True
    assert "SPEC175_CASE_TIMEOUT" in Path(result["stderrPath"]).read_text()
