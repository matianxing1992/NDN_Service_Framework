"""Focused Spec186 profile, candidate and zero-side-effect gate tests."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess

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


def test_all_profiles_pin_apptainer_153_and_local_path_is_executable():
    paths = sorted((ROOT / "profiles").glob("spec184-*.json"))
    for path in paths:
        profile = candidate.load_profile(path, repo_root=ROOT)
        runtime = profile["runtime"]["apptainer"]
        assert runtime["version"] == "1.5.3"
        if profile["topology"]["mode"] == "minindn":
            assert runtime["path"] == "/usr/local/bin/apptainer"
            assert Path(runtime["path"]).is_file()
        else:
            assert runtime["path"] == "/home/tma1/.local/bin/apptainer-1.5.3"


def test_apptainer_134_profile_is_rejected(tmp_path):
    value = json.loads(profile_path().read_text())
    value["runtime"]["apptainer"]["version"] = "1.3.4"
    path = tmp_path / "wrong-apptainer.json"
    path.write_text(json.dumps(value))
    with pytest.raises(candidate.CandidateError, match="APPTAINER_VERSION_POLICY"):
        candidate.load_profile(path, repo_root=ROOT)


def test_source_commit_must_descend_from_spec186_baseline(tmp_path):
    value = json.loads(profile_path().read_text())
    value["candidate"]["sourceCommit"] = "0df480f4a0979b697eee820038266eb813ba7c62"
    path = tmp_path / "unrelated-source.json"
    path.write_text(json.dumps(value))
    with pytest.raises(candidate.CandidateError, match="SOURCE_BASELINE_LINEAGE"):
        candidate.load_profile(path, repo_root=ROOT)


def test_role_backend_and_service_are_bound_to_gpu_contract(tmp_path):
    value = json.loads(profile_path().read_text())
    value["roles"][0]["gpu"] = 0
    value["roles"][0]["service"] = "/ObjectDetection/YOLOv8/wrong"
    path = tmp_path / "role-contract.json"
    path.write_text(json.dumps(value))
    with pytest.raises(candidate.CandidateError, match="ROLE_SERVICE"):
        candidate.load_profile(path, repo_root=ROOT)
    value["roles"][0]["service"] = "/AI/YOLO/YOLO26n/BackboneNeck"
    path.write_text(json.dumps(value))
    with pytest.raises(candidate.CandidateError, match="ROLE_BACKEND_GPU_MISMATCH"):
        candidate.load_profile(path, repo_root=ROOT)


def test_two_node_profile_rejects_duplicate_nfd_endpoint_host(tmp_path):
    value = json.loads((ROOT / "profiles/spec184-yolo-tiger-two-node-normal.json").read_text())
    value["topology"]["nfdEndpoints"][1]["host"] = value["topology"]["nfdEndpoints"][0]["host"]
    path = tmp_path / "duplicate-endpoint-host.json"
    path.write_text(json.dumps(value))
    with pytest.raises(candidate.CandidateError, match="NFD_ENDPOINT_HOSTS_NOT_DISTINCT"):
        candidate.load_profile(path, repo_root=ROOT)


@pytest.mark.parametrize("mutation", ["unknown", "duplicate", "wrong_case", "wrong_digest",
                                      "nested_unknown", "role_node", "role_identity",
                                      "bundle_digest", "tiger_fallback", "role_fallback"])
def test_profile_mutations_fail_closed(tmp_path, mutation):
    value = json.loads(profile_path().read_text())
    if mutation == "unknown":
        value["ambientEnvironment"] = {"LD_LIBRARY_PATH": "/host/lib"}
    elif mutation == "wrong_case":
        value["case"] = "unknown"
    elif mutation == "wrong_digest":
        value["model"]["model"]["sha256"] = "bad"
    elif mutation == "nested_unknown":
        value["topology"]["unexpected"] = True
    elif mutation == "role_node":
        value["roles"][0]["node"] = "other-host"
    elif mutation == "role_identity":
        value["roles"][1]["identity"] = value["roles"][0]["identity"]
    elif mutation == "bundle_digest":
        value["runtime"]["application"]["bundleSha256"] = "f" * 64
    elif mutation == "tiger_fallback":
        value = json.loads((ROOT / "profiles/spec184-yolo-tiger-single-gpu.json").read_text())
        value["runtime"]["allowCpuFallback"] = True
    elif mutation == "role_fallback":
        value["roles"][0]["allowCpuFallback"] = False
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


def test_pre_dispatch_has_no_legacy_yolo_runner_drift(tmp_path):
    repository_root = ROOT.parents[1]
    profile = candidate.load_profile(profile_path(), repo_root=repository_root)
    manifest = candidate.build_candidate_manifest(profile, repo_root=repository_root)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    receipt = candidate.pre_dispatch(profile_path(), candidate_path, repo_root=repository_root)
    assert not any(item.startswith("HARNESS_MODEL_FAMILY_MISMATCH:")
                   for item in receipt["failures"])
    assert not any(item.startswith("HARNESS_ENVIRONMENT_UNDECLARED:")
                   for item in receipt["failures"])
    assert "HARNESS_ROLE_SERVICE_MAP_UNDECLARED" not in receipt["failures"]
    assert receipt["sideEffects"] == {"ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}


def test_pre_dispatch_rejects_tiger_resource_placeholders(tmp_path):
    path = ROOT / "profiles/spec184-yolo-tiger-single-gpu.json"
    profile = candidate.load_profile(path, repo_root=ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    receipt = candidate.pre_dispatch(path, candidate_path, repo_root=ROOT)
    assert "RESOURCE_PLACEHOLDER:account" in receipt["failures"]
    assert "GPU_UUID_PLACEHOLDER" in receipt["failures"]
    assert "GPU_SIGNATURE_PLACEHOLDER" in receipt["failures"]


def test_pre_dispatch_rejects_qwen_q3_against_onnx_harness(tmp_path):
    path = ROOT / "profiles/spec184-qwen06b-minindn-cpu.json"
    repository_root = ROOT.parents[1]
    profile = candidate.load_profile(path, repo_root=repository_root)
    manifest = candidate.build_candidate_manifest(profile, repo_root=repository_root)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    receipt = candidate.pre_dispatch(path, candidate_path, repo_root=repository_root)
    assert "HARNESS_MODEL_CONTRACT_MISMATCH:expected-onnxruntime-cpu" in receipt["failures"]
    assert any(item.startswith("HARNESS_QWEN_INPUTS_UNDECLARED:")
               for item in receipt["failures"])


@pytest.mark.parametrize("section", ["runtime", "application", "external", "security"])
def test_pre_dispatch_rejects_malformed_manifest_sections_without_traceback(tmp_path, section):
    profile = candidate.load_profile(profile_path(), repo_root=ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    manifest[section] = []
    candidate_path = tmp_path / "malformed.json"
    candidate_path.write_text(json.dumps(manifest))
    receipt = candidate.pre_dispatch(profile_path(), candidate_path, repo_root=ROOT)
    assert receipt["ok"] is False
    assert any(item.startswith("CANDIDATE_SECTION_NOT_OBJECT:") for item in receipt["failures"])
    assert receipt["sideEffects"] == {"ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}


def test_pre_dispatch_rejects_unknown_nested_manifest_field(tmp_path):
    profile = candidate.load_profile(profile_path(), repo_root=ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=ROOT)
    manifest["runtime"]["baseSif"]["unexpected"] = "must-reject"
    candidate_path = tmp_path / "nested-unknown.json"
    candidate_path.write_text(json.dumps(manifest))
    receipt = candidate.pre_dispatch(profile_path(), candidate_path, repo_root=ROOT)
    assert receipt["ok"] is False
    assert "UNKNOWN_CANDIDATE_ASSET_FIELD:runtime.baseSif:unexpected" in receipt["failures"]
    assert receipt["sideEffects"] == {"ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}


def _run_sbatch(tmp_path, *, run_id="sbatch-test", argv=None, rendered=None, run_root=None):
    script = ROOT / "jobs/spec184/run.sbatch"
    run_root = run_root or (tmp_path / "run")
    config = {
        "schemaVersion": "spec186-effective-config-v1",
        "runId": run_id,
        "candidateDigest": "a" * 64,
        "runRoot": str(run_root),
    }
    env = os.environ.copy()
    env.update({
        "SPEC186_RUN_ID": run_id,
        "SPEC186_EFFECTIVE_CONFIG": json.dumps(config),
        "SPEC186_EXECUTE": "1",
        "SPEC186_EXEC_ARGV": json.dumps(argv or ["/usr/bin/true"]),
        "SPEC186_EXEC_ENV": json.dumps(rendered or {}),
    })
    return subprocess.run(["bash", str(script)], env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def test_run_sbatch_rejects_malformed_argv_before_run_root_creation(tmp_path):
    result = _run_sbatch(tmp_path, argv="not-json")
    assert result.returncode != 0
    assert "SPEC186_EXEC_ARGV_INVALID" in result.stderr
    assert "Traceback" not in result.stderr
    assert not (tmp_path / "run").exists()


def test_run_sbatch_rejects_run_root_reuse(tmp_path):
    first = _run_sbatch(tmp_path)
    assert first.returncode == 0
    second = _run_sbatch(tmp_path)
    assert second.returncode != 0
    assert "SPEC186_RUN_ROOT_CREATE_FAILED" in second.stderr


def test_run_sbatch_scopes_home_to_run_root(tmp_path):
    result = _run_sbatch(
        tmp_path,
        argv=["/usr/bin/python3", "-c",
              "from pathlib import Path; import os; Path('home.txt').write_text(os.environ['HOME'])"],
    )
    assert result.returncode == 0
    assert (tmp_path / "run" / "home.txt").read_text() == str(tmp_path / "run" / "home")


def test_run_sbatch_rejects_identity_env_drift_before_run_root_creation(tmp_path):
    result = _run_sbatch(tmp_path, rendered={"SPEC186_CANDIDATE_DIGEST": "b" * 64})
    assert result.returncode != 0
    assert "SPEC186_EXEC_ENV_IDENTITY_MISMATCH" in result.stderr
    assert not (tmp_path / "run").exists()


def test_terminal_collector_rejects_mixed_candidate_and_unreaped_process():
    digest = "a" * 64
    record = {"candidateDigest": digest, "runId": "run-1", "status": "PASS", "exitCode": 0,
              "protocol": {"candidateDigest": digest, "completed": True,
                           "terminalResponse": True},
              "numerical": {"candidateDigest": digest, "matched": True,
                            "independent": True, "oracleDigest": "b" * 64},
              "roles": [{"name": name, "candidateDigest": digest, "observed": True,
                         "backend": "onnxruntime-cpu", "gpu": -1}
                        for name in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")],
              "process": {"candidateDigest": digest, "allExited": True, "exitCode": 0},
              "cleanup": {"candidateDigest": digest, "reaped": True}}
    assert candidate.validate_terminal([record], digest)["status"] == "PASS"
    mixed = dict(record, candidateDigest="b" * 64)
    assert candidate.validate_terminal([mixed], digest)["status"] == "FAILED"
    unreaped = dict(record, cleanup={"candidateDigest": digest, "reaped": False})
    assert candidate.validate_terminal([unreaped], digest)["status"] == "FAILED"


def test_terminal_collector_rejects_marker_only_pass():
    digest = "a" * 64
    marker = {"candidateDigest": digest, "runId": "run-1", "status": "PASS",
              "exitCode": 0, "cleanup": {"candidateDigest": digest, "reaped": True}}
    result = candidate.validate_terminal([marker], digest)
    assert result["status"] == "FAILED"
    assert "PROTOCOL_EVIDENCE:0" in result["failures"]
    assert "NUMERICAL_EVIDENCE:0" in result["failures"]
    assert "ROLE_EVIDENCE:0" in result["failures"]
    assert "PROCESS_EVIDENCE:0" in result["failures"]


def test_terminal_collector_rejects_invalid_run_id():
    digest = "a" * 64
    result = candidate.validate_terminal([{
        "candidateDigest": digest, "runId": "../old", "status": "PASS", "exitCode": 0,
        "protocol": {"candidateDigest": digest, "completed": True, "terminalResponse": True},
        "numerical": {"candidateDigest": digest, "matched": True, "independent": True,
                       "oracleDigest": "b" * 64},
        "roles": [{"name": name, "candidateDigest": digest, "observed": True,
                   "backend": "onnxruntime-cpu", "gpu": -1}
                  for name in ("Stage0", "Stage1")],
        "process": {"candidateDigest": digest, "allExited": True, "exitCode": 0},
        "cleanup": {"candidateDigest": digest, "reaped": True},
    }], digest)
    assert result["status"] == "FAILED"
    assert "RUN_ID_INVALID:0" in result["failures"]


def test_terminal_collector_rejects_invalid_yolo_gpu_role_policy():
    digest = "a" * 64
    roles = [{"name": name, "candidateDigest": digest, "observed": True,
              "backend": "onnxruntime-cuda", "gpu": 0}
             for name in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")]
    record = {"candidateDigest": digest, "runId": "gpu-role", "status": "PASS",
              "exitCode": 0,
              "protocol": {"candidateDigest": digest, "completed": True,
                           "terminalResponse": True},
              "numerical": {"candidateDigest": digest, "matched": True,
                            "independent": True, "oracleDigest": "b" * 64},
              "roles": roles,
              "process": {"candidateDigest": digest, "allExited": True, "exitCode": 0},
              "cleanup": {"candidateDigest": digest, "reaped": True}}
    result = candidate.validate_terminal([record], digest)
    assert result["status"] == "FAILED"
    assert "ROLE_EVIDENCE:0" in result["failures"]


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


def test_submit_rejects_existing_run_root_before_scheduler_call(monkeypatch, tmp_path):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_root_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    manifest = candidate.build_candidate_manifest(
        candidate.load_profile(profile_path(), repo_root=ROOT), repo_root=ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": True, "failures": [], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    monkeypatch.setattr(submit.subprocess, "run", lambda *a, **k: (_ for _ in ()).throw(
        AssertionError("scheduler must not run for an existing run root")))
    run_root = tmp_path / "run"
    run_root.mkdir()
    result = submit.submit(profile_path(), candidate_path, run_id="duplicate",
                           run_root=run_root)
    assert result["status"] == "REJECTED"
    assert result["schedulerCalls"] == 0
    assert result["runRootError"] == "SPEC186_RUN_ROOT_EXISTS_OR_INVALID"


def test_submit_rejects_missing_candidate_without_traceback(tmp_path):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_missing_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    result = submit.submit(profile_path(), tmp_path / "missing.json", run_id="missing",
                           run_root=tmp_path / "run")
    assert result["status"] == "REJECTED"
    assert result["schedulerCalls"] == 0
    assert result["error"].startswith("CANDIDATE_INPUT_INVALID:")


def test_effective_config_has_explicit_case_transport_and_candidate():
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_render_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=submit.REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
    effective = submit.render_effective(profile, manifest, "render", Path("/tmp/render"))
    assert effective["case"] == "yolo-minindn-normal"
    assert effective["candidateDigest"] == manifest["candidateDigest"]
    # MiniNDN is the host orchestration process.  The runner's child NFD and
    # native applications receive the exact SIF through the command-provider
    # environment; wrapping MiniNDN itself in --containall hides its host
    # dependencies and cannot work.
    assert effective["argv"][:2] == ["/usr/bin/python3", str(submit.REPO_ROOT / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py")]
    assert effective["environment"]["SPEC186_APPTAINER_VERSION"] == "1.5.3"
    assert effective["environment"]["NDN_CLIENT_TRANSPORT"] == "unix:///run/nfd.sock"
    assert effective["environment"]["SPEC180_NATIVE_PROVIDER_BINARY"] == "/app/bundle/bin/di-native-provider"
    assert effective["environment"]["SPEC180_RUNTIME_SIF"].endswith(".sif")
    assert effective["argv"][-2:] == ["--case", "Y-B"]
    assert all(role["allowCpuFallback"] is True for role in effective["roles"])
    assert effective["roles"][0]["service"] == "/AI/YOLO/YOLO26n/BackboneNeck"
    assert effective["environment"]["SPEC180_YOLO_CANONICAL_PACKAGE"].endswith(
        "/Y-B/canonical-package")
    assert effective["environment"]["SPEC180_YOLO_CONFIG"].endswith(
        "/Y-B/case-config.json")
    assert effective["environment"]["SPEC180_RUNTIME_RUN_ROOT"] == "/tmp/render"


def test_sif_child_paths_map_only_case_run_root(tmp_path):
    runner_path = ROOT.parent / "NDNSF_DI_YoloAckDriven_Minindn.py"
    runner_spec = importlib.util.spec_from_file_location(
        "spec180_runner_path_mapping_test_module", runner_path)
    runner = importlib.util.module_from_spec(runner_spec)
    import sys
    sys.modules[runner_spec.name] = runner
    runner_spec.loader.exec_module(runner)
    run_root = tmp_path / "case"
    assert runner._sif_visible_path(
        run_root / "evidence/case-policy.json", run_root
    ) == "/run/spec186/evidence/case-policy.json"
    assert runner._sif_visible_path(
        run_root / "security/request-envelope.key", run_root
    ) == "/run/spec186/security/request-envelope.key"
    external = tmp_path / "case-bundle/case-config.json"
    assert runner._sif_visible_path(external, run_root) == str(external)
    assert runner._sif_visible_path("1", run_root) == "1"
    assert runner._sif_visible_path("*=WARN", run_root) == "*=WARN"


def test_tiger_render_targets_apptainer_and_yolo_runner():
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_tiger_render_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    path = ROOT / "profiles" / "spec184-yolo-tiger-single-gpu.json"
    profile = candidate.load_profile(path, repo_root=submit.REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
    effective = submit.render_effective(profile, manifest, "tiger-render", Path("/tmp/tiger-render"))
    assert effective["execution"]["mode"] == "slurm-apptainer"
    launcher = effective["execution"]["containerLauncher"]
    assert launcher[:2] == ["/home/tma1/.local/bin/apptainer-1.5.3", "exec"]
    assert "/opt/ndnsf-di/replay/repo/Experiments/NDNSF_DI_YoloAckDriven_Minindn.py" in effective["argv"]
    assert effective["argv"][-2:] == ["--case", "Y-A"]
    assert effective["environment"]["SPEC180_RUNTIME_OUTER"] == "1"


def test_local_passes_rendered_environment_to_child(tmp_path, monkeypatch):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_environment_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=submit.REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": True, "failures": [], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    monkeypatch.setattr(submit.candidate, "load_profile", lambda *a, **k: profile)
    monkeypatch.setenv("LD_LIBRARY_PATH", "/host/lib")
    monkeypatch.setenv("PYTHONPATH", "/host/python")
    run_root = tmp_path / "run"
    result = submit.local_run(
        profile_path(), candidate_path, run_id="env-check", run_root=run_root,
        command=["/usr/bin/python3", "-c",
                 "from pathlib import Path; import os; Path('env.txt').write_text(os.environ['SPEC186_RUN_ID']); Path('leaks.txt').write_text(os.environ.get('LD_LIBRARY_PATH', '') + '|' + os.environ.get('PYTHONPATH', '')); Path('path.txt').write_text(os.environ['PATH'])"])
    assert result["status"] == "UNQUALIFIED"
    assert (run_root / "env.txt").read_text() == "env-check"
    assert (run_root / "leaks.txt").read_text() == "|"
    assert "/usr/sbin" in (run_root / "path.txt").read_text()
    assert "/sbin" in (run_root / "path.txt").read_text()
    assert all((run_root / child).is_dir()
               for child in ("evidence", "state", "security", "home"))


def test_local_rejects_run_root_reuse(tmp_path, monkeypatch):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_reuse_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=submit.REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": True, "failures": [], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    monkeypatch.setattr(submit.candidate, "load_profile", lambda *a, **k: profile)
    run_root = tmp_path / "run"
    run_root.mkdir()
    result = submit.local_run(
        profile_path(), candidate_path, run_id="reuse", run_root=run_root,
        command=["/usr/bin/true"])
    assert result["status"] == "REJECTED"
    assert result["runRootError"] == "SPEC186_RUN_ROOT_EXISTS"


def test_submit_exports_rendered_environment(monkeypatch, tmp_path):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_export_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=submit.REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": True, "failures": [], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    captured = {}

    def fake_run(argv, **kwargs):
        captured["argv"] = argv
        captured["env"] = kwargs["env"]
        return type("Result", (), {"returncode": 0, "stdout": "submitted"})()

    monkeypatch.setattr(submit.subprocess, "run", fake_run)
    result = submit.submit(profile_path(), candidate_path, run_id="export",
                           run_root=tmp_path / "run")
    assert result["status"] == "SUBMITTED"
    rendered = json.loads(captured["env"]["SPEC186_EXEC_ENV"])
    assert rendered["SPEC186_RUN_ID"] == "export"
    assert json.loads(captured["env"]["SPEC186_EXEC_ARGV"])[-2:] == ["--case", "Y-B"]
    assert "--ntasks-per-node=1" in captured["argv"]
    assert "--mem=8G" in captured["argv"]


def test_local_rejects_invalid_command_without_traceback(tmp_path, monkeypatch):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_invalid_command_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=submit.REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(submit.candidate, "pre_dispatch", lambda *a, **k: {
        "ok": True, "failures": [], "sideEffects": {
            "ssh": 0, "rsync": 0, "staging": 0, "sbatch": 0}})
    monkeypatch.setattr(submit.candidate, "load_profile", lambda *a, **k: profile)
    result = submit.local_run(profile_path(), candidate_path, run_id="bad-command",
                              run_root=tmp_path / "run", command=[None])
    assert result["status"] == "REJECTED"
    assert result["error"] == "SPEC186_LOCAL_ARGV_INVALID"


def test_local_lifecycle_bounds_timeout_and_reaps_process(tmp_path, monkeypatch):
    submit_spec = importlib.util.spec_from_file_location(
        "spec186_submit_lifecycle_test_module", ROOT / "jobs/spec184/submit.py")
    submit = importlib.util.module_from_spec(submit_spec)
    submit_spec.loader.exec_module(submit)
    profile = candidate.load_profile(profile_path(), repo_root=submit.REPO_ROOT)
    profile["timeouts"]["requestSeconds"] = 1
    profile["timeouts"]["cleanupSeconds"] = 1
    manifest = candidate.build_candidate_manifest(profile, repo_root=submit.REPO_ROOT)
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
