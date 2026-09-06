from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
INVENTORY_SCRIPT = ROOT / "scripts/spec180_inventory.py"
RUNNER_SCRIPT = ROOT / "scripts/run_spec180_local_gate.py"
QWEN_WRAPPER = ROOT / "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py"


def _load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _modules():
    inventory = _load(INVENTORY_SCRIPT, "spec180_inventory")
    runner = _load(RUNNER_SCRIPT, "run_spec180_local_gate")
    return inventory, runner


def _digest(value) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _fixture_root(tmp_path: Path, inventory) -> Path:
    for relative in ("build/unit-tests", "build/integration-tests"):
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        target.chmod(0o755)
    case_body = (
        "import sys\n"
        "case = sys.argv[sys.argv.index('--case') + 1] if '--case' in sys.argv else None\n"
        "if '--spec175-case' in sys.argv: case = sys.argv[sys.argv.index('--spec175-case') + 1]\n"
        "if case in {'M01', 'M11'}: print(f'NDNSF_DI_SPEC175_MININDN_WRAPPER_PASS case={case}')\n"
        "else: print(f'SPEC180_CASE_RESULT status=PASS case={case}')\n"
    )
    for _case, relative, _args in inventory.DEFAULT_CASES:
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(case_body, encoding="utf-8")
    test_file = tmp_path / "tests/python/test_spec180_fixture.py"
    test_file.parent.mkdir(parents=True, exist_ok=True)
    test_file.write_text("def test_fixture():\n    assert True\n", encoding="utf-8")
    return tmp_path


def _inventory(tmp_path: Path):
    inventory, runner = _modules()
    root = _fixture_root(tmp_path, inventory)
    value = inventory.build_inventory(
        root,
        candidate_id="candidate-test",
        candidate_digest="sha256:" + "a" * 64,
        source_revision="b" * 40,
        effective_config_digest="sha256:" + "c" * 64,
        integration_listing="Suite*\n    Test*\n",
        python_selectors=(
            "tests/python/test_spec180_fixture.py::test_fixture",
        ),
    )
    return inventory, runner, root, value


def _refresh_inventory_digest(inventory_module, inventory) -> None:
    without_digest = dict(inventory)
    without_digest.pop("inventoryDigest", None)
    inventory["inventoryDigest"] = inventory_module.canonical_digest(without_digest)


def _file_digest(inventory_module, path: Path) -> str:
    return inventory_module.digest_bytes(path.read_bytes())


def test_local_gate_runs_each_inventory_entry_and_seals_snapshot(tmp_path: Path):
    _inventory_module, runner, root, inventory = _inventory(tmp_path)
    result = runner.run_local_gate(
        inventory,
        root=root,
        output_root=tmp_path / "evidence",
        environment={"PYTHONPATH": "", "SPEC175_RUN_REAL_MININDN": "1"},
    )
    assert result["status"] == "PASS"
    assert result["entryCount"] == 6
    assert all(item["status"] == "PASS" for item in result["entries"])
    assert all(isinstance(item["pid"], int) for item in result["entries"])
    snapshot = Path(result["inventoryPath"])
    assert snapshot.is_file()
    assert result["inventoryFileSha256"].startswith("sha256:")
    assert all(Path(item["stdoutPath"]).is_file() for item in result["entries"])


def test_source_digest_failure_has_no_child_or_output_side_effect(tmp_path: Path):
    _inventory_module, runner, root, inventory = _inventory(tmp_path)
    case_file = root / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
    case_file.write_text(case_file.read_text(encoding="utf-8") + "# tamper\n",
                         encoding="utf-8")
    output = tmp_path / "evidence"
    with pytest.raises(runner.LocalGateError, match="ENTRY_FILE_DIGEST_MISMATCH"):
        runner.run_local_gate(inventory, root=root, output_root=output,
                              environment={"SPEC175_RUN_REAL_MININDN": "1"})
    assert not output.exists()


def test_missing_case_oracle_is_unqualified(tmp_path: Path):
    _inventory_module, runner, root, inventory = _inventory(tmp_path)
    target = next(item for item in inventory["entries"] if item["id"] == "minindn-y-a")
    source = root / target["path"]
    source.write_text(
        "import sys\n"
        "case = sys.argv[sys.argv.index('--case') + 1]\n"
        "if case != 'Y-A': print(f'SPEC180_CASE_RESULT status=PASS case={case}')\n",
        encoding="utf-8",
    )
    for entry in inventory["entries"]:
        if entry["path"] == target["path"]:
            entry["artifactSha256"] = _file_digest(_inventory_module, source)
    _refresh_inventory_digest(_inventory_module, inventory)
    result = runner.run_local_gate(
        inventory, root=root, output_root=tmp_path / "evidence",
        environment={"SPEC175_RUN_REAL_MININDN": "1"},
    )
    assert result["status"] == "UNQUALIFIED"
    assert result["failedEntryIds"] == ["minindn-y-a"]
    assert result["entries"][3]["status"] == "CASE_ORACLE_MISSING"


