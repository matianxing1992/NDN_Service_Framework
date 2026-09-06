from __future__ import annotations

import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/spec180_candidate.py"


def load_candidate():
    spec = importlib.util.spec_from_file_location("spec180_candidate", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def digest(module, letter):
    return "sha256:" + letter * 64


def candidate_fixture(module, *, source_dirty=False):
    planes = {
        name: {"revision": name + "-r1", "digest": digest(module, format(i + 1, "x"))}
        for i, name in enumerate(module.REQUIRED_PLANES)
    }
    source_delta = {
        "dirty": source_dirty,
        "paths": ["src/changed.cpp"] if source_dirty else [],
    }
    return module.CandidateRecord("candidate-1", planes, source_delta)


def test_canonical_identity_is_stable_and_binds_all_planes():
    module = load_candidate()
    candidate = candidate_fixture(module, source_dirty=True)
    reordered = module.CandidateRecord(
        "candidate-1",
        dict(reversed(list(candidate.planes.items()))),
        {"paths": ["src/changed.cpp"], "dirty": True},
    )
    assert candidate.digest.startswith("sha256:")
    assert candidate.digest == reordered.digest
    assert set(candidate.to_dict()["planes"]) == set(module.REQUIRED_PLANES)


def test_dirty_source_bytes_must_be_explicit():
    module = load_candidate()
    planes = {
        name: {"digest": digest(module, "a")}
        for name in module.REQUIRED_PLANES
    }
    with pytest.raises(module.CandidateIdentityError, match="modified/untracked"):
        module.CandidateRecord("candidate-1", planes, {"dirty": True, "paths": []})


def test_missing_or_empty_plane_fails_closed():
    module = load_candidate()
    planes = {
        name: {"digest": digest(module, "a")}
        for name in module.REQUIRED_PLANES
    }
    del planes["sif"]
    with pytest.raises(module.CandidateIdentityError, match="missing candidate planes"):
        module.CandidateRecord("candidate-1", planes, {"dirty": False, "paths": []})


def test_cross_candidate_evidence_is_rejected():
    module = load_candidate()
    first = candidate_fixture(module)
    second = module.CandidateRecord(
        "candidate-2",
        dict(first.planes, runtime={"revision": "runtime-r2", "digest": digest(module, "f")}),
        first.source_delta,
    )
    with pytest.raises(module.CandidateIdentityError, match="does not match"):
        module.validate_evidence_for_candidate(
            second,
            {"candidateId": first.candidate_id, "candidateDigest": first.digest},
        )
    module.validate_evidence_for_candidate(
        second,
        {"candidateId": second.candidate_id, "candidateDigest": second.digest},
    )


def test_invalidation_names_earliest_restart_gate():
    module = load_candidate()
    first = candidate_fixture(module)
    changed = dict(first.planes)
    changed["sif"] = {"revision": "sif-r2", "digest": digest(module, "f")}
    second = module.CandidateRecord(first.candidate_id, changed, first.source_delta)
    result = module.invalidation_for_change(first, second)
    assert result["changedPlanes"] == ["sif"]
    assert result["earliestRestart"] == "SIF_BUILD"
    assert result["candidateChanged"] is True


def test_unmodified_candidate_has_no_invalidation():
    module = load_candidate()
    candidate = candidate_fixture(module)
    result = module.invalidation_for_change(candidate, candidate)
    assert result == {
        "changedPlanes": [],
        "earliestRestart": None,
        "candidateChanged": False,
    }


def _write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")


def _reference(path):
    raw = path.read_bytes()
    return {"path": str(path), "sha256": "sha256:" + hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


@pytest.fixture
def local_delivery(tmp_path):
    # Exercise real Git/source checks and the production process supervisor.
    # Its tiny command fixtures establish tool integrity, not MiniNDN behavior.
    from test_spec180_local_gate import _inventory
    module = load_candidate()
    external = tmp_path / "inputs"
    external.mkdir()
    paths = {}
    for group in module.LOCAL_INPUT_GROUPS:
        path = external / (group + ".txt")
        path.write_text("controlled delivery fixture: " + group)
        paths[group] = path
    checkpoint = paths["models"]
    registry = external / "registry.json"
    _write_json(registry, {"catalogue": {"publicKeyPath": paths["publicKeys"].name}})
    paths["registries"] = registry
    environment = {"SPEC180_YOLO_CHECKPOINT": str(checkpoint),
                   "SPEC180_YOLO_CATALOGUE_REGISTRY": str(registry)}
    inventory_module, gate, root, inventory = _inventory(tmp_path / "source", environment)
    reviewed_at = time.time()
    output = tmp_path / "qualification"
    result = gate.run_local_gate(inventory, root=root, output_root=output, environment=environment)
    assert result["status"] == "PASS"
    qualification = output / "local-qualification.json"
    _write_json(qualification, result)
    audit = external / "audit.json"
    _write_json(audit, {
        "schema": "spec181-convergence-verdict-v1", "verdict": "PASS",
        "sourceRevision": inventory["sourceRevision"], "reviewedAtUnix": reviewed_at,
        "inputDigest": inventory["inputDigest"],
        "effectiveConfigDigest": inventory["effectiveConfigDigest"],
        "evidence": [_reference(paths["contracts"])],
    })
    recipe = {
        "sourceRevision": inventory["sourceRevision"],
        "inputs": {group: [str(path)] for group, path in paths.items()},
        "localEnvironment": {"nativeArtifacts": [str(root / "build/integration-tests")]},
        "validation": {"audit": str(audit), "inventory": str(output / "inventory.json"),
                       "qualification": str(qualification)},
        "reproduction": str(paths["runners"]), "limitations": str(paths["contracts"]),
        "experimentTransfer": {"owner": "experiment-machine", "T010": "TRANSFERRED",
                               "T011": "TRANSFERRED", "contract": str(paths["contracts"])},
    }
    return module, root, environment, recipe


def test_local_delivery_seals_and_verifies_without_sif(local_delivery):
    module, root, environment, recipe = local_delivery
    record = module.seal_development_delivery(recipe, root=root, environment=environment)
    assert record["schema"] == "spec181-development-delivery-v1"
    assert record["handoffStatus"] == "READY_FOR_EXPERIMENT_MACHINE"
    assert record["experimentTransfer"]["T010"] == "TRANSFERRED"
    assert record["candidateDigest"] != record["deliveryDigest"]
    body = dict(record)
    expected = body.pop("deliveryDigest")
    assert expected == "sha256:" + hashlib.sha256(json.dumps(
        body, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    assert len(record["validation"]["logs"]) == 12
    assert "sif" not in record and "planes" not in record
    module.verify_development_delivery(record, root=root, environment=environment)


@pytest.mark.parametrize("group", ["models", "publicKeys", "oracle"])
def test_local_delivery_rejects_changed_external_file(local_delivery, group):
    module, root, environment, recipe = local_delivery
    record = module.seal_development_delivery(recipe, root=root, environment=environment)
    Path(recipe["inputs"][group][0]).write_text("changed input")
    with pytest.raises(ValueError, match="DELIVERY_REFERENCE_MISMATCH"):
        module.verify_development_delivery(record, root=root, environment=environment)


@pytest.mark.parametrize("kind", ["tracked", "untracked", "new-commit"])
def test_local_delivery_rejects_source_changes(local_delivery, kind):
    from test_spec180_local_gate import _git
    module, root, environment, recipe = local_delivery
    record = module.seal_development_delivery(recipe, root=root, environment=environment)
    (root / ("support.py" if kind != "untracked" else "new-input.py")).write_text("CHANGED = 1\n")
    if kind == "new-commit":
        _git(root, "add", "support.py")
        _git(root, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
             "-c", "commit.gpgsign=false", "commit", "-qm", "Change tested source")
    with pytest.raises(ValueError, match="SOURCE_"):
        module.verify_development_delivery(record, root=root, environment=environment)


def test_local_delivery_rejects_changed_configuration(local_delivery):
    module, root, environment, recipe = local_delivery
    record = module.seal_development_delivery(recipe, root=root, environment=environment)
    with pytest.raises(ValueError, match="DELIVERY_CONFIGURATION_MISMATCH"):
        module.verify_development_delivery(record, root=root,
                                           environment=dict(environment, NEW_VALUE="different"))


@pytest.mark.parametrize("mutation,reason", [
    ("source", "IDENTITY_MISMATCH"), ("candidate", "IDENTITY_MISMATCH"),
    ("missing-entry", "ENTRIES_INCOMPLETE"), ("entry-count", "ENTRIES_INCOMPLETE"),
    ("exit", "ENTRY_NOT_PASS"), ("signal", "ENTRY_NOT_PASS"), ("timeout", "ENTRY_NOT_PASS"),
    ("cleanup", "ENTRY_NOT_PASS"), ("redaction", "ENTRY_NOT_PASS"),
    ("command", "ENTRY_MISMATCH"), ("environment", "CHILD_ENVIRONMENT_MISMATCH"),
    ("unexecuted", "QUALIFICATION_INCOMPLETE"), ("failed", "QUALIFICATION_INCOMPLETE"),
    ("log-bytes", "LOG_DIGEST_MISMATCH"), ("oracle", "CASE_ORACLE_MISSING"),
])
def test_local_delivery_rejects_false_pass_evidence(local_delivery, mutation, reason):
    module, root, environment, recipe = local_delivery
    path = Path(recipe["validation"]["qualification"])
    result = json.loads(path.read_text())
    entry = result["entries"][0]
    if mutation == "source": result["sourceRevision"] = "b" * 40
    elif mutation == "candidate": result["candidateDigest"] = "sha256:" + "b" * 64
    elif mutation == "missing-entry": result["entries"].pop()
    elif mutation == "entry-count": result["entryCount"] -= 1
    elif mutation == "exit": entry["exitCode"] = 2
    elif mutation == "signal": entry["signal"] = 11
    elif mutation == "timeout": entry["timedOut"] = True
    elif mutation in ("cleanup", "redaction"): entry[mutation] = "FAIL"
    elif mutation == "command": entry["command"] = ["true"]
    elif mutation == "environment": entry["environmentDigest"] = "sha256:" + "b" * 64
    elif mutation == "unexecuted": result["unexecutedEntryIds"] = [entry["id"]]
    elif mutation == "failed": result["failedEntryIds"] = [entry["id"]]
    elif mutation == "log-bytes": Path(entry["stdoutPath"]).write_text("changed")
    else:
        entry = next(row for row in result["entries"] if row["kind"] == "minindn-case")
        Path(entry["stdoutPath"]).write_text("exit zero without registered marker")
        entry["stdoutSha256"] = _reference(Path(entry["stdoutPath"]))["sha256"]
    _write_json(path, result)
    with pytest.raises(ValueError, match=reason):
        module.seal_development_delivery(recipe, root=root, environment=environment)


@pytest.mark.parametrize("mutation,reason", [
    ("blocked", "AUDIT_NOT_PASS"), ("source", "AUDIT_IDENTITY_MISMATCH"),
    ("input", "AUDIT_IDENTITY_MISMATCH"), ("late", "AUDIT_AFTER_QUALIFICATION"),
    ("evidence", "AUDIT_EVIDENCE_MISSING"),
])
def test_local_delivery_rejects_missing_or_stale_audit(local_delivery, mutation, reason):
    module, root, environment, recipe = local_delivery
    path = Path(recipe["validation"]["audit"])
    audit = json.loads(path.read_text())
    if mutation == "blocked": audit["verdict"] = "BLOCK"
    elif mutation == "source": audit["sourceRevision"] = "b" * 40
    elif mutation == "input": audit["inputDigest"] = "sha256:" + "b" * 64
    elif mutation == "late": audit["reviewedAtUnix"] = time.time() + 60
    else: audit["evidence"] = []
    _write_json(path, audit)
    with pytest.raises(ValueError, match=reason):
        module.seal_development_delivery(recipe, root=root, environment=environment)


@pytest.mark.parametrize("mutation,reason", [
    ("field", "RECIPE_FIELDS_INVALID"), ("group", "INPUT_GROUPS_INVALID"),
    ("empty", "REFERENCE_LIST_EMPTY"), ("remote-pass", "TRANSFER_INVALID"),
])
def test_local_delivery_requires_complete_local_recipe(local_delivery, mutation, reason):
    module, root, environment, recipe = local_delivery
    if mutation == "field": del recipe["reproduction"]
    elif mutation == "group": del recipe["inputs"]["models"]
    elif mutation == "empty": recipe["inputs"]["models"] = []
    else: recipe["experimentTransfer"]["T010"] = "PASS"
    with pytest.raises(ValueError, match=reason):
        module.seal_development_delivery(recipe, root=root, environment=environment)


def test_local_delivery_rejects_recomputed_outer_digest(local_delivery):
    module, root, environment, recipe = local_delivery
    record = module.seal_development_delivery(recipe, root=root, environment=environment)
    record["handoffStatus"] = "LOCAL_DEVELOPMENT_PASS"
    body = dict(record)
    body.pop("deliveryDigest")
    record["deliveryDigest"] = "sha256:" + hashlib.sha256(json.dumps(
        body, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    with pytest.raises(ValueError, match="DELIVERY_RECORD_MISMATCH"):
        module.verify_development_delivery(record, root=root, environment=environment)


def test_local_delivery_cli_is_exclusive_and_verification_does_not_write(local_delivery, tmp_path):
    module, root, environment, recipe = local_delivery
    recipe_path, env_path, output = (tmp_path / name for name in ("recipe.json", "env.json", "delivery.json"))
    _write_json(recipe_path, recipe)
    _write_json(env_path, environment)
    args = ["--root", str(root), "--environment-json", str(env_path)]
    assert module.main(["seal", *args, "--input", str(recipe_path), "--output", str(output)]) == 0
    original = output.read_bytes()
    verified = subprocess.run([sys.executable, str(SCRIPT), "verify", *args,
                               "--input", str(output)], capture_output=True, text=True)
    assert verified.returncode == 0, verified.stderr
    assert "SPEC181_DEVELOPMENT_DELIVERY_OK" in verified.stdout
    assert output.read_bytes() == original
    assert module.main(["seal", *args, "--input", str(recipe_path), "--output", str(output)]) == 78
    assert output.read_bytes() == original
    assert module.main(["seal", *args, "--input", str(recipe_path),
                        "--output", str(root / "evidence/seal.json")]) == 78
    assert not (root / "evidence/seal.json").exists()
    assert list(tmp_path.glob(".delivery-*")) == []


def test_local_delivery_cli_write_failure_leaves_no_partial_seal(local_delivery, tmp_path, monkeypatch):
    module, root, environment, recipe = local_delivery
    recipe_path, env_path, output = (tmp_path / name for name in ("recipe.json", "env.json", "delivery.json"))
    _write_json(recipe_path, recipe)
    _write_json(env_path, environment)
    def failed_sync(_descriptor):
        raise OSError("injected output flush failure")
    monkeypatch.setattr(module.os, "fsync", failed_sync)
    assert module.main(["seal", "--root", str(root), "--input", str(recipe_path),
                        "--environment-json", str(env_path), "--output", str(output)]) == 78
    assert not output.exists()
    assert list(tmp_path.glob(".delivery-*")) == []
