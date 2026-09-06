from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
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


def _git(root: Path, *args: str) -> str:
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("GIT_")}
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return subprocess.check_output(
        ["git", "-C", str(root), *args], env=environment, text=True,
        stderr=subprocess.PIPE).strip()


def _seal_fixture(root: Path, *extra_paths: str) -> str:
    paths = [".gitignore", "support.py", "tests/python/test_spec180_fixture.py",
             "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"]
    _git(root, "add", "--", *paths, *extra_paths)
    _git(root, "-c", "user.name=Spec181 fixture", "-c",
         "user.email=spec181@example.invalid", "-c", "commit.gpgsign=false",
         "commit", "-qm", "Seal local gate fixture")
    return _git(root, "rev-parse", "HEAD")


def _rebind_revision(module, inventory, revision: str) -> None:
    inventory["sourceRevision"] = revision
    for entry in inventory["entries"]:
        entry["sourceRevision"] = revision
    _refresh_inventory_digest(module, inventory)


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
    (tmp_path / "support.py").write_text("VALUE = 1\n", encoding="utf-8")
    (tmp_path / ".gitignore").write_text(
        "build/\n__pycache__/\n.pytest_cache/\nevidence/\n", encoding="utf-8")
    _git(tmp_path, "init", "-q")
    _seal_fixture(tmp_path)
    return tmp_path


