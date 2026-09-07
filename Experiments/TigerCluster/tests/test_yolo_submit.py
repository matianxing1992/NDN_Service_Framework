"""Run the operator CLI itself; synthetic artifacts never qualify a model."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

from test_yolo_operator_profile import profile_fixture
from test_yolo_closure import input_plane

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "jobs/yolo/submit.py"


def submit_module():
    spec = importlib.util.spec_from_file_location("spec183_yolo_submit", CLI)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


def test_contract_commands_are_exposed_but_require_explicit_binding(tmp_path):
    # The public commands exist now, but argparse must not invent defaults for
    # a profile/run/output/case.  This keeps an incomplete candidate from
    # reaching any filesystem, Apptainer, or Slurm boundary.
    for action in ("submit", "local", "prepare"):
        result = cli(action, cwd=tmp_path)
        assert result.returncode == 2
        assert "required" in result.stderr
    result = cli("collect", cwd=tmp_path)
    assert result.returncode == 2
    assert "required" in result.stderr


def test_private_slurm_runner_requires_allocation(tmp_path):
    result = cli("run", "--profile", tmp_path / "profile.json", "--run-id",
                 "test-run", "--output", tmp_path / "results", "--case", "local-cpu",
                 cwd=tmp_path)
    assert result.returncode == 2
    assert json.loads(result.stdout)["reason"] == "ALLOCATION_REQUIRED"


@pytest.mark.parametrize("case,gate,nodes", [
    ("single-node-gpu", "localSif", 1),
    ("two-node-gpu", "singleNodeGpu", 2),
    ("negative-dependency", "twoNodeGpu", 2),
])
def test_submission_prerequisites_and_allocation_contract(tmp_path, monkeypatch, case, gate, nodes):
    module = submit_module()
    path, profile = profile_fixture(tmp_path)
    bundle = tmp_path / "frozen"
    wrapper = bundle / "jobs/yolo/run.sbatch"
    wrapper.parent.mkdir(parents=True)
    wrapper.write_bytes((ROOT / "jobs/yolo/run.sbatch").read_bytes())
    wrapper.chmod(0o555)
    prepared = {"case": case, "bundle": str(bundle),
                "candidateDigest": "sha256:" + "a" * 64,
                "profileDigest": "sha256:" + "b" * 64}
    args = SimpleNamespace(profile=path, run_id="test-submit", case=case,
                           output=tmp_path / "output")
    command = module._submission_command(path, profile, args, prepared, "spec183-test")
    assert "--time=00:15:00" in command
    assert "--nodes=" + str(nodes) in command
    assert "--ntasks=" + str(nodes) in command
    assert "--gres=gpu:rtx_6000:1" in command
    assert command[-6] == str(wrapper)
    seen = []
    monkeypatch.setattr(module, "_dispatch_report",
                        lambda _: ({"qualification": "READY",
                                    "documentDigest": prepared["profileDigest"]}, profile))
    monkeypatch.setattr(module, "_load_prepared", lambda *_: prepared)
    monkeypatch.setattr(module, "_gate_receipt",
                        lambda path, profile, name: seen.append(name))
    def forbidden(*args, **kwargs):
        pytest.fail("unwired staging must not launch Slurm")
    monkeypatch.setattr(subprocess, "run", forbidden)
    assert module._submit(args) == module.INCOMPLETE
    assert seen == [gate]
    assert not args.output.exists()


def test_submit_rejects_local_case_before_reading_or_launching(tmp_path):
    module = submit_module()
    with pytest.raises(module.ClosureError, match="SUBMIT_CASE"):
        module._submit(SimpleNamespace(case="local-cpu"))


def test_local_sif_first_run_requires_host_gate_not_its_own_result(tmp_path, monkeypatch):
    module = submit_module()
    monkeypatch.setattr(module, "_dispatch_report", lambda _: ({"qualification": "READY"}, {}))
    monkeypatch.setattr(module, "_load_prepared", lambda *_: {
        "case": "local-cpu", "candidateDigest": "sha256:" + "a" * 64})
    seen = []
    monkeypatch.setattr(module, "_gate_receipt", lambda path, profile, gate: seen.append(gate))
    args = SimpleNamespace(profile=tmp_path / "profile", output=tmp_path / "output",
                           run_id="test-local", case="local-cpu")
    assert module._local(args) == module.INCOMPLETE
    assert seen == ["hostMinindn"]
    assert not args.output.exists()


@pytest.mark.parametrize("mutation,reason", [
    ("case", "SUBMIT_CASE"), ("profile", "PROFILE_CHANGED_AFTER_PREPARE"),
])
def test_submit_rejects_changed_preparation_before_receipts(tmp_path, monkeypatch, mutation, reason):
    module = submit_module()
    prepared = {"case": "two-node-gpu", "profileDigest": "sha256:" + "a" * 64}
    if mutation == "case":
        prepared["case"] = "single-node-gpu"
    monkeypatch.setattr(module, "_dispatch_report", lambda _: (
        {"qualification": "READY", "documentDigest": "sha256:" + ("b" if mutation == "profile" else "a") * 64}, {}))
    monkeypatch.setattr(module, "_load_prepared", lambda *_: prepared)
    def forbidden(*args, **kwargs):
        pytest.fail("changed preparation must be rejected before receipt checks")
    monkeypatch.setattr(module, "_gate_receipt", forbidden)
    args = SimpleNamespace(profile=tmp_path / "profile", run_id="test-submit",
                           output=tmp_path / "output", case="two-node-gpu")
    with pytest.raises(module.ClosureError, match=reason):
        module._submit(args)


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
    assert all(r['requestId'].startswith(plan['namespace'] + '/requests/') for r in plan['requests'])
    assert all(r['output'] == str(Path(plan['output']) / 'node0/user/requests' / str(r['index']))
               for r in plan['requests'])
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


def dispatch_profile(tmp_path):
    from runtime.yolo_bundle import freeze_harness
    from runtime.yolo_profile import check_plane
    from test_yolo_bundle import fixture_manifest
    from test_yolo_closure import next_plane
    path, value = input_profile(tmp_path)
    manifest, expected = fixture_manifest(tmp_path / "source")
    frozen = tmp_path / "frozen"
    freeze_harness(manifest, frozen, expected_manifest_sha256=expected)
    inputs = Path(value["release"]["inputs"]["path"])
    iid = check_plane(inputs, expected_stage="inputs")["id"]
    runtime, _ = next_plane(tmp_path / "runtime", "runtime", iid)
    rid = check_plane(runtime, expected_stage="runtime", parent_id=iid)["id"]
    dispatch, doc = next_plane(tmp_path / "dispatch", "dispatch", rid)
    harness = dispatch.parent / "harnessManifest"
    harness.write_bytes(manifest.read_bytes())
    doc["files"]["harnessManifest"] = dict(file_ref(harness), path=harness.name)
    dispatch.write_text(json.dumps(doc))
    value["release"].update(runtime=file_ref(runtime), dispatch=file_ref(dispatch))
    value["evidence"]["harnessManifest"] = file_ref(frozen / "harness-manifest.json")
    path.write_text(json.dumps(value))
    return path, value, frozen


def test_dispatch_check_binds_and_verifies_frozen_harness(tmp_path):
    path, _, _ = dispatch_profile(tmp_path)
    result = cli("check", "--stage", "dispatch", "--profile", path, cwd=tmp_path)
    assert result.returncode == 78, result.stderr
    report = json.loads(result.stdout)
    assert report["harness"]["integrity"] == "VERIFIED"
    assert report["harness"]["qualification"] == "NOT_EVALUATED"
    assert report["qualification"] == "NOT_EVALUATED"


@pytest.mark.parametrize("mutation,reason", [
    ("unbound", "HARNESS_DISPATCH_BINDING"),
    ("injected", "HARNESS_EXTRA_CONTENT"),
    ("writable", "HARNESS_NOT_SEALED"),
])
def test_dispatch_rejects_unbound_or_changed_bundle(tmp_path, mutation, reason):
    path, value, frozen = dispatch_profile(tmp_path)
    if mutation == "unbound":
        plane = Path(value["release"]["dispatch"]["path"])
        doc = json.loads(plane.read_text())
        replacement = plane.parent / "harnessManifest"
        replacement.write_text("another manifest")
        doc["files"]["harnessManifest"] = dict(file_ref(replacement), path=replacement.name)
        plane.write_text(json.dumps(doc))
        value["release"]["dispatch"] = file_ref(plane)
        path.write_text(json.dumps(value))
    elif mutation == "injected":
        frozen.chmod(0o755)
        (frozen / "oracle.npy").write_bytes(b"fixture forbidden extra")
        (frozen / "oracle.npy").chmod(0o444)
        frozen.chmod(0o555)
    else:
        (frozen / "runtime/baseline.py").chmod(0o644)
    result = cli("check", "--stage", "dispatch", "--profile", path, cwd=tmp_path)
    assert result.returncode == 2, result.stderr
    assert json.loads(result.stdout)["reason"] == reason


def _negative_collection_input(prepared, *, valid=True):
    candidate = "sha256:" + "a" * 64
    request_id = prepared["plan"]["requests"][0]["requestId"]
    rejection = {
        "schema": "tiger-yolo-expected-rejection-v1", "status": "REJECTED",
        "qualification": "EXPECTED_REJECTION_COMPONENT_ONLY",
        "case": "negative-dependency", "runId": prepared["runId"],
        "requestId": request_id, "attempt": 1, "candidateDigest": candidate,
        "selection": {"status": "COMMITTED", "selectedProvider": "/run/provider/BackboneNeck",
                       "selectionCount": 1, "reselectionCount": 0},
        "failure": {"boundary": "DEPENDENCY_DATA_MISSING",
                    "edge": {"producer": "BackboneNeck", "consumer": "DetectShard0",
                             "plannedName": "/run/data/backbone-to-head0"},
                    "observedAfterSelection": True, "reselected": False},
        "response": {"present": False, "success": False},
        "cleanup": {"qualification": "CLEANUP_COMPONENT_ONLY", "allChildrenReaped": True,
                     "forced": False, "remainingChildren": 0, "deadlineSatisfied": True},
        "elapsedMs": 812, "deadlineMs": 60000,
    }
    if not valid:
        rejection.pop("response")
    return {
        "schema": "tiger-yolo-collection-input-v1", "status": "READY",
        "runId": prepared["runId"], "candidateDigest": prepared["candidateDigest"],
        "case": "negative-dependency", "kind": "expected-rejection",
        "requestId": request_id, "attempt": 1,
        "candidateDigestForRequest": candidate, "requestDeadlineMs": 60000,
        "rejection": rejection,
    }


def test_collect_runs_expected_rejection_oracle_and_writes_immutable_verdict(tmp_path, monkeypatch, capsys):
    module = submit_module()
    run_id = "negative-run"
    request_id = "/test/spec183/negative-run/requests/0"
    prepared = {
        "runId": run_id, "case": "negative-dependency",
        "candidateDigest": "sha256:" + "b" * 64,
        "profileDigest": "sha256:" + "c" * 64,
        "plan": {"schema": "tiger-yolo-run-plan-v1", "runId": run_id,
                 "case": "negative-dependency", "requests": [
                     {"index": 0, "warmup": False, "requestId": request_id,
                      "output": str(tmp_path / "results" / run_id / "node0")}]},
    }
    root = tmp_path / "results" / run_id
    root.mkdir(parents=True)
    (root / "prepare.json").write_text("unused")
    (root / "collection-input.json").write_text(json.dumps(_negative_collection_input(prepared)))
    monkeypatch.setattr(module, "_dispatch_report",
                        lambda path: ({"qualification": "READY",
                                       "documentDigest": prepared["profileDigest"]}, {}))
    monkeypatch.setattr(module, "_load_prepared", lambda output, value: prepared)
    args = SimpleNamespace(profile=tmp_path / "profile.json", run_id=run_id,
                           output=tmp_path / "results")
    assert module._collect(args) == 0
    verdict = json.loads((root / "verdict.json").read_text())
    assert verdict["qualification"] == "EXPECTED_REJECTION_PASS"
    assert verdict["collectorSchema"] == "tiger-yolo-collector-v1"
    assert json.loads(capsys.readouterr().out)["status"] == "PASS"
    mode = (root / "verdict.json").stat().st_mode & 0o777
    assert mode == 0o444
    assert module._collect(args) == 0


def test_collect_retains_first_rejection_without_promoting_bad_handoff(tmp_path, monkeypatch):
    module = submit_module()
    run_id = "negative-run"
    prepared = {
        "runId": run_id, "case": "negative-dependency",
        "candidateDigest": "sha256:" + "b" * 64,
        "profileDigest": "sha256:" + "c" * 64,
        "plan": {"schema": "tiger-yolo-run-plan-v1", "runId": run_id,
                 "case": "negative-dependency", "requests": [
                     {"index": 0, "warmup": False,
                      "requestId": "/test/spec183/negative-run/requests/0",
                      "output": str(tmp_path / "results" / run_id / "node0")}]},
    }
    root = tmp_path / "results" / run_id
    root.mkdir(parents=True)
    (root / "collection-input.json").write_text(
        json.dumps(_negative_collection_input(prepared, valid=False)))
    monkeypatch.setattr(module, "_dispatch_report",
                        lambda path: ({"qualification": "READY",
                                       "documentDigest": prepared["profileDigest"]}, {}))
    monkeypatch.setattr(module, "_load_prepared", lambda output, value: prepared)
    args = SimpleNamespace(profile=tmp_path / "profile.json", run_id=run_id,
                           output=tmp_path / "results")
    assert module._collect(args) == module.INCOMPLETE
    failure = json.loads((root / "collection-failure.json").read_text())
    assert failure["schema"] == "tiger-yolo-collection-failure-v1"
    assert failure["status"] == "FAIL"
    assert not (root / "verdict.json").exists()
