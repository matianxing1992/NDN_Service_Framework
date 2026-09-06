from __future__ import annotations

import importlib.machinery
import importlib.util
import json
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[2]
SEALER = REPO / "scripts/seal_spec175_gate_prerequisite.py"
VALIDATOR_PATH = (
    REPO / "packaging/ndnsf-di-container/bin/spec175-gate-prerequisites")
LOADER = importlib.machinery.SourceFileLoader(
    "spec175_gate_prerequisites", str(VALIDATOR_PATH))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
assert SPEC is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(VALIDATOR)


IDENTITY = {
    "sourceSeal": "sha256:" + "1" * 64,
    "exactSif": "sha256:" + "2" * 64,
    "workloadConfig": "sha256:" + "3" * 64,
    "modelArtifacts": "sha256:" + "4" * 64,
}


def _write(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def _closure(path: Path, gate: str) -> None:
    _write(path, {
        "schema": "ndnsf-di-spec175-candidate-closure-v1",
        "status": "PASS", "candidateId": "candidate-175",
        "selectedGate": gate, "candidateTuple": IDENTITY,
    })


def _seal(tmp_path: Path, gate: str) -> Path:
    closure = tmp_path / f"closure-{gate}.json"
    evidence = tmp_path / f"evidence-{gate}.json"
    output = tmp_path / f"prerequisite-{gate}.json"
    _closure(closure, gate)
    _write(evidence, {"status": "PASS", "gate": gate})
    completed = subprocess.run([
        sys.executable, str(SEALER), "--gate", gate,
        "--candidate-closure", str(closure), "--evidence", str(evidence),
        "--output", str(output),
    ], text=True, capture_output=True, check=False)
    assert completed.returncode == 0, completed.stderr
    return output


def test_sealed_g5_g6_g6c_prerequisites_authorize_g7(tmp_path: Path):
    paths = {gate: _seal(tmp_path, gate) for gate in ("G5", "G6", "G6C")}
    closure = tmp_path / "closure-G7.json"
    _closure(closure, "G7")
    result = VALIDATOR.validate(closure, "G7", [
        f"{gate}={path}" for gate, path in paths.items()])
    assert result["status"] == "PASS", result["errors"]
    assert set(result["prerequisites"]) == {"G5", "G6", "G6C"}


def test_prerequisite_validator_rejects_identity_drift(tmp_path: Path):
    paths = {gate: _seal(tmp_path, gate) for gate in ("G5", "G6", "G6C")}
    value = json.loads(paths["G6C"].read_text(encoding="utf-8"))
    value["sifSha256"] = "sha256:" + "9" * 64
    _write(paths["G6C"], value)
    closure = tmp_path / "closure-G7.json"
    _closure(closure, "G7")
    result = VALIDATOR.validate(closure, "G7", [
        f"{gate}={path}" for gate, path in paths.items()])
    assert result["status"] == "FAIL"
    assert any("sifSha256 mismatch" in error for error in result["errors"])


def test_prerequisite_validator_rejects_missing_cumulative_gate(tmp_path: Path):
    paths = {gate: _seal(tmp_path, gate) for gate in ("G5", "G6")}
    closure = tmp_path / "closure-G7.json"
    _closure(closure, "G7")
    result = VALIDATOR.validate(closure, "G7", [
        f"{gate}={path}" for gate, path in paths.items()])
    assert result["status"] == "FAIL"
    assert any("requires exactly" in error for error in result["errors"])
