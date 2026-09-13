"""Focused Spec186 profile, candidate and zero-side-effect gate tests."""
import copy
import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("spec186_candidate_test_module", ROOT / "runtime/spec186_candidate.py")
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


def profile_path(name="spec184-yolo-minindn-normal.json"):
    return ROOT / "profiles" / name


def test_all_declared_spec186_profiles_have_strict_schema():
    paths = sorted((ROOT / "profiles").glob("spec184-*.json"))
    assert paths
    cases = {candidate.load_profile(path, repo_root=ROOT)["case"] for path in paths}
    assert "yolo-minindn-normal" in cases
    assert "qwen06b-minindn-cpu" in cases
    assert "yolo-tiger-two-node-normal" in cases


@pytest.mark.parametrize("mutation", ["unknown", "duplicate", "wrong_case", "wrong_digest"])
def test_profile_mutations_fail_closed(tmp_path, mutation):
    value = json.loads(profile_path().read_text())
    if mutation == "unknown":
        value["ambientEnvironment"] = {"LD_LIBRARY_PATH": "/host/lib"}
    elif mutation == "wrong_case":
        value["case"] = "unknown"
    elif mutation == "wrong_digest":
        value["model"]["model"]["sha256"] = "bad"
    if mutation == "duplicate":
        path = tmp_path / "duplicate.json"
        path.write_text(profile_path().read_text().replace(
            '"schemaVersion": "ndnsf-spec186-experiment-profile-v1",',
            '"schemaVersion": "ndnsf-spec186-experiment-profile-v1",\n  "schemaVersion": "ndnsf-spec186-experiment-profile-v1",', 1))
    else:
        path = tmp_path / "profile.json"
        path.write_text(json.dumps(value))
    with pytest.raises(candidate.CandidateError):
        candidate.load_profile(path, repo_root=ROOT)


def test_candidate_digest_is_deterministic_and_change_plane_is_explicit():
    profile = candidate.load_profile(profile_path(), repo_root=ROOT)
    first = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    second = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    assert first == second
    changed = copy.deepcopy(first)
    changed["external"]["model"]["sha256"] = "f" * 64
    assert candidate.earliest_restart_gate(first, changed) == "T007/T008"


def test_application_bundle_directory_digest_is_content_addressed(tmp_path):
    bundle = tmp_path / "bundle"
    (bundle / "bin").mkdir(parents=True)
    (bundle / "bin" / "provider").write_bytes(b"provider-v1")
    first = candidate.tree_digest(bundle)
    assert first == candidate.tree_digest(bundle)
    (bundle / "bin" / "worker").write_bytes(b"worker-v1")
    assert candidate.tree_digest(bundle) != first


def test_pre_dispatch_rejects_without_remote_side_effects(tmp_path):
    profile = candidate.load_profile(profile_path(), repo_root=ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    receipt = candidate.pre_dispatch(profile_path(), candidate_path, repo_root=ROOT)
    assert receipt["ok"] is False
    assert receipt["sideEffects"] == {"ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}
    assert any(item.startswith("FILE_MISSING:") or item.startswith("PLACEHOLDER_DIGEST:")
               for item in receipt["failures"])


def test_terminal_collector_rejects_mixed_candidate_and_unreaped_process():
    record = {"candidateDigest": "a" * 64, "status": "PASS", "exitCode": 0,
              "cleanup": {"reaped": True}}
    assert candidate.validate_terminal([record], "a" * 64)["status"] == "PASS"
    mixed = dict(record, candidateDigest="b" * 64)
    assert candidate.validate_terminal([mixed], "a" * 64)["status"] == "FAILED"
    unreaped = dict(record, cleanup={"reaped": False})
    assert candidate.validate_terminal([unreaped], "a" * 64)["status"] == "FAILED"


def test_submit_rejects_before_scheduler_call(monkeypatch, tmp_path):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    manifest = candidate.build_candidate_manifest(
        candidate.load_profile(profile_path(), repo_root=ROOT), repo_root=ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": False, "failures": ["fixture"], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    monkeypatch.setattr(submit.subprocess, "run", lambda *a, **k: (_ for _ in ()).throw(
        AssertionError("scheduler must not run after pre-dispatch rejection")))
    result = submit.submit(profile_path(), candidate_path, run_id="reject",
                           run_root=tmp_path / "run")
    assert result["status"] == "REJECTED" and result["schedulerCalls"] == 0


def test_effective_config_has_explicit_case_transport_and_candidate():
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_render_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    effective = submit.render_effective(profile, manifest, "render", Path("/tmp/render"))
    assert effective["case"] == "yolo-minindn-normal"
    assert effective["candidateDigest"] == manifest["candidateDigest"]
    assert effective["environment"]["NDN_CLIENT_TRANSPORT"] == "unix:///run/nfd.sock"
    assert effective["argv"][-2:] == ["--case", "Y-A"]


def test_local_lifecycle_bounds_timeout_and_reaps_process(tmp_path, monkeypatch):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_lifecycle_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=ROOT)
    profile["timeouts"]["requestSeconds"] = 1
    profile["timeouts"]["cleanupSeconds"] = 1
    manifest = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": True, "failures": [], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    monkeypatch.setattr(submit.candidate, "load_profile", lambda *a, **k: profile)
    result = submit.local_run(
        profile_path(), candidate_path, run_id="timeout", run_root=tmp_path / "run",
        command=["/usr/bin/python3", "-c", "import time; time.sleep(5)"])
    assert result["status"] == "FAILED"
    assert result["cleanup"]["reaped"] is True and result["cleanup"]["forced"] is True
