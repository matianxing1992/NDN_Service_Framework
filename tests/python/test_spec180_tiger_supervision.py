"""Real, tiny subprocess regressions for T013 supervision, never qualification."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging/ndnsf-di-container/jobs/spec180/supervise-tiger.py"


def load():
    spec = importlib.util.spec_from_file_location("spec180_tiger_supervision", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def supervisor(tmp_path):
    module = load()
    value = module.Supervisor(tmp_path)
    yield module, value
    value.cleanup(grace_s=0.1, kill_s=1)


def start(value, name, code):
    return value.start(name, [sys.executable, "-u", "-c", code], os.environ.copy())


def test_dead_child_cannot_pass_with_existing_ready_marker(supervisor):
    module, value = supervisor
    child = start(value, "controller", "print('READY')")
    child.process.wait(timeout=2)
    with pytest.raises(module.SupervisionError, match="CHILD_EXITED:controller"):
        value.wait_marker(child, "READY", 1)


def test_readiness_checks_other_children(supervisor):
    module, value = supervisor
    controller = start(value, "controller", "raise SystemExit(7)")
    controller.process.wait(timeout=2)
    repo = start(value, "repo", "import time; print('READY'); time.sleep(5)")
    with pytest.raises(module.SupervisionError, match="CHILD_EXITED:controller"):
        value.wait_marker(repo, "READY", 1)


def test_readiness_times_out_and_does_not_start_next_phase(supervisor):
    module, value = supervisor
    controller = start(value, "controller", "import time; time.sleep(5)")
    with pytest.raises(module.SupervisionError, match="READY_TIMEOUT:controller"):
        value.wait_marker(controller, "READY", 0.1)
    assert [c.name for c in value.children] == ["controller"]


def test_duplicate_process_name_rejected_before_spawn(supervisor):
    module, value = supervisor
    start(value, "controller", "import time; time.sleep(5)")
    with pytest.raises(module.SupervisionError, match="DUPLICATE_CHILD"):
        start(value, "controller", "raise SystemExit(99)")
    assert len(value.children) == 1


def test_hung_user_is_bounded_even_after_success_marker(supervisor):
    module, value = supervisor
    user = start(value, "user", "import time; print('YOLO_ACK_DRIVEN_RESULT status=true'); time.sleep(5)")
    with pytest.raises(module.SupervisionError, match="USER_TIMEOUT"):
        value.wait_user(user, 0.1)
    record = value.cleanup(grace_s=0.1, kill_s=1)
    assert record["children"][0]["timedOut"] is True


def test_cleanup_collects_failure_and_escalates_stubborn_child(supervisor):
    _, value = supervisor
    stubborn = start(value, "provider-Merge", "import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); print('READY'); time.sleep(30)")
    value.wait_marker(stubborn, "READY", 1)
    bad = start(value, "repo", "raise SystemExit(7)")
    bad.process.wait(timeout=2)
    before = time.monotonic()
    record = value.cleanup(grace_s=0.1, kill_s=1)
    assert time.monotonic() - before < 2
    assert record["allExited"] and record["outputClosed"]
    rows = {x["id"]: x for x in record["children"]}
    assert rows["repo"]["exitStatus"] == 7
    assert rows["provider-Merge"]["exitStatus"] == -signal.SIGKILL
    assert rows["provider-Merge"]["timedOut"] is True


def test_cleanup_reaches_group_after_leader_exit(supervisor, tmp_path):
    _, value = supervisor
    pidfile = tmp_path / "descendant.pid"
    code = ("import subprocess,sys; p=subprocess.Popen([sys.executable,'-c',"
            "'import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); time.sleep(30)']); "
            f"open({str(pidfile)!r},'w').write(str(p.pid))")
    child = start(value, "repo", code)
    child.process.wait(timeout=2)
    descendant = int(pidfile.read_text())
    try:
        record = value.cleanup(grace_s=0.1, kill_s=1)
        assert record["allExited"]
        stat = Path(f"/proc/{descendant}/stat")
        assert not stat.exists() or stat.read_text().split(")", 1)[1].split()[0] == "Z"
    finally:
        try:
            os.kill(descendant, signal.SIGKILL)
        except ProcessLookupError:
            pass


def test_unproduced_result_cannot_qualify(tmp_path):
    module = load()
    with pytest.raises(module.SupervisionError, match="TERMINAL_RESULT_MISSING"):
        module.validate_terminal(tmp_path, "sha256:" + "a" * 64)


def test_in_image_launcher_uses_same_tree_as_probe():
    text = (SCRIPT.parent / "run-functional.sh").read_text()
    assert 'BUNDLE_RENDERER=' not in text
    assert '/bundle/packaging/ndnsf-di-container/jobs/spec180/run-ndnsf-yolo.sh' not in text
    assert '/opt/ndnsf-di/replay/repo/packaging/ndnsf-di-container/jobs/spec180' in text


@pytest.mark.parametrize("failure", ["controller", "repo", "provider-DetectShard0", ""])
def test_real_execute_owns_phase_barriers_and_always_cleans(tmp_path, monkeypatch, failure):
    module = load()
    monkeypatch.setenv("SLURM_JOB_ID", "focused-test")
    monkeypatch.setenv("SPEC180_CANDIDATE_ID", "focused-test")
    monkeypatch.setenv("SPEC180_CANDIDATE_DIGEST", "sha256:" + "a" * 64)
    actions = []
    class RuntimeDouble:
        def __init__(self, logs):
            self.children = []
        def start(self, name, argv, env):
            actions.append("start:" + name)
            child = type("Child", (), {"name": name})()
            self.children.append(child)
            return child
        def wait_marker(self, child, marker, seconds):
            actions.append("ready:" + child.name)
            if child.name == failure:
                raise module.SupervisionError("READY_TIMEOUT:" + child.name)
        def wait_until(self, predicate, seconds, label):
            actions.append(label)
        def wait_user(self, child, seconds):
            actions.append("user-exited")
        def cleanup(self):
            actions.append("cleanup")
            return {"children": [], "childExitCount": 0, "allExited": True,
                    "outputClosed": True, "survivors": [], "pids": {}}
    monkeypatch.setattr(module, "Supervisor", RuntimeDouble)
    monkeypatch.setattr(module.subprocess, "run", lambda *a, **k: None)
    paths = {}
    for name in ("controller", "repo", "user"):
        paths[name + "_args"] = tmp_path / (name + ".args")
        paths[name + "_args"].write_text("/bin/true\n")
    provider_dir = tmp_path / "providers"
    provider_dir.mkdir()
    for role in module.ROLES:
        (provider_dir / (role + ".args")).write_text("/bin/true\n")
    nfd_config = tmp_path / "nfd.conf"
    nfd_config.write_text("fixture")
    args = type("Args", (), dict(paths, evidence=tmp_path,
        scratch=tmp_path / "runtime", provider_args_dir=provider_dir, nfd_config=nfd_config))()
    assert module.execute(args) == 8  # no result producer exists, even when user exits zero
    assert actions[-1] == "cleanup"
    if failure == "controller":
        assert "start:repo" not in actions
    if failure in {"controller", "repo"}:
        assert "start:provider-BackboneNeck" not in actions
    if failure:
        assert "start:user" not in actions
    else:
        assert actions.index("ready:controller") < actions.index("start:repo")
        assert actions.index("ready:repo") < actions.index("start:provider-BackboneNeck")
        assert actions.index("ready:provider-Merge") < actions.index("start:user")
    terminal = json.loads((tmp_path / "orchestration-terminal.json").read_text())
    assert terminal["status"] == "FAILED"
    # Collection now precedes terminal validation. This supervisor double has
    # no evidence producer, so the collector must reject before validation.
    assert terminal["reason"] == ("READY_TIMEOUT:" + failure if failure else "CollectionError")
    assert (tmp_path / "supervision.json").is_file()


def test_terminal_matches_observed_children_and_pids(tmp_path):
    from test_spec180_tiger_contract import result_fixture
    module = load()
    result = result_fixture(tmp_path)
    (tmp_path / "spec180-result.json").write_text(json.dumps(result))
    observed = {"children": result["children"], "childExitCount": 8,
                "allExited": True, "outputClosed": True,
                "pids": {"provider-" + p["role"]: p["pid"] for p in result["runtimeOracle"]["fields"]["providers"]}}
    module.validate_terminal(tmp_path, result["candidateDigest"], result["candidateId"], observed)
    observed["pids"]["provider-Merge"] += 1
    with pytest.raises(module.SupervisionError, match="TERMINAL_PROVIDER_PID_MISMATCH"):
        module.validate_terminal(tmp_path, result["candidateDigest"], result["candidateId"], observed)


def test_sealed_source_includes_terminal_validator():
    path = ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/prepare-local-sif-source.py"
    spec = importlib.util.spec_from_file_location("source_sealer", path)
    sealer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sealer)
    selected = {str(path) for path in sealer.selected_files(ROOT)}
    assert "scripts/validate_spec180_results.py" in selected
    assert "packaging/ndnsf-di-container/jobs/spec180/supervise-tiger.py" in selected


def test_deferred_qwen_gate_fails_before_file_or_container_access(tmp_path):
    env = os.environ.copy()
    for name in ("GATE", "PROFILE_ID", "PROFILE_SHA256", "RUN_ID", "CANDIDATE_ID",
                 "CANDIDATE_DIGEST", "SIF", "SIF_SHA256", "MODEL_MANIFEST",
                 "MODEL_MANIFEST_SHA256", "MODEL_ROOT", "WORKLOAD", "WORKLOAD_SHA256", "OUTPUT_ROOT"):
        env["SPEC180_" + name] = str(tmp_path / "must-not-exist")
    env["SPEC180_GATE"] = "qwen-functional"
    result = subprocess.run(["bash", str(SCRIPT.parent / "run-functional.sh")],
                            env=env, capture_output=True, text=True, timeout=2)
    assert result.returncode == 2
    assert "SPEC180_UNKNOWN_GATE:qwen-functional" in result.stderr
    assert not (tmp_path / "must-not-exist").exists()