def _inventory(tmp_path: Path, environment=None):
    inventory, runner = _modules()
    root = _fixture_root(tmp_path, inventory)
    value = inventory.build_inventory(
        root,
        candidate_id="candidate-test",
        candidate_digest="sha256:" + "a" * 64,
        source_revision=_git(root, "rev-parse", "HEAD"),
        environment={} if environment is None else environment,
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
    environment = {"PYTHONPATH": "", "SPEC175_RUN_REAL_MININDN": "1"}
    _inventory_module, runner, root, inventory = _inventory(tmp_path, environment)
    result = runner.run_local_gate(
        inventory,
        root=root,
        output_root=tmp_path / "evidence",
        environment=environment,
    )
    assert result["status"] == "PASS"
    assert result["effectiveConfigDigest"] == _inventory_module.canonical_digest(
        result["effectiveConfiguration"])
    assert result["effectiveConfiguration"]["environmentDigest"] == _digest(environment)
    assert result["entryCount"] == 6
    assert all(item["status"] == "PASS" for item in result["entries"])
    assert all(isinstance(item["pid"], int) for item in result["entries"])
    snapshot = Path(result["inventoryPath"])
    assert snapshot.is_file()
    assert result["inventoryFileSha256"].startswith("sha256:")
    assert all(Path(item["stdoutPath"]).is_file() for item in result["entries"])
    for entry in result["entries"]:
        actual_environment = dict(environment)
        if entry["kind"] == "minindn-case":
            actual_environment["SPEC180_CASE_OUTPUT_DIR"] = str(
                tmp_path / "evidence" / entry["id"] / "case-output")
        assert entry["environmentDigest"] == _digest(actual_environment)


def test_source_digest_failure_has_no_child_or_output_side_effect(tmp_path: Path):
    _inventory_module, runner, root, inventory = _inventory(tmp_path)
    case_file = root / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
    case_file.write_text(case_file.read_text(encoding="utf-8") + "# tamper\n",
                         encoding="utf-8")
    output = tmp_path / "evidence"
    with pytest.raises(runner.LocalGateError, match="ENTRY_FILE_DIGEST_MISMATCH"):
        runner.run_local_gate(inventory, root=root, output_root=output,
                              environment={})
    assert not output.exists()


@pytest.mark.parametrize("mutation", [None, "bytes", "size"])
def test_checkout_identity_verifies_materialized_lfs_pointer(tmp_path: Path, mutation):
    _module, runner, root, _inventory_value = _inventory(tmp_path)
    content = b"materialized release fixture\n"
    target = root / "release.bin"
    target.write_text(
        "version https://git-lfs.github.com/spec/v1\n"
        "oid sha256:" + hashlib.sha256(content).hexdigest() + "\n"
        "size " + str(len(content)) + "\n", encoding="utf-8")
    revision = _seal_fixture(root, "release.bin")
    target.write_bytes(content if mutation is None else
                       content.replace(b"release", b"RELEASE") if mutation == "bytes" else
                       content + b"extra")
    if mutation is None:
        runner._validate_source_checkout(root, revision)
    else:
        with pytest.raises(runner.LocalGateError, match="SOURCE_TRACKED_BYTES_MISMATCH"):
            runner._validate_source_checkout(root, revision)


def test_checkout_identity_rejects_subdirectory_root(tmp_path: Path):
    _module, runner, root, inventory = _inventory(tmp_path)
    with pytest.raises(runner.LocalGateError, match="SOURCE_CHECKOUT_ROOT_MISMATCH"):
        runner._validate_source_checkout(root / "tests", inventory["sourceRevision"])


def test_checkout_identity_ignores_ambient_git_redirection(tmp_path: Path, monkeypatch):
    _module, runner, root, inventory = _inventory(tmp_path)
    monkeypatch.setenv("GIT_DIR", str(root / "not-the-repository"))
    monkeypatch.setenv("GIT_WORK_TREE", str(root / "tests"))
    monkeypatch.setenv("GIT_INDEX_FILE", str(root / "not-the-index"))
    runner._validate_source_checkout(root, inventory["sourceRevision"])


def test_checkout_identity_keeps_generated_build_plane_separate(tmp_path: Path):
    _module, runner, root, inventory = _inventory(tmp_path)
    generated = root / "build-system-j2/c4che"
    generated.mkdir(parents=True)
    (generated / "_cache.py").write_text("CXX = ['/usr/bin/c++']\n", encoding="utf-8")
    runner._validate_source_checkout(root, inventory["sourceRevision"])


@pytest.mark.parametrize("generated", [True, False])
def test_checkout_identity_recognizes_only_generated_waf_layout(tmp_path: Path, generated):
    _module, runner, root, _inventory_value = _inventory(tmp_path)
    (root / "waf").write_text("# tracked build tool fixture\n", encoding="utf-8")
    revision = _seal_fixture(root, "waf")
    name = ".waf3-2.0.24-" + "c" * 32 if generated else ".waf-unregistered"
    directory = root / name / "waflib"
    directory.mkdir(parents=True)
    (directory / "Build.py").write_text("# generated tool fixture\n", encoding="utf-8")
    (root / ".git/info/exclude").write_text(".waf*/\n", encoding="utf-8")
    if generated:
        runner._validate_source_checkout(root, revision)
    else:
        with pytest.raises(runner.LocalGateError, match="SOURCE_IGNORED_CODE"):
            runner._validate_source_checkout(root, revision)


@pytest.mark.parametrize("target", ["tracked", "external", "unsealed"])
def test_checkout_identity_binds_symbolic_link_target(tmp_path: Path, target):
    _module, runner, root, _inventory_value = _inventory(tmp_path / "checkout")
    if target == "external":
        destination = tmp_path / "external.py"
        destination.write_text("VALUE = 1\n", encoding="utf-8")
    elif target == "unsealed":
        destination = root / "build/generated.py"
        destination.write_text("VALUE = 1\n", encoding="utf-8")
    else:
        destination = root / "support.py"
    (root / "linked.py").symlink_to(destination)
    revision = _seal_fixture(root, "linked.py")
    if target == "tracked":
        runner._validate_source_checkout(root, revision)
    else:
        reason = "SOURCE_PATH_ESCAPES_CHECKOUT" if target == "external" else "SOURCE_LINK_TARGET_UNSEALED"
        with pytest.raises(runner.LocalGateError, match=reason):
            runner._validate_source_checkout(root, revision)


def test_local_gate_retains_children_but_rejects_source_changed_during_execution(tmp_path: Path):
    module, runner, root, inventory = _inventory(tmp_path)
    path = "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
    source = root / path
    source.write_text("from pathlib import Path\n"
                      "Path('support.py').write_text('VALUE = 2\\n')\n" + source.read_text(),
                      encoding="utf-8")
    for entry in inventory["entries"]:
        if entry["path"] == path:
            entry["artifactSha256"] = _file_digest(module, source)
    _rebind_revision(module, inventory, _seal_fixture(root))
    result = runner.run_local_gate(inventory, root=root,
                                   output_root=root / "evidence", environment={})
    assert result["status"] == "UNQUALIFIED"
    assert all(item["status"] == "PASS" for item in result["entries"])
    assert result["cleanup"] == "PASS"
    assert result["sourceIdentity"] == {
        "status": "FAIL", "reason": "SOURCE_TRACKED_BYTES_MISMATCH:support.py"}


@pytest.mark.parametrize("mutation", [None, "absent", "bytes", "head"])
def test_checkout_identity_binds_populated_submodule(tmp_path: Path, mutation):
    _module, runner, root, _inventory_value = _inventory(tmp_path)
    submodule = root / "deps/library"
    submodule.mkdir(parents=True)
    _git(submodule, "init", "-q")
    (submodule / "library.py").write_text("VALUE = 1\n", encoding="utf-8")
    _git(submodule, "add", "--", "library.py")
    def commit_submodule():
        _git(submodule, "-c", "user.name=Spec181 fixture", "-c",
             "user.email=spec181@example.invalid", "-c", "commit.gpgsign=false",
             "commit", "-qm", "Seal submodule fixture")
    commit_submodule()
    revision = _seal_fixture(root, "deps/library")
    if mutation == "absent":
        shutil.rmtree(submodule)
    elif mutation in {"bytes", "head"}:
        (submodule / "library.py").write_text("VALUE = 2\n", encoding="utf-8")
        if mutation == "head":
            _git(submodule, "add", "--", "library.py")
            commit_submodule()
    if mutation in {None, "absent"}:
        runner._validate_source_checkout(root, revision)
    else:
        reason = "SOURCE_REVISION_MISMATCH" if mutation == "head" else "SOURCE_TRACKED_BYTES_MISMATCH"
        with pytest.raises(runner.LocalGateError, match=reason):
            runner._validate_source_checkout(root, revision)


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
    _rebind_revision(_inventory_module, inventory, _seal_fixture(root))
    result = runner.run_local_gate(
        inventory, root=root, output_root=tmp_path / "evidence",
        environment={},
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
        environment={},
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
                              environment={})


def test_command_outside_inventory_source_is_rejected(tmp_path: Path):
    inventory_module, runner, root, inventory = _inventory(tmp_path)
    target = next(item for item in inventory["entries"] if item["id"] == "cpp-unit-suite")
    target["command"] = ["/bin/true"]
    target["commandDigest"] = _digest(target["command"])
    _refresh_inventory_digest(inventory_module, inventory)
    with pytest.raises(runner.LocalGateError, match="ENTRY_COMMAND_PATH_MISMATCH"):
        runner.run_local_gate(inventory, root=root,
                              output_root=tmp_path / "evidence",
                              environment={})


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


@pytest.mark.parametrize("explicit", [False, True])
def test_qwen_wrapper_accepts_runner_owned_output_directory(monkeypatch,
                                                             tmp_path: Path, explicit):
    wrapper = _load(QWEN_WRAPPER, "spec180_qwen_wrapper")
    monkeypatch.setenv("SPEC180_CASE_OUTPUT_DIR", str(tmp_path / "q-c"))
    argv = ["--case", "M01", "--seed", "1750001"]
    if explicit:
        argv += ["--output-dir", str(tmp_path / "explicit")]
    args = wrapper.build_parser().parse_args(argv)
    assert args.output_dir == str(tmp_path / ("explicit" if explicit else "q-c"))


def test_qwen_wrapper_rejects_missing_output_before_child_start(monkeypatch):
    wrapper = _load(QWEN_WRAPPER, "spec180_qwen_wrapper")
    monkeypatch.delenv("SPEC180_CASE_OUTPUT_DIR", raising=False)
    monkeypatch.setattr(sys, "argv", [str(QWEN_WRAPPER), "--case", "M01", "--seed", "1750001"])
    monkeypatch.setattr(wrapper.subprocess, "run", lambda *a, **kw: pytest.fail("child started"))
    with pytest.raises(SystemExit, match="--output-dir or SPEC180_CASE_OUTPUT_DIR is required"):
        wrapper.main()


@pytest.mark.parametrize("mutation,reason", [
    ("no-git", "SOURCE_CHECKOUT_UNAVAILABLE"),
    ("wrong-revision", "SOURCE_REVISION_MISMATCH"),
    ("dirty-revision", "SOURCE_REVISION_NOT_COMMIT"),
    ("modified", "SOURCE_TRACKED_BYTES_MISMATCH"),
    ("staged", "SOURCE_INDEX_MISMATCH"),
    ("assume-unchanged", "SOURCE_TRACKED_BYTES_MISMATCH"),
    ("untracked", "SOURCE_UNTRACKED_INPUT"),
    ("ignored-source", "SOURCE_IGNORED_CODE"),
    ("mode", "SOURCE_TRACKED_MODE_MISMATCH"),
])
def test_checkout_identity_rejects_before_qualification_children(
        tmp_path: Path, monkeypatch, mutation: str, reason: str):
    module, runner, root, inventory = _inventory(tmp_path)
    source = root / "support.py"
    if mutation == "no-git":
        shutil.rmtree(root / ".git")
    elif mutation == "wrong-revision":
        _rebind_revision(module, inventory, "b" * 40)
    elif mutation == "dirty-revision":
        _rebind_revision(module, inventory, inventory["sourceRevision"] + "-dirty")
    elif mutation in {"modified", "staged", "assume-unchanged"}:
        if mutation == "assume-unchanged":
            _git(root, "update-index", "--assume-unchanged", "support.py")
        source.write_text("VALUE = 2\n", encoding="utf-8")
        if mutation == "staged":
            _git(root, "add", "--", "support.py")
    elif mutation == "untracked":
        (root / "override.py").write_text("VALUE = 2\n", encoding="utf-8")
    elif mutation == "ignored-source":
        hidden = root / "tests/python/hidden.py"
        hidden.write_text("VALUE = 2\n", encoding="utf-8")
        (root / ".git/info/exclude").write_text("hidden.py\n", encoding="utf-8")
    elif mutation == "mode":
        source.chmod(0o755)
    output = root / "evidence"
    def unexpected_child(*args, **kwargs):
        pytest.fail("qualification child started before source identity rejection")
    monkeypatch.setattr(runner, "_run_entry", unexpected_child)
    with pytest.raises(runner.LocalGateError, match=reason):
        runner.run_local_gate(inventory, root=root, output_root=output, environment={})
    assert not output.exists()


@pytest.mark.parametrize("environment,reason", [
    ({"LD_LIBRARY_PATH": "/unsealed/runtime"}, "EFFECTIVE_CONFIG_DIGEST_MISMATCH"),
    ({"PYTHONPATH": "/unsealed/imports"}, "EFFECTIVE_CONFIG_DIGEST_MISMATCH"),
    ({"PATH": "/unsealed/tools"}, "EFFECTIVE_CONFIG_DIGEST_MISMATCH"),
    ({"SPEC180_CASE_OUTPUT_DIR": "/unsealed/output"}, "CONFIG_RESERVED_ENVIRONMENT"),
    ({}, "EFFECTIVE_CONFIG_DIGEST_MISMATCH"),
])
def test_local_gate_rejects_unbound_configuration_before_children(
        tmp_path: Path, monkeypatch, environment, reason):
    module, runner, root, inventory = _inventory(tmp_path)
    if not environment:
        inventory["effectiveConfigDigest"] = "sha256:" + "d" * 64
        for entry in inventory["entries"]:
            entry["effectiveConfigDigest"] = inventory["effectiveConfigDigest"]
        _refresh_inventory_digest(module, inventory)
    def unexpected_child(*args, **kwargs):
        pytest.fail("qualification child started with unbound configuration")
    monkeypatch.setattr(runner, "_run_entry", unexpected_child)
    with pytest.raises(runner.LocalGateError, match=reason):
        runner.run_local_gate(inventory, root=root,
                              output_root=root / "evidence", environment=environment)
    assert not (root / "evidence").exists()


def test_local_gate_binds_interpreter_bytes_before_children(tmp_path: Path, monkeypatch):
    interpreter = tmp_path / "build/python"
    interpreter.parent.mkdir(parents=True)
    shutil.copy2(Path(sys.executable).resolve(), interpreter)
    monkeypatch.setattr(sys, "executable", str(interpreter))
    _module, runner, root, inventory = _inventory(tmp_path)
    with interpreter.open("ab") as stream:
        stream.write(b"changed interpreter bytes")
    def unexpected_child(*args, **kwargs):
        pytest.fail("qualification child started with a changed interpreter")
    monkeypatch.setattr(runner, "_run_entry", unexpected_child)
    with pytest.raises(runner.LocalGateError, match="EFFECTIVE_CONFIG_DIGEST_MISMATCH"):
        runner.run_local_gate(inventory, root=root,
                              output_root=root / "evidence", environment={})
    assert not (root / "evidence").exists()


def test_local_gate_children_consume_frozen_explicit_environment(tmp_path: Path, monkeypatch):
    environment = {"SPEC181_FIXTURE_VALUE": "sealed"}
    monkeypatch.setenv("SPEC181_AMBIENT_ONLY", "must-not-be-inherited")
    module, runner, root, inventory = _inventory(tmp_path, environment)
    source = root / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
    source.write_text(
        "import os\n"
        "assert os.environ['SPEC181_FIXTURE_VALUE'] == 'sealed'\n"
        "assert 'SPEC181_AMBIENT_ONLY' not in os.environ\n"
        "assert 'SPEC181_LATE_VALUE' not in os.environ\n" + source.read_text(),
        encoding="utf-8")
    for entry in inventory["entries"]:
        if entry["kind"] == "minindn-case":
            entry["artifactSha256"] = _file_digest(module, source)
    _rebind_revision(module, inventory, _seal_fixture(root))
    original = runner._run_entry
    def mutate_caller_then_execute(*args, **kwargs):
        environment["SPEC181_LATE_VALUE"] = "unsealed"
        return original(*args, **kwargs)
    monkeypatch.setattr(runner, "_run_entry", mutate_caller_then_execute)
    result = runner.run_local_gate(inventory, root=root,
                                   output_root=root / "evidence", environment=environment)
    assert result["status"] == "PASS"
    assert all(entry["exitCode"] == 0 for entry in result["entries"])
    assert result["effectiveConfiguration"]["environmentDigest"] == _digest(
        {"SPEC181_FIXTURE_VALUE": "sealed"})


def test_local_gate_collects_children_when_interpreter_changes_during_run(tmp_path: Path, monkeypatch):
    interpreter = tmp_path / "build/python"
    interpreter.parent.mkdir(parents=True)
    executable = Path(sys.executable).resolve()
    interpreter.symlink_to(executable)
    replacement = interpreter.with_name("changed-python")
    shutil.copy2(executable, replacement)
    with replacement.open("ab") as stream:
        stream.write(b"changed interpreter bytes")
    monkeypatch.setattr(sys, "executable", str(interpreter))
    _module, runner, root, inventory = _inventory(tmp_path)
    original = runner._run_entry
    def execute_then_replace(root, entry, *args, **kwargs):
        result = original(root, entry, *args, **kwargs)
        if entry.get("case") == "Y-N":
            interpreter.unlink()
            interpreter.symlink_to(replacement)
        return result
    monkeypatch.setattr(runner, "_run_entry", execute_then_replace)
    result = runner.run_local_gate(inventory, root=root,
                                   output_root=root / "evidence", environment={})
    assert result["status"] == "UNQUALIFIED"
    assert result["configurationIdentity"] == {
        "status": "FAIL", "reason": "CONFIGURATION_CHANGED_DURING_RUN"}
    assert result["cleanup"] == "PASS"
    assert len(result["entries"]) == 6
    assert all(entry["status"] == "PASS" for entry in result["entries"])


def test_inventory_and_gate_clis_share_actual_configuration(tmp_path: Path, capsys):
    module, runner = _modules()
    root = _fixture_root(tmp_path / "source", module)
    integration = root / "build/integration-tests"
    integration.write_text(
        "#!/bin/sh\n[ \"$SPEC181_FIXTURE_VALUE\" = explicit-config-fixture ] || exit 91\n"
        "printf 'Suite*\\n    Test*\\n'\n", encoding="utf-8")
    environment = {"SPEC181_FIXTURE_VALUE": "explicit-config-fixture"}
    (root / "tests/python/test_spec180_fixture.py").write_text(
        "import os\nassert os.environ['SPEC181_FIXTURE_VALUE'] == 'explicit-config-fixture'\n"
        "def test_fixture():\n    assert True\n", encoding="utf-8")
    _seal_fixture(root)
    environment_path = tmp_path / "environment.json"
    environment_path.write_text(json.dumps(environment), encoding="utf-8")
    inventory_path = tmp_path / "inventory.json"
    assert module.main([
        "--root", str(root), "--candidate-id", "candidate-cli",
        "--candidate-digest", "sha256:" + "a" * 64,
        "--source-revision", _git(root, "rev-parse", "HEAD"),
        "--environment-json", str(environment_path), "--output", str(inventory_path),
    ]) == 0
    value = json.loads(inventory_path.read_text())
    assert value["effectiveConfigDigest"] == module.canonical_digest(
        module.local_launch_configuration(root, environment, value["timeoutSeconds"]))
    output = tmp_path / "evidence"
    assert runner.main([
        "--inventory", str(inventory_path), "--root", str(root),
        "--output-root", str(output), "--environment-json", str(environment_path),
    ]) == 0
    result = json.loads((output / "local-qualification.json").read_text())
    assert result["status"] == "PASS" and result["entryCount"] == 6
    assert result["effectiveConfigDigest"] == value["effectiveConfigDigest"]
    assert "explicit-config-fixture" not in capsys.readouterr().out
    environment_path.write_text(json.dumps({"SPEC181_FIXTURE_VALUE": "changed"}), encoding="utf-8")
    refused_output = tmp_path / "refused"
    assert runner.main([
        "--inventory", str(inventory_path), "--root", str(root),
        "--output-root", str(refused_output), "--environment-json", str(environment_path),
    ]) == 78
    assert not refused_output.exists()


@pytest.mark.parametrize("mutation", ["model", "referenced-key", "add-model", "map", "case-config",
                                      "checkpoint", "registry-public-key"])
def test_local_gate_rejects_external_input_drift_before_children(tmp_path, monkeypatch, mutation):
    external = tmp_path / "inputs"
    package = external / "package"
    package.mkdir(parents=True)
    model = package / "model.onnx"
    model.write_bytes(b"frozen model")
    key = external / "recipient.key"
    key.write_bytes(b"fixture private bytes")
    key_map = external / "recipients.json"
    key_map.write_text(json.dumps({"provider": str(key)}))
    checkpoint = external / "checkpoint.pt"
    checkpoint.write_bytes(b"frozen checkpoint")
    public_key = external / "catalogue.pub"
    public_key.write_bytes(b"registered public fixture")
    registry = external / "registry.json"
    registry.write_text(json.dumps({"catalogue": {"publicKeyPath": "catalogue.pub"}}))
    environment = {"SPEC180_YOLO_CANONICAL_PACKAGE": str(package),
                   "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(key_map),
                   "SPEC180_YOLO_CHECKPOINT": str(checkpoint),
                   "SPEC180_YOLO_CATALOGUE_REGISTRY": str(registry)}
    for case in ("A", "B", "N"):
        config = external / (case + ".json")
        config.write_text(json.dumps({"case": case}))
        environment["SPEC181_LOCAL_CONFIG_Y_" + case] = str(config)
    _, runner, root, inventory = _inventory(tmp_path / "source", environment)
    if mutation == "model":
        model.write_bytes(b"changed model")
    elif mutation == "referenced-key":
        key.write_bytes(b"changed private bytes")
    elif mutation == "add-model":
        (package / "external.data").write_bytes(b"new external initializer")
    elif mutation == "map":
        key_map.write_text(json.dumps({"other-provider": str(key)}))
    elif mutation == "checkpoint":
        checkpoint.write_bytes(b"changed checkpoint")
    elif mutation == "registry-public-key":
        public_key.write_bytes(b"changed public fixture")
    else:
        Path(environment["SPEC181_LOCAL_CONFIG_Y_B"]).write_text('{"roles": []}')
    monkeypatch.setattr(runner, "_run_entry", lambda *a: pytest.fail("child started with changed inputs"))
    output = tmp_path / "evidence"
    with pytest.raises(runner.LocalGateError, match="INPUT_IDENTITY_MISMATCH"):
        runner.run_local_gate(inventory, root=root, output_root=output, environment=environment)
    assert not output.exists()


@pytest.mark.parametrize("case", ["Y-A", "Y-B", "Y-N"])
def test_supervised_case_receives_its_own_configuration(tmp_path, case):
    # This tests the production process supervisor, not MiniNDN semantics.
    module, runner = _modules()
    environment = {}
    for name in ("Y-A", "Y-B", "Y-N"):
        config = tmp_path / (name + ".json")
        config.write_text(json.dumps({"case": name}))
        environment["SPEC181_LOCAL_CONFIG_" + name.replace("-", "_")] = str(config)
    module.local_launch_configuration(tmp_path, environment, 10)
    script = tmp_path / "observe.py"
    script.write_text(
        "import json, os, pathlib, sys\n"
        "case = sys.argv[1]\n"
        "value = json.loads(pathlib.Path(os.environ['SPEC180_YOLO_CONFIG']).read_text())\n"
        "assert value['case'] == case, 'wrong case configuration'\n"
        "print('OBSERVED_CONFIG=' + value['case'])\n"
        "print('SPEC180_CASE_RESULT status=PASS case=' + case)\n")
    command = [sys.executable, str(script), case]
    entry = {"id": "case-" + case, "kind": "minindn-case", "case": case,
             "command": command, "commandDigest": module.canonical_digest(command),
             "timeoutSeconds": 10}
    result = runner._run_entry(tmp_path, entry, tmp_path / "evidence", environment)
    assert result["status"] == "PASS"
    assert result["exitCode"] == 0 and result["cleanup"] == "PASS"
    assert "OBSERVED_CONFIG=" + case in Path(result["stdoutPath"]).read_text()
    assert "SPEC180_YOLO_CONFIG" not in environment


@pytest.mark.parametrize("phase,completed", [("before-cases", 3), ("last-case", 6)])
def test_input_drift_preserves_completed_children_and_blocks_qualification(tmp_path, monkeypatch, phase, completed):
    key = tmp_path / "requester.key"
    key.write_bytes(b"frozen fixture bytes")
    environment = {"NDNSF_DI_ENVELOPE_KEY_FILE": str(key)}
    _, runner, root, inventory = _inventory(tmp_path / "source", environment)
    original = runner._run_entry
    def execute_then_mutate(root, entry, *args):
        result = original(root, entry, *args)
        if ((phase == "before-cases" and entry["kind"] == "python-selector") or
                (phase == "last-case" and entry.get("case") == "Y-N")):
            key.write_bytes(b"changed after child")
        return result
    monkeypatch.setattr(runner, "_run_entry", execute_then_mutate)
    result = runner.run_local_gate(inventory, root=root, output_root=tmp_path / "evidence",
                                   environment=environment)
    assert result["status"] == "UNQUALIFIED"
    assert result["inputIdentity"]["status"] == "FAIL"
    assert result["entryCount"] == completed
    assert len(result["unexecutedEntryIds"]) == 6 - completed
    assert result["cleanup"] == "PASS"
    assert all(item["status"] == "PASS" and Path(item["stdoutPath"]).is_file()
               for item in result["entries"])
