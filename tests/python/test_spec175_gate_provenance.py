from __future__ import annotations

from pathlib import Path
import importlib.util
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def _load_g1_runner():
    path = ROOT / "scripts/run_spec175_python_gate.py"
    spec = importlib.util.spec_from_file_location("spec175_g1_runner", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _missing_source_seal_result(script: str, tmp_path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / script),
            "--output",
            str(tmp_path / "manifest.json"),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_g1_gate_requires_source_seal_before_running_tests(tmp_path: Path) -> None:
    result = _missing_source_seal_result("run_spec175_python_gate.py", tmp_path)
    assert result.returncode == 2
    assert "--source-seal" in result.stderr


def test_g2_gate_requires_source_seal_before_running_binary(tmp_path: Path) -> None:
    result = _missing_source_seal_result("run_spec175_integration_gate.py", tmp_path)
    assert result.returncode == 2
    assert "--source-seal" in result.stderr


def test_g1_artifact_record_requires_resolved_ldd_closure(tmp_path: Path) -> None:
    runner = _load_g1_runner()
    missing = runner._artifact_record(tmp_path / "missing.so")
    assert missing["available"] is False
    assert missing["ready"] is False
    assert missing["sha256"] is None

    subject = runner._artifact_record(Path("/bin/true"))
    assert subject["available"] is True
    assert subject["ready"] is True
    assert str(subject["sha256"]).startswith("sha256:")
    assert subject["unresolvedLibraries"] == []


def test_g1_runner_records_native_and_python_artifact_contract() -> None:
    source = (ROOT / "scripts/run_spec175_python_gate.py").read_text(
        encoding="utf-8")
    for field in (
        '"nativeUnit"',
        '"pythonExtension"',
        '"pythonImport"',
        '"blockingIssues"',
        '"toolchain"',
    ):
        assert field in source
