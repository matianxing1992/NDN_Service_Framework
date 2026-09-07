"""Run the operator CLI itself; synthetic artifacts never qualify a model."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

from test_yolo_operator_profile import profile_fixture
from test_yolo_closure import input_plane

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "jobs/yolo/submit.py"


def file_ref(path):
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()}


def input_profile(tmp_path):
    path, value = profile_fixture(tmp_path)
    plane, _ = input_plane(tmp_path / "inputs")
    value["release"]["inputs"] = file_ref(plane)
    path.write_text(json.dumps(value))
    return path, value


def cli(*args, cwd):
    return subprocess.run([sys.executable, "-B", str(CLI), *map(str, args)],
                          cwd=cwd, capture_output=True, text=True, timeout=10)


def test_cli_check_integrity_never_qualifies_synthetic_release(tmp_path):
    path, _ = input_profile(tmp_path)
    result = cli("check", "--stage", "inputs", "--profile", path, cwd=tmp_path)
    assert result.returncode == 78, result.stderr
    report = json.loads(result.stdout)
    assert report["integrity"] == "VERIFIED"
    assert report["qualification"] == "NOT_EVALUATED"
    assert report["status"] == "INCOMPLETE"
    assert report["pending"]
    assert not (tmp_path / "cache").exists()


@pytest.mark.parametrize("mutation,reason", [
    ("manifest-hash", "FILE_DIGEST"), ("payload", "FILE_DIGEST"),
    ("unknown", "PROFILE_SCHEMA"), ("future-stage", "PROFILE_STAGE_ARTIFACTS"),
])
def test_cli_rejects_before_any_external_command_or_write(tmp_path, mutation, reason):
    path, value = input_profile(tmp_path)
    if mutation == "manifest-hash":
        value["release"]["inputs"]["sha256"] = "sha256:" + "0" * 64
    elif mutation == "payload":
        file = tmp_path / "inputs/baseSif"
        file.write_bytes(b"X" * file.stat().st_size)
    elif mutation == "unknown":
        value["force"] = True
    path.write_text(json.dumps(value))
    before = {p.relative_to(tmp_path): p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()}
    result = cli("check", "--profile", path, "--stage",
                 "dispatch" if mutation == "future-stage" else "inputs", cwd=tmp_path)
    assert result.returncode == 2, result.stderr
    assert json.loads(result.stdout)["reason"].startswith(reason)
    after = {p.relative_to(tmp_path): p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()}
    assert before == after


def test_no_execution_command_is_exposed_before_wiring(tmp_path):
    for action in ("submit", "local", "prepare", "collect"):
        result = cli(action, cwd=tmp_path)
        assert result.returncode == 2
        assert "invalid choice" in result.stderr


def test_deterministic_run_plan_has_four_provider_roles_and_four_requests(tmp_path):
    path, _ = input_profile(tmp_path)
    result = cli("check", "--stage", "inputs", "--profile", path,
                 "--run-id", "test-normal-01", "--output", tmp_path / "results",
                 "--case", "two-node-gpu", cwd=tmp_path)
    assert result.returncode == 78, result.stderr
    plan = json.loads(result.stdout)["runPlan"]
    assert plan["status"] == "PLANNED"
    assert plan["allocation"] is None
    assert len(plan["requests"]) == 4
    assert [r["warmup"] for r in plan["requests"]] == [True, False, False, False]
    assert len({r["requestId"] for r in plan["requests"]}) == 4
    assert len(plan["nodes"]) == 2
    assert plan["nodes"][0]["providerRoles"] == {"BackboneNeck": "cuda:0", "Merge": "cpu"}
    assert plan["nodes"][1]["providerRoles"] == {"DetectShard0": "cuda:0", "DetectShard1": "cuda:0"}
    assert not (tmp_path / "results").exists()


@pytest.mark.parametrize("case,nodes,requests,gpu", [
    ("local-cpu", 1, 2, False), ("single-node-gpu", 1, 2, True),
    ("two-node-gpu", 2, 4, True), ("negative-dependency", 2, 1, True),
])
def test_registered_case_layout_and_schedule(tmp_path, case, nodes, requests, gpu):
    path, _ = input_profile(tmp_path)
    result = cli("check", "--stage", "inputs", "--profile", path,
                 "--run-id", "test-case-01", "--output", "results", "--case", case, cwd=tmp_path)
    assert result.returncode == 78, result.stderr
    plan = json.loads(result.stdout)["runPlan"]
    assert len(plan["nodes"]) == nodes
    assert len(plan["requests"]) == requests
    roles = {k: v for n in plan["nodes"] for k, v in n["providerRoles"].items()}
    assert len(roles) == 4
    assert roles["Merge"] == "cpu"
    assert all(v == ("cuda:0" if gpu else "cpu") for k, v in roles.items() if k != "Merge")
    assert len(set(plan["identities"].values())) == len(plan["identities"])
    assert plan["output"] == str(tmp_path / "results/test-case-01")


def test_case_digest_ignores_run_and_location_but_binds_behavior(tmp_path):
    path, value = input_profile(tmp_path)

    def plan(run, cwd):
        result = cli("check", "--stage", "inputs", "--profile", path,
                     "--run-id", run, "--output", "results", "--case", "two-node-gpu", cwd=cwd)
        assert result.returncode == 78, result.stderr
        return json.loads(result.stdout)["runPlan"]

    first = plan("test-first", tmp_path)
    assert first == plan("test-first", tmp_path.parent)
    second = plan("test-second", tmp_path)
    assert first["caseBehaviorDigest"] == second["caseBehaviorDigest"]
    assert set(first["identities"].values()).isdisjoint(second["identities"].values())
    assert {r["requestId"] for r in first["requests"]}.isdisjoint(r["requestId"] for r in second["requests"])
    value["storage"]["sharedRunRoot"] = "/another/project/runs"
    value["runtime"]["apptainer"] = "/another/bin/apptainer"
    path.write_text(json.dumps(value))
    assert first["caseBehaviorDigest"] == plan("test-first", tmp_path)["caseBehaviorDigest"]
    value["timing"]["progressTimeoutSeconds"] = 29
    path.write_text(json.dumps(value))
    assert first["caseBehaviorDigest"] != plan("test-first", tmp_path)["caseBehaviorDigest"]


@pytest.mark.parametrize("extra,reason", [
    (["--run-id", "test-normal"], "RUN_PREVIEW_OPTIONS"),
    (["--run-id", "../escape", "--output", "results", "--case", "two-node-gpu"], "RUN_ID"),
    (["--run-id", "test-normal", "--output", "/", "--case", "two-node-gpu"], "RUN_OUTPUT_ROOT"),
])
def test_invalid_preview_rejected(tmp_path, extra, reason):
    path, _ = input_profile(tmp_path)
    result = cli("check", "--stage", "inputs", "--profile", path, *extra, cwd=tmp_path)
    assert result.returncode == 2, result.stderr
    assert json.loads(result.stdout)["reason"] == reason


def test_check_audited_no_launch_network_or_filesystem_mutations(tmp_path):
    path, _ = input_profile(tmp_path)
    # Audit the real CLI main in a fresh interpreter, including all imports.
    # Python3.8 uuid (imported by jsonschema) uses the read-only `uname -p`
    # helper. Permit only that exact command; no experiment command is allowed.
    wrapper = """
import os, runpy, sys
def audit(event, args):
    if event == 'subprocess.Popen' and args[0] == 'uname' and tuple(args[1]) == ('uname', '-p'):
        return
    if (event.startswith(('subprocess.', 'os.exec', 'os.spawn', 'os.posix_spawn'))
        or event in ('os.system', 'socket.connect', 'os.mkdir', 'os.remove', 'os.rename', 'os.chmod', 'os.link', 'os.symlink')):
        raise AssertionError('SIDE_EFFECT:' + event)
    if event == 'open':
        flags = args[2]
        if args[0] != '/dev/null' and isinstance(flags, int) and flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_APPEND):
            raise AssertionError('WRITE_OPEN')
sys.addaudithook(audit)
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name='__main__')
"""
    result = subprocess.run([sys.executable, "-B", "-c", wrapper, str(CLI), "check",
                             "--profile", str(path), "--stage", "inputs"],
                            cwd=tmp_path, capture_output=True, text=True, timeout=10)
    assert result.returncode == 78, result.stderr
    assert json.loads(result.stdout)["integrity"] == "VERIFIED"
