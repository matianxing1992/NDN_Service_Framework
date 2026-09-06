"""Offline tests of real orchestration decisions; never submit a cluster job."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("tiger_baseline_test_module", ROOT / "runtime/baseline.py")
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


@pytest.mark.parametrize("key,value", [
    ("nodes", 1), ("tcpPort", 80), ("cpusPerNode", True), ("requestTimeoutMs", -1),
    ("sifSha256", "latest"), ("sif", "/tmp/runtime.sif:/etc"),
    ("partition", "bigTiger\n--nodes=100"), ("memory", "ALL"),
    ("unknownEnvironment", {"NDNSF_SKIP_VALIDATION": "1"}),
])
def test_profile_rejects_unconsumed_or_invalid_input(tmp_path, key, value):
    profile = json.loads((ROOT / "profiles/two-node.json").read_text())
    profile[key] = value
    path = tmp_path / "profile.json"
    path.write_text(json.dumps(profile))
    with pytest.raises(ValueError):
        baseline.load_profile(path)


def test_container_has_only_own_home_and_in_image_tools(tmp_path):
    profile = baseline.load_profile(ROOT / "profiles/two-node.json")
    command = baseline.container_command(profile, tmp_path / "bundle", tmp_path / "private/user",
        tmp_path / "public", tmp_path / "out/user", [baseline.BIN + "/nfdc", "status"], node=tmp_path / "node")
    assert command[command.index("--home") + 1] == str(tmp_path / "private/user") + ":/identities/user"
    assert "--cleanenv" in command and "--containall" in command
    assert not any(x.startswith("HOME=") for x in command)
    assert not any(":/identities:rw" in x or "root.key" in x for x in command)
    assert command[-2:] == ["/opt/ndnsf-di/current/bin/nfdc", "status"]


def fixture_records():
    run = {"runId": "unit-case", "sifSha256": "a" * 64, "bundleFiles": {"a.py": "b" * 64},
           "profileSha256": "c" * 64, "mode": "slurm", "payload": "hello", "privateRemoved": True,
           "namespace": "/example/tiger/unit-case"}
    nodes = []
    for rank in (0, 1):
        cases = {"raw-roundtrip": {"status": "PASS", "payload": f"DATA:raw{1-rank}:unit-case"},
                 "bad-signature": {"status": "PASS", "reason": "ValidationFailure"},
                 "wrong-root": {"status": "PASS", "reason": "ValidationFailure"}}
        if rank == 0:
            provider = run["namespace"] + "/provider"
            cases.update({"service-echo": {"status": "PASS", "payload": "ECHO:hello",
                              "requestId": "/request-1", "selectedProviders": [provider],
                              "acks": [{"provider": provider, "status": True,
                                        "service": "/TIGER_ECHO", "requestId": "/request-1"}]},
                          "native-trust-rejection": {"status": "PASS", "nativeValidationFailure": True,
                              "boundary": "PUBPARAMS", "exitCode": 134,
                              "termination": "EXPECTED_AUTHENTICATION_ABORT"},
                          "permission-rejection": {"status": "PASS", "nativeRejection": True,
                                                   "providerCalls": ["hello"]}})
        nodes.append({**run, "rank": rank, "hostname": f"node{rank}", "status": "PASS", "cases": cases,
                      "runtime": {"native": {"/opt/runtime.so": "d" * 64}},
                      "cleanup": [{"reaped": True, "forced": False, "exitedBeforeCleanup": False}]})
    return run, nodes


def test_valid_results_require_real_distinct_nodes_and_local_is_not_cluster():
    run, nodes = fixture_records()
    assert baseline.collect(run, nodes, 0)["status"] == "PASS"
    nodes[1]["hostname"] = nodes[0]["hostname"]
    assert baseline.collect(run, nodes, 0)["status"] == "FAIL"
    run["mode"] = "local"
    assert baseline.collect(run, nodes, 0)["status"] == "LOCAL_PASS"


@pytest.mark.parametrize("mutation", ["exit", "missing", "signature-timeout", "wrong-payload", "identity",
                                     "cleanup", "denied-executed", "private", "fake-pass"])
def test_collector_rejects_false_green(mutation):
    run, nodes = fixture_records()
    exit_code = 0
    if mutation == "exit": exit_code = 1
    elif mutation == "missing": del nodes[1]["cases"]["wrong-root"]
    elif mutation == "signature-timeout": nodes[0]["cases"]["bad-signature"]["reason"] = "InterestTimeout"
    elif mutation == "wrong-payload": nodes[0]["cases"]["service-echo"]["payload"] = "ECHO:wrong"
    elif mutation == "identity": nodes[1]["sifSha256"] = "d" * 64
    elif mutation == "cleanup": nodes[1]["cleanup"][0]["forced"] = True
    elif mutation == "denied-executed": nodes[0]["cases"]["permission-rejection"]["providerCalls"].append("MUST_NOT_EXECUTE")
    elif mutation == "private": run["privateRemoved"] = False
    elif mutation == "fake-pass": nodes[0]["cases"] = {key: {"status": "PASS"} for key in nodes[0]["cases"]}
    assert baseline.collect(run, nodes, exit_code)["status"] == "FAIL"


@pytest.mark.parametrize("field,value", [
    ("provider", "/example/tiger/another-run/provider"), ("status", False),
    ("status", "true"), ("service", "/TIGER_CONTROL"), ("requestId", "/another-request"),
])
def test_collector_binds_ack_to_actual_service_request(field, value):
    run, nodes = fixture_records()
    nodes[0]["cases"]["service-echo"]["acks"][0][field] = value
    assert baseline.collect(run, nodes, 0)["status"] == "FAIL"


@pytest.mark.parametrize("field,value", [
    ("acks", ["observed"]), ("acks", {}), ("requestId", ""), ("requestId", "/"),
    ("selectedProviders", []), ("selectedProviders", ["/another-provider"]),
])
def test_collector_rejects_missing_or_unrelated_selection(field, value):
    run, nodes = fixture_records()
    nodes[0]["cases"]["service-echo"][field] = value
    assert baseline.collect(run, nodes, 0)["status"] == "FAIL"


@pytest.mark.parametrize("boundary,code,termination", [
    ("PUBPARAMS", 134, "EXPECTED_AUTHENTICATION_ABORT"),
    ("PUBPARAMS", -6, "EXPECTED_AUTHENTICATION_ABORT"),
    ("PERMISSION", 0, "NORMAL"),
])
def test_collector_accepts_only_documented_native_rejection_tuples(boundary, code, termination):
    run, nodes = fixture_records()
    rejection = nodes[0]["cases"]["native-trust-rejection"]
    rejection.update(boundary=boundary, exitCode=code, termination=termination)
    assert baseline.collect(run, nodes, 0)["status"] == "PASS"


@pytest.mark.parametrize("mutation", ["missing-boundary", "missing-exit", "missing-termination",
    "generic-crash", "wrong-abort-boundary", "normal-abort", "permission-abort",
    "permission-bool-exit", "string-validation"])
def test_collector_cannot_promote_forged_native_trust_pass(mutation):
    run, nodes = fixture_records()
    rejection = nodes[0]["cases"]["native-trust-rejection"]
    if mutation == "missing-boundary": del rejection["boundary"]
    elif mutation == "missing-exit": del rejection["exitCode"]
    elif mutation == "missing-termination": del rejection["termination"]
    elif mutation == "generic-crash": rejection["exitCode"] = 139
    elif mutation == "wrong-abort-boundary": rejection["boundary"] = "REQUEST"
    elif mutation == "normal-abort": rejection["termination"] = "NORMAL"
    elif mutation == "permission-abort": rejection["boundary"] = "PERMISSION"
    elif mutation == "permission-bool-exit":
        rejection.update(boundary="PERMISSION", exitCode=False, termination="NORMAL")
    elif mutation == "string-validation": rejection["nativeValidationFailure"] = "true"
    assert baseline.collect(run, nodes, 0)["status"] == "FAIL"


def test_child_ownership_reaps_own_process_and_leaves_unrelated_process(tmp_path):
    other = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(60)"], start_new_session=True)
    own = baseline.Processes(tmp_path)
    try:
        child = own.start("child", [sys.executable, "-c", "import time;time.sleep(60)"])
        rows = own.close()
        assert child.poll() is not None and rows[0]["reaped"] and not rows[0]["forced"]
        assert other.poll() is None
        assert own.close() == []
    finally:
        other.terminate()
        other.wait(timeout=3)


def test_dead_child_does_not_pass_ready_file(tmp_path):
    own = baseline.Processes(tmp_path)
    child = own.start("dead", [sys.executable, "-c", "raise SystemExit(3)"])
    child.wait(timeout=5)
    baseline.write_json(tmp_path / "ready.json", {"status": "READY"})
    try:
        with pytest.raises(RuntimeError, match="CHILD_EXIT"):
            baseline.wait_json(tmp_path / "ready.json", 1, own)
    finally:
        own.close()


def test_prepare_does_not_execute_sbatch(tmp_path):
    proc = subprocess.run([sys.executable, str(ROOT / "jobs/baseline/submit.py"), "prepare",
        "--profile", str(ROOT / "profiles/two-node.json"), "--output", str(tmp_path / "absent"),
        "--run-id", "unit-prepare"], capture_output=True, text=True, check=True)
    value = json.loads(proc.stdout)
    assert value["argv"][0] == "sbatch" and "--nodes=2" in value["argv"]
    assert "--export=NONE" in value["argv"]
    assert not (tmp_path / "absent").exists()
    assert "runtime/baseline.py" in value["bundleFiles"] and "apps/service_probe.py" in value["bundleFiles"]


def test_container_environment_does_not_accept_ambient_binds(monkeypatch):
    monkeypatch.setenv("APPTAINER_BINDPATH", "/host/lib:/opt/native")
    monkeypatch.setenv("SINGULARITYENV_PYTHONPATH", "/host/venv")
    monkeypatch.setenv("NDNSF_CONTROLLER_CERT_FILE", "/host/cert")
    env = baseline.container_env()
    assert not any(key.startswith(("APPTAINER", "SINGULARITY", "NDNSF")) for key in env)


@pytest.mark.parametrize("code,log", [(134, "unrelated crash"), (1, "timeout"),
    (0, "Fetched public parameters cannot be authenticated:"),
    (1, "Fetched public parameters cannot be authenticated:")])
def test_native_trust_negative_cannot_accept_generic_failure(code, log):
    with pytest.raises(RuntimeError):
        baseline.native_trust_rejection(code, log, {})


def test_old_image_trust_rejection_records_exact_abort_boundary():
    result = baseline.native_trust_rejection(134, "Fetched public parameters cannot be authenticated: invalid trust", {})
    assert result["boundary"] == "PUBPARAMS"
    assert result["exitCode"] == 134 and result["termination"] == "EXPECTED_AUTHENTICATION_ABORT"
