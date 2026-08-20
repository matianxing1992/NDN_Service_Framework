from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ndnsf-di"))
from collect_spec170_candidate_inputs import (  # noqa: E402
    build_inventory,
)


def _git_fixture(root: Path) -> None:
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.name", "Spec170 Test"],
                   cwd=root, check=True)
    subprocess.run(["git", "config", "user.email", "spec170@example.invalid"],
                   cwd=root, check=True)


def test_inventory_is_deterministic_and_excludes_generated_outputs(tmp_path):
    source = tmp_path / "src"
    source.mkdir()
    (source / "keep.py").write_text("value = 1\n", encoding="utf-8")
    executable = source / "run.sh"
    executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    executable.chmod(0o755)
    (source / "build").mkdir()
    (source / "build" / "stale.so").write_bytes(b"old")
    (source / "__pycache__").mkdir()
    (source / "__pycache__" / "keep.pyc").write_bytes(b"generated")
    _git_fixture(tmp_path)
    subprocess.run(["git", "add", "."], cwd=tmp_path, check=True)
    subprocess.run(["git", "commit", "-qm", "fixture"],
                   cwd=tmp_path, check=True)

    first = build_inventory(tmp_path, ("src",))
    second = build_inventory(tmp_path, ("src",))
    assert first["schemaVersion"] == "spec170-candidate-input-inventory-v1"
    assert first["inventoryDigest"] == second["inventoryDigest"]
    assert [row["path"] for row in first["files"]] == [
        "src/keep.py", "src/run.sh"
    ]
    assert first["files"][1]["executable"] is True

    (source / "keep.py").write_text("value = 2\n", encoding="utf-8")
    changed = build_inventory(tmp_path, ("src",))
    assert changed["inventoryDigest"] != first["inventoryDigest"]
    assert changed["worktreeStatusRows"] == 1


def test_inventory_fails_closed_for_missing_input_root(tmp_path):
    _git_fixture(tmp_path)
    try:
        build_inventory(tmp_path, ("required",))
    except SystemExit as error:
        assert str(error) == "SPEC170_CANDIDATE_INPUT_MISSING:required"
    else:
        raise AssertionError("missing candidate input root was accepted")