def test_secret_like_child_output_is_redaction_failure(tmp_path: Path):
    _inventory_module, runner, root, inventory = _inventory(tmp_path)
    target = next(item for item in inventory["entries"] if item["id"] == "cpp-unit-suite")
    alternate = root / "build/unit-secret"
    alternate.write_text("#!/bin/sh\necho secret=do-not-store\n", encoding="utf-8")
    alternate.chmod(0o755)
    target["path"] = "build/unit-secret"
    target["artifactSha256"] = _file_digest(_inventory_module, alternate)
    target["command"] = [target["path"], "--log_level=nothing"]
    target["commandDigest"] = _digest(target["command"])
    _refresh_inventory_digest(_inventory_module, inventory)
    result = runner.run_local_gate(
        inventory, root=root, output_root=tmp_path / "evidence",
        environment={"SPEC175_RUN_REAL_MININDN": "1"},
    )
    assert result["status"] == "UNQUALIFIED"
    unit = result["entries"][0]
    assert unit["status"] == "REDACTION_FAIL"
    stored = Path(unit["stdoutPath"]).read_text(encoding="utf-8")
    assert "do-not-store" not in stored
    assert "<REDACTED>" in stored


def test_nonempty_output_root_is_rejected_before_execution(tmp_path: Path):
    _inventory_module, runner, root, inventory = _inventory(tmp_path)
    output = tmp_path / "evidence"
    output.mkdir()
    (output / "old.json").write_text("old", encoding="utf-8")
    with pytest.raises(runner.LocalGateError, match="OUTPUT_ROOT_NOT_EMPTY"):
        runner.run_local_gate(inventory, root=root, output_root=output,
                              environment={"SPEC175_RUN_REAL_MININDN": "1"})


def test_command_outside_inventory_source_is_rejected(tmp_path: Path):
    inventory_module, runner, root, inventory = _inventory(tmp_path)
    target = next(item for item in inventory["entries"] if item["id"] == "cpp-unit-suite")
    target["command"] = ["/bin/true"]
    target["commandDigest"] = _digest(target["command"])
    _refresh_inventory_digest(inventory_module, inventory)
    with pytest.raises(runner.LocalGateError, match="ENTRY_COMMAND_PATH_MISMATCH"):
        runner.run_local_gate(inventory, root=root,
                              output_root=tmp_path / "evidence",
                              environment={"SPEC175_RUN_REAL_MININDN": "1"})


def test_out_of_scope_qwen_case_rejected_before_any_child(tmp_path: Path):
    inventory_module, runner, root, inventory = _inventory(tmp_path)
    entry = next(item for item in inventory["entries"] if item["kind"] == "minindn-case")
    entry["case"] = "Q-C"
    _refresh_inventory_digest(inventory_module, inventory)
    with pytest.raises(runner.LocalGateError,
                       match="UNREGISTERED_CASE:Q-C"):
        runner.run_local_gate(inventory, root=root,
                              output_root=tmp_path / "evidence", environment={})
    assert not (tmp_path / "evidence").exists()


def test_cli_rejects_non_string_environment_values(tmp_path: Path):
    inventory_module, runner, root, inventory = _inventory(tmp_path)
    del inventory_module
    inventory_path = tmp_path / "inventory.json"
    inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    environment_path = tmp_path / "environment.json"
    environment_path.write_text(json.dumps({"SPEC175_RUN_REAL_MININDN": 1}),
                                encoding="utf-8")
    assert runner.main([
        "--inventory", str(inventory_path), "--root", str(root),
        "--output-root", str(tmp_path / "evidence"),
        "--environment-json", str(environment_path),
    ]) == 78


def test_qwen_wrapper_accepts_runner_owned_output_directory(monkeypatch,
                                                             tmp_path: Path):
    wrapper = _load(QWEN_WRAPPER, "spec180_qwen_wrapper")
    monkeypatch.setenv("SPEC180_CASE_OUTPUT_DIR", str(tmp_path / "q-c"))
    args = wrapper.build_parser().parse_args(["--case", "M01", "--seed", "1750001"])
    assert args.output_dir == str(tmp_path / "q-c")
