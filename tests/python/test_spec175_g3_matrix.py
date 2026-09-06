"""Focused checks for the single Spec175 G3 matrix entry point."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/run_spec175_g3_matrix.py"


def _module():
    spec = importlib.util.spec_from_file_location("spec175_g3_matrix", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _args(module, tmp_path: Path, *, dry_run: bool = True):
    argv = [
        "--project-root", str(ROOT),
        "--output-root", str(tmp_path / "g3"),
        "--manifest-output", str(tmp_path / "g3-manifest.json"),
        "--source-seal", str(ROOT / ".specify/feature.json"),
        "--topology-file", str(ROOT / "Experiments/Topology/spec175-host-gate.conf"),
    ]
    if dry_run:
        argv.append("--dry-run")
    return module.build_parser().parse_args(argv)


def test_dry_run_renders_exact_fixed_matrix(capsys, tmp_path: Path) -> None:
    module = _module()
    args = _args(module, tmp_path)

    assert module.run_matrix(args) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["status"] == "DRY_RUN"
    assert payload["cases"] == list(module.CASES)
    assert payload["repetitionsPerCase"] == 3
    assert payload["total"] == 42
    assert payload["entries"][0]["seed"] == module.WORKLOAD_SEED
    assert payload["entries"][9]["seed"] == module.WORKLOAD_SEED
    assert payload["entries"][12]["seed"] == module.FAULT_SEED
    assert "--admission-control" not in payload["entries"][0]["command"]
    assert not (tmp_path / "g3").exists()


def test_existing_output_root_is_rejected_without_mutation(monkeypatch, tmp_path: Path) -> None:
    module = _module()
    monkeypatch.setattr(module.os, "geteuid", lambda: 0)
    output_root = tmp_path / "g3"
    output_root.mkdir()
    marker = output_root / "do-not-touch"
    marker.write_text("keep\n", encoding="utf-8")
    args = module.build_parser().parse_args([
        "--project-root", str(ROOT),
        "--output-root", str(output_root),
        "--manifest-output", str(tmp_path / "manifest.json"),
        "--source-seal", str(ROOT / ".specify/feature.json"),
        "--fixture-manifest", str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1/manifest.json"),
        "--tiny-fixture-root", str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"),
        "--topology-file", str(ROOT / "Experiments/Topology/spec175-host-gate.conf"),
    ])

    with pytest.raises(module.MatrixError, match="output root must not already exist"):
        module.run_matrix(args)
    assert marker.read_text(encoding="utf-8") == "keep\n"
    assert not (tmp_path / "manifest.json").exists()


def test_real_run_rejects_non_root_before_creating_output(monkeypatch, tmp_path: Path) -> None:
    module = _module()
    monkeypatch.setattr(module.os, "geteuid", lambda: 1000)
    args = _args(module, tmp_path, dry_run=False)

    with pytest.raises(module.MatrixError, match="requires root"):
        module.run_matrix(args)
    assert not (tmp_path / "g3").exists()
