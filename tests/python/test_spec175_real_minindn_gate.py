"""Contract checks for the Spec175 four-Provider MiniNDN launcher."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py"
PIPELINE_SCRIPT = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
REPO_BOOTSTRAP_SCRIPT = ROOT / "Experiments/spec175_repo_bootstrap.py"
USER_SCRIPT = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py"
G3_SCRIPT = ROOT / "scripts/spec175_g3_manifest.py"


def _module():
    spec = importlib.util.spec_from_file_location("spec175_minindn_gate", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _pipeline_module():
    spec = importlib.util.spec_from_file_location(
        "spec175_llm_pipeline_minindn", PIPELINE_SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _g3_module():
    spec = importlib.util.spec_from_file_location("spec175_g3_manifest", G3_SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_g3_parent_run_root_resolves_r1_r2_r3(tmp_path: Path):
    g3 = _g3_module()
    base = tmp_path / "M01"
    for index in range(1, 4):
        (base / f"r{index}").mkdir(parents=True)

    assert [
        g3.resolve_repetition_dir(base, index)
        for index in range(1, 4)
    ] == [base / "r1", base / "r2", base / "r3"]


def _repo_bootstrap_module():
    spec = importlib.util.spec_from_file_location(
        "spec175_repo_bootstrap", REPO_BOOTSTRAP_SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _user_module():
    user_dir = str(USER_SCRIPT.parent)
    if user_dir not in sys.path:
        sys.path.insert(0, user_dir)
    spec = importlib.util.spec_from_file_location("spec175_llm_pipeline_user", USER_SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_matrix_has_fourteen_cases_and_fault_seed_contract():
    gate = _module()
    assert tuple(gate.CASES) == tuple(f"M{index:02d}" for index in range(1, 15))
    assert gate.CASES["M01"] == "healthy"
    assert gate.CASES["M10"] == "ack-capacity-permutation"
    assert gate.CASES["M11"] == "two-turn-delta-prefill"
    assert gate.CASES["M14"] == "concurrent-parent-cancel-prefetch"


def test_clean_command_has_four_providers_and_disabled_admission(tmp_path: Path):
    gate = _module()
    root = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"
    args = gate.build_parser().parse_args([
        "--case", "M01", "--seed", "1750001",
        "--output-dir", str(tmp_path / "out"),
        "--tiny-fixture-root", str(root), "--dry-run",
    ])
    command = gate.qualification_command(args)
    assert ["--stages", "4"] == command[command.index("--stages"):command.index("--stages") + 2]
    assert command[command.index("--runtime") + 1] == "tiny-onnx"
    assert command[command.index("--seed") + 1] == "1750001"
    assert "--tiny-onnx-fixture-root" in command
    assert "--admission-control" not in command
    assert command[command.index("--initial-sync-settle-s") + 1] == "5"
    topology = Path(command[command.index("--topology-file") + 1]).resolve()
    assert topology == (
        ROOT / "Experiments/Topology/spec175-host-gate.conf").resolve()


def test_g4_command_binds_the_exact_sif_runtime(tmp_path: Path):
    gate = _module()
    root = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"
    sif = tmp_path / "candidate.sif"
    args = gate.build_parser().parse_args([
        "--case", "M01", "--seed", "1750001",
        "--output-dir", str(tmp_path / "out"),
        "--tiny-fixture-root", str(root),
        "--runtime-sif", str(sif),
        "--runtime-apptainer", "/opt/apptainer/1.5.3/bin/apptainer",
    ])
    command = gate.qualification_command(args)
    assert command[command.index("--runtime-sif") + 1] == str(sif)
    assert command[command.index("--runtime-apptainer") + 1] == \
        "/opt/apptainer/1.5.3/bin/apptainer"


def test_full_generation_marker_is_runtime_authoritative():
    gate = _pipeline_module()
    assert gate.uses_full_generation_stage_markers(
        "LLM_PIPELINE_QWEN_FULL_STAGE_START runtime=tiny-onnx")
    assert not gate.uses_full_generation_stage_markers(
        "LLM_PIPELINE_STAGE_INPUT runtime=tiny-onnx")


def test_spec175_cases_enable_native_provider_timing_markers():
    source = PIPELINE_SCRIPT.read_text(encoding="utf-8")
    assert 'args.runtime == "qwen-onnx-cpu-native" or args.spec175_case' in source
    assert 'base_env["NDNSF_DI_RUNTIME_TIMING"] = "1"' in source


def test_spec175_uses_bounded_ndn_log_timeline_instead_of_global_trace():
    source = PIPELINE_SCRIPT.read_text(encoding="utf-8")
    assert 'base_env["NDNSF_CONTROL_TIMING"] = "1"' in source
    assert 'base_env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = "1"' in source
    assert '"*=WARN:"' in source
    assert '"ndn_service_framework.TimelineTrace=WARN:"' in source
    assert '"ndnsf.di.RuntimeEvidence=WARN"' in source
    assert '"ndn_service_framework.ServiceProvider=INFO:"' not in source
    assert '"ndn_service_framework.ServiceUser=INFO"' not in source
    spec175_block = source[source.index("if args.spec175_case:"):]
    assert 'base_env["NDN_LOG"] = "ndn_service_framework.*=TRACE"' not in \
        spec175_block[:spec175_block.index("if args.spec107_diagnostic:")]


def test_spec175_native_timing_records_use_one_ndn_log_sink():
    root = PIPELINE_SCRIPT.parents[1]
    runtime_header = (root / "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp")
    runtime_source = (root / "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.cpp")
    coordinator_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp")
    worker_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.cpp")
    provider_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp")
    fault_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeFaultInjection.cpp")
    wait_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.cpp")
    timeline_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/DiTimelineTrace.hpp")
    dependency_source = (
        root / "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp")
    onnx_source = (
        root / "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp")
    header = runtime_header.read_text(encoding="utf-8")
    sink = runtime_source.read_text(encoding="utf-8")
    coordinator = coordinator_source.read_text(encoding="utf-8")
    worker = worker_source.read_text(encoding="utf-8")
    provider = provider_source.read_text(encoding="utf-8")
    fault = fault_source.read_text(encoding="utf-8")
    wait = wait_source.read_text(encoding="utf-8")
    timeline = timeline_source.read_text(encoding="utf-8")
    dependency = dependency_source.read_text(encoding="utf-8")
    onnx = onnx_source.read_text(encoding="utf-8")

    assert "logRuntimeEvidence(const std::string& record)" in header
    for helper in ("logRuntimeTrace", "logRuntimeInfo", "logRuntimeWarn", "logRuntimeError"):
        assert f"{helper}(const std::string& record)" in header
    assert "NDN_LOG_INIT(ndnsf.di.RuntimeEvidence);" in sink
    assert "NDN_LOG_WARN(record);" in sink
    for source in (coordinator, worker, provider, fault, wait, timeline, dependency):
        assert "std::cout" not in source
        assert "std::cerr" not in source
        assert "std::clog" not in source
    assert "logRuntimeTrace(record.str());" in coordinator
    assert "logRuntimeTrace(\"NDNSF_DI_WORKER event=enqueue_ready\")" in worker
    assert "logRuntimeWarn(record.str());" in provider
    assert "logRuntimeError(record.str());" in provider
    assert "logRuntimeWarn(record.str());" in fault
    assert wait.count("logRuntimeWarn(record.str());") >= 3
    assert "logRuntimeEvidence(record.str());" in timeline
    assert dependency.count("logRuntimeEvidence(record.str());") >= 2
    for marker in (
            "NDNSF_DI_PROVIDER_HANDLER_TIMING",
            "NDNSF_DI_DEPENDENCY_INPUT_TIMING",
            "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING"):
        assert marker in provider
    assert provider.count("logRuntimeEvidence(record.str());") >= 4
    assert "NDNSF_DI_ONNX_TIMING" in onnx
    assert "logRuntimeEvidence(record.str());" in onnx


def test_spec175_runtime_log_filter_is_verified_in_a_subprocess():
    """The named component must honor severity filters in a real process."""
    binary = ROOT / "build/unit-tests"
    if not binary.exists():
        pytest.fail("build/unit-tests is required for the logging subprocess gate")

    test_name = "DiNativeRuntimeLogging/EmitsBoundedRecordsForFilterRegression"

    def run_with_filter(filter_value: str) -> str:
        env = dict(os.environ)
        env["NDN_LOG"] = filter_value
        result = subprocess.run(
            [str(binary), f"--run_test={test_name}", "--log_level=nothing"],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout + result.stderr

    warn_output = run_with_filter("*=ERROR:ndnsf.di.RuntimeEvidence=WARN")
    assert "NDNSF_DI_LOG_FILTER_TEST level=WARN" in warn_output
    assert "NDNSF_DI_LOG_FILTER_TEST level=ERROR" in warn_output
    assert "NDNSF_DI_LOG_FILTER_TEST level=TRACE" not in warn_output
    assert "NDNSF_DI_LOG_FILTER_TEST level=INFO" not in warn_output

    trace_output = run_with_filter("*=WARN:ndnsf.di.RuntimeEvidence=TRACE")
    for level in ("TRACE", "INFO", "WARN", "ERROR"):
        assert f"NDNSF_DI_LOG_FILTER_TEST level={level}" in trace_output
    marker_lines = [
        line for line in trace_output.splitlines()
        if "NDNSF_DI_LOG_FILTER_TEST" in line
    ]
    assert len(marker_lines) == 4
    assert all(line.count("NDNSF_DI_LOG_FILTER_TEST") == 1
               for line in marker_lines)


def test_spec175_assignment_evidence_survives_bounded_warn_filter():
    service_user = (
        PIPELINE_SCRIPT.parents[1] / "ndn-service-framework/ServiceUser.cpp"
    ).read_text(encoding="utf-8")
    marker = service_user.index("NDNSF_COLLAB_ASSIGNMENT_SELECTED")
    assert "NDN_LOG_WARN(" in service_user[marker - 80:marker]


def test_spec175_lifecycle_evidence_is_sorted_and_redacts_sensitive_fields(
        tmp_path: Path):
    gate = _module()
    (tmp_path / "provider.log").write_text(
        "0 WARN NDNSF_CONTROL_TIMING role=provider event=ack_handler_done "
        "steady_us=20 timestamp_us=200 requestId=/r status=true token=secret\n"
        "0 WARN NDNSF_CONTROL_TIMING role=provider event=ack_handler_start "
        "steady_us=10 timestamp_us=100 requestId=/r serviceName=/S\n",
        encoding="utf-8",
    )
    summary = gate.write_request_lifecycle_evidence(tmp_path)
    rows = [json.loads(line) for line in
            (tmp_path / "spec175-request-lifecycle.jsonl").read_text().splitlines()]
    assert summary["status"] == "PASS"
    assert summary["eventCount"] == 2
    assert summary["requestCount"] == 1
    assert [row["event"] for row in rows] == [
        "ack_handler_start", "ack_handler_done"]
    assert rows[1]["fields"] == {"status": "true"}
    assert "secret" not in json.dumps(rows)


def test_spec175_lifecycle_evidence_redacts_prompt_answer_logits_and_state(
        tmp_path: Path):
    gate = _module()
    (tmp_path / "provider.log").write_text(
        "0 WARN NDNSF_CONTROL_TIMING role=provider event=bounded "
        "steady_us=10 timestamp_us=100 requestId=/r "
        "promptText=prompt-secret answerText=answer-secret logits=logit-secret "
        "tokenKey=token-secret keyBytes=key-secret stateTensor=state-secret "
        "tensorData=tensor-secret stateTensorBytesOnNdn=0 status=true\n",
        encoding="utf-8",
    )
    summary = gate.write_request_lifecycle_evidence(tmp_path)
    rows = [json.loads(line) for line in
            (tmp_path / "spec175-request-lifecycle.jsonl").read_text().splitlines()]
    assert summary["status"] == "PASS"
    assert rows[0]["fields"] == {"stateTensorBytesOnNdn": "0", "status": "true"}
    serialized = json.dumps(rows)
    for value in ("prompt-secret", "answer-secret", "logit-secret", "token-secret",
                  "key-secret", "state-secret", "tensor-secret"):
        assert value not in serialized


def test_spec175_requires_fib_snapshot_and_post_user_join_svs_settle():
    source = PIPELINE_SCRIPT.read_text(encoding="utf-8")
    user_source = USER_SCRIPT.read_text(encoding="utf-8")
    repo_source = REPO_BOOTSTRAP_SCRIPT.read_text(encoding="utf-8")
    assert "write_spec175_nfd_route_snapshot" in source
    assert '"spec175-nfd-route-snapshot.json"' in source
    assert "SPEC175_NFD_ROUTE_OR_STRATEGY_MISSING" in source
    assert "install_spec175_svs_group_fanout" in source
    assert 'rh.addOrigin(ndn.net.hosts, [GROUP_IDENTITY])' not in source
    assert source.index("install_spec175_svs_group_fanout(", source.index(
        "ndn.start()")) < source.index("write_spec175_nfd_route_snapshot(",
                                       source.index("ndn.start()"))
    assert source.index("write_spec175_nfd_route_snapshot(") < source.index(
        "Provider process readiness complete") < source.index("user_log = OUT")
    assert "--spec175-case requires --initial-sync-settle-s 5.0" in source
    assert "--initial-sync-settle-s " in source
    assert "NDNSF_DI_PROVIDER_SVS_SETTLED" not in source
    assert "NDNSF_DI_USER_SVS_SETTLED" in user_source
    assert 'base_env["NDNSF_SVS_PERIODIC_SYNC_MS"] = "1000"' in source
    assert '"svsPeriodicSyncMs": int(' in source
    assert '" --initial-sync-settle-s 5"' in source
    assert "wait_for_initial_sync(args)" in repo_source
    assert "NDNSF_DI_REPO_USER_SVS_SETTLED" in repo_source
    client_start = user_source.index("client = APPClient.from_config(")
    assert client_start < user_source.index(
        "NDNSF_DI_USER_SVS_SETTLED", client_start) < user_source.index(
            "_run_spec175_real_m11_case(", client_start)


def test_spec175_nfd_snapshot_requires_group_routes_but_keeps_repo_diagnostic(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    gate = _pipeline_module()
    nodes = [SimpleNamespace(name="p0"), SimpleNamespace(name="u")]
    ndn = SimpleNamespace(net=SimpleNamespace(hosts=nodes))

    def node_cmd(_node, command):
        if "strategy list" in command:
            return (
                "/example/llm-pipeline multicast\n"
                "/example/llm-pipeline/group multicast\n"
            )
        return "/example/llm-pipeline\n/example/llm-pipeline/group\n"

    monkeypatch.setattr(gate.perf, "node_cmd", node_cmd)
    path = tmp_path / "route-snapshot.json"
    snapshot = gate.write_spec175_nfd_route_snapshot(
        ndn,
        path,
        ("/example/llm-pipeline", "/example/llm-pipeline/group"),
        ("/NDNSF/DistributedRepo",),
    )
    reference = gate.summarize_spec175_nfd_route_snapshot(snapshot, path)

    assert snapshot["status"] == "PASS"
    assert snapshot["missing"] == {}
    assert all(
        not node["prefixObserved"]["/NDNSF/DistributedRepo"]
        for node in snapshot["nodes"].values()
    )
    assert reference["status"] == "PASS"
    assert reference["path"] == str(path.resolve())
    assert reference["sha256"] == (
        "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest())
    assert "nodes" not in reference


def test_spec175_nfd_snapshot_fails_closed_when_group_route_is_missing(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    gate = _pipeline_module()
    ndn = SimpleNamespace(net=SimpleNamespace(
        hosts=[SimpleNamespace(name="p3")]))

    def node_cmd(_node, command):
        if "strategy list" in command:
            return "/example/llm-pipeline multicast\n"
        return "/example/llm-pipeline\n"

    monkeypatch.setattr(gate.perf, "node_cmd", node_cmd)
    path = tmp_path / "route-snapshot.json"
    with pytest.raises(
            RuntimeError, match="SPEC175_NFD_ROUTE_OR_STRATEGY_MISSING"):
        gate.write_spec175_nfd_route_snapshot(
            ndn,
            path,
            ("/example/llm-pipeline", "/example/llm-pipeline/group"),
        )
    snapshot = json.loads(path.read_text(encoding="utf-8"))
    assert snapshot["status"] == "FAIL"
    assert snapshot["missing"] == {
        "p3": ["/example/llm-pipeline/group"]}


def test_spec175_nfd_snapshot_rejects_incomplete_group_fanout(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    gate = _pipeline_module()
    ndn = SimpleNamespace(net=SimpleNamespace(
        hosts=[SimpleNamespace(name="a")]))

    def node_cmd(_node, command):
        if "strategy list" in command:
            return (
                "/example/llm-pipeline multicast\n"
                "/example/llm-pipeline/group multicast\n"
            )
        if "fib list" in command:
            return (
                "/example/llm-pipeline nexthops={faceid=10 (cost=10)}\n"
                "/example/llm-pipeline/group "
                "nexthops={faceid=279 (cost=10)}\n"
            )
        return "/example/llm-pipeline\n/example/llm-pipeline/group\n"

    monkeypatch.setattr(gate.perf, "node_cmd", node_cmd)
    path = tmp_path / "route-snapshot.json"
    with pytest.raises(
            RuntimeError, match="SPEC175_NFD_ROUTE_OR_STRATEGY_MISSING"):
        gate.write_spec175_nfd_route_snapshot(
            ndn,
            path,
            ("/example/llm-pipeline", "/example/llm-pipeline/group"),
            expected_next_hops={
                "a": {"/example/llm-pipeline/group": ("279", "281")},
            },
        )
    snapshot = json.loads(path.read_text(encoding="utf-8"))
    assert snapshot["status"] == "FAIL"
    assert snapshot["missing"] == {}
    assert snapshot["missingNextHops"] == {
        "a": {"/example/llm-pipeline/group": ["281"]}}


def test_spec175_nfdc_mutation_retries_only_transient_authorization_rejection(
        monkeypatch: pytest.MonkeyPatch):
    gate = _pipeline_module()
    outputs = iter([
        "Error 403 when adding route: authorization rejected\n"
        "__SPEC175_NFDC_RC__=1\n",
        "route-add-accepted\n__SPEC175_NFDC_RC__=0\n",
    ])
    sleeps = []
    monkeypatch.setattr(gate.perf, "node_cmd", lambda _node, _command: next(outputs))
    monkeypatch.setattr(gate.time, "sleep", sleeps.append)

    result = gate.run_spec175_nfdc_mutation(
        SimpleNamespace(name="a"),
        "nfdc route add /example/llm-pipeline/group 287 origin 255 cost 10",
    )

    assert result["status"] == "PASS"
    assert result["attemptCount"] == 2
    assert result["authorizationRetryCount"] == 1
    assert [attempt["returnCode"] for attempt in result["attempts"]] == [1, 0]
    assert sleeps == [gate.SPEC175_NFDC_RETRY_DELAY_S] * 2


def test_spec175_nfdc_mutation_fails_closed_after_bounded_403_retries(
        monkeypatch: pytest.MonkeyPatch):
    gate = _pipeline_module()
    calls = []

    def node_cmd(_node, command):
        calls.append(command)
        return (
            "Error 403 when adding route: authorization rejected\n"
            "__SPEC175_NFDC_RC__=1\n"
        )

    monkeypatch.setattr(gate.perf, "node_cmd", node_cmd)
    monkeypatch.setattr(gate.time, "sleep", lambda _delay: None)
    result = gate.run_spec175_nfdc_mutation(
        SimpleNamespace(name="a"), "nfdc route add /group 287")

    assert result["status"] == "FAIL"
    assert result["attemptCount"] == gate.SPEC175_NFDC_MAX_ATTEMPTS
    assert result["authorizationRetryCount"] == \
        gate.SPEC175_NFDC_MAX_ATTEMPTS - 1
    assert len(calls) == gate.SPEC175_NFDC_MAX_ATTEMPTS


def test_spec175_nfdc_mutation_does_not_retry_non_authorization_error(
        monkeypatch: pytest.MonkeyPatch):
    gate = _pipeline_module()
    calls = []

    def node_cmd(_node, command):
        calls.append(command)
        return "Error 404 when adding route: face not found\n__SPEC175_NFDC_RC__=1\n"

    monkeypatch.setattr(gate.perf, "node_cmd", node_cmd)
    monkeypatch.setattr(gate.time, "sleep", lambda _delay: None)
    result = gate.run_spec175_nfdc_mutation(
        SimpleNamespace(name="a"), "nfdc route add /group 999")

    assert result["status"] == "FAIL"
    assert result["attemptCount"] == 1
    assert result["authorizationRetryCount"] == 0
    assert len(calls) == 1


def test_spec175_repo_fetches_start_before_waiting_for_any_fetch():
    source = PIPELINE_SCRIPT.read_text(encoding="utf-8")
    launch = source.index("fetch_jobs = []")
    append = source.index("fetch_jobs.append", launch)
    collect = source.index("fetched_artifacts = []", append)
    wait = source.index("fetch_proc.wait", collect)
    assert launch < append < collect < wait


def test_spec175_repo_publisher_probes_same_identity_route_before_release(
        tmp_path: Path):
    repo = _repo_bootstrap_module()

    class FakeResponse:
        status = True
        payload = b'{"repoNode":"/NDNSF/DistributedRepo/Node/0","schema":"repo"}'
        error = ""
        data_name = "/NDNSF/DistributedRepo/Node/0/NDNSF/RESPONSE/x"
        signer_certificate = "/cert/repo"
        wire_digest = "sha256:" + "a" * 64

    class FakeUser:
        def __init__(self):
            self.calls = []

        def request_service(self, *args, **kwargs):
            self.calls.append((args, kwargs))
            return FakeResponse()

    service_user = FakeUser()
    backend = SimpleNamespace(control=SimpleNamespace(service_user=service_user))
    output = tmp_path / "probe.json"
    args = SimpleNamespace(
        user="/NDNSF/DistributedRepo/Publisher",
        ack_timeout_ms=5000,
        timeout_ms=60000,
        probe_timeout_ms=None,
        probe_output=str(output),
    )
    report = repo.run_live_service_probe(backend, args)
    assert report["status"] == "PASS"
    assert report["route"]["service"] == "/NDNSF/DistributedRepo/Object/v1/STATUS"
    assert report["route"]["provider"] == "/NDNSF/DistributedRepo/Node/0"
    assert report["request"]["nonMutating"] is True
    assert service_user.calls[0][0][0] == report["route"]["service"]
    assert json.loads(output.read_text()) == report


def test_spec175_repo_route_probe_fails_closed_without_provider_response(
        tmp_path: Path):
    repo = _repo_bootstrap_module()

    class FakeResponse:
        status = False
        payload = b""
        error = "timeout"
        data_name = ""
        signer_certificate = ""
        wire_digest = ""

    class FakeUser:
        def request_service(self, *args, **kwargs):
            return FakeResponse()

    args = SimpleNamespace(
        user="/publisher", ack_timeout_ms=10, timeout_ms=20,
        probe_timeout_ms=20, probe_output=str(tmp_path / "probe.json"),
    )
    report = repo.run_live_service_probe(
        SimpleNamespace(control=SimpleNamespace(service_user=FakeUser())), args)
    assert report["status"] == "FAIL"
    assert report["failureReason"] == "REPO_SERVICE_ROUTE_NOT_READY"
    assert json.loads(Path(args.probe_output).read_text())["status"] == "FAIL"


def test_spec175_repo_route_probe_retries_transient_no_response(tmp_path: Path):
    repo = _repo_bootstrap_module()

    class FakeResponse:
        payload = b'{"repoNode":"/NDNSF/DistributedRepo/Node/0","schema":"repo"}'
        error = ""
        data_name = "/NDNSF/DistributedRepo/Node/0/NDNSF/RESPONSE/x"
        signer_certificate = "/cert/repo"
        wire_digest = "sha256:" + "a" * 64

        def __init__(self, status: bool):
            self.status = status

    class FakeUser:
        def __init__(self):
            self.calls = []

        def request_service(self, *args, **kwargs):
            self.calls.append((args, kwargs))
            return FakeResponse(status=len(self.calls) > 1)

    service_user = FakeUser()
    args = SimpleNamespace(
        user="/publisher", ack_timeout_ms=10, timeout_ms=20,
        probe_timeout_ms=100, probe_attempt_timeout_ms=20,
        probe_retries=2, probe_retry_backoff_ms=0,
        probe_output=str(tmp_path / "probe.json"),
    )
    report = repo.run_live_service_probe(
        SimpleNamespace(control=SimpleNamespace(service_user=service_user)),
        args,
    )
    assert report["status"] == "PASS"
    assert report["attemptCount"] == 2
    assert len(report["attempts"]) == 2
    assert len(service_user.calls) == 2
    assert service_user.calls[0][1]["request_id"] != service_user.calls[1][1]["request_id"]


def test_spec175_repo_route_probe_retry_exhaustion_remains_fail_closed(
        tmp_path: Path):
    repo = _repo_bootstrap_module()

    class FakeResponse:
        status = False
        payload = b""
        error = "timeout"
        data_name = ""
        signer_certificate = ""
        wire_digest = ""

    class FakeUser:
        def __init__(self):
            self.calls = 0

        def request_service(self, *args, **kwargs):
            self.calls += 1
            return FakeResponse()

    service_user = FakeUser()
    args = SimpleNamespace(
        user="/publisher", ack_timeout_ms=10, timeout_ms=20,
        probe_timeout_ms=100, probe_attempt_timeout_ms=20,
        probe_retries=2, probe_retry_backoff_ms=0,
        probe_output=str(tmp_path / "probe.json"),
    )
    report = repo.run_live_service_probe(
        SimpleNamespace(control=SimpleNamespace(service_user=service_user)),
        args,
    )
    assert report["status"] == "FAIL"
    assert report["failureReason"] == "REPO_SERVICE_ROUTE_NOT_READY"
    assert report["attemptCount"] == 3
    assert service_user.calls == 3


def test_spec175_repo_route_probe_is_before_publication_barrier():
    source = REPO_BOOTSTRAP_SCRIPT.read_text(encoding="utf-8")
    probe = source.index("run_live_service_probe(backend, args)")
    barrier = source.index("wait_for_publication_start(args)")
    assert probe < barrier

    pipeline = PIPELINE_SCRIPT.read_text(encoding="utf-8")
    command = pipeline.index('" --probe-output "')
    ready = pipeline.index("NDNSF_DI_SPEC175_REPO_ROUTE_PROBE_PASS", command)
    release = pipeline.index("release_file_barrier(", ready)
    assert command < ready < release


def test_tiny_stream_uses_and_verifies_the_campaign_request_id():
    source = USER_SCRIPT.read_text(encoding="utf-8")
    expected_tokens = source.index(
        'expected_tokens = list(expected_doc.get("generatedTokens", ()))')
    branch = source.rindex(
        'elif args.runtime == TINY_ONNX_RUNTIME:', 0, expected_tokens)
    end = source.index('else:', branch)
    body = source[branch:end]
    assert "tiny_wire_request_id = _campaign_wire_request_id(" in body
    assert "wire_request_id=tiny_wire_request_id" in body
    assert "tiny streamed response request ID mismatch" in body


def test_expected_terminal_stream_cleans_up_before_return_and_client_shutdown():
    """A timeout oracle must cancel the native stream before the process exits."""
    user = _user_module()

    class FakeInvocation:
        request_id = "/spec175-M08-test"

        def __init__(self):
            self.cancel_count = 0

        def cancel(self):
            self.cancel_count += 1

    class FakeServiceUser:
        def __init__(self, invocation):
            self.invocation = invocation

        def request_service_streaming(self, *args, **kwargs):
            def fail_later():
                time.sleep(0.01)
                kwargs["on_error"]({
                    "code": 6,
                    "message": "stream event gap exceeded retry budget",
                })

            thread = threading.Thread(target=fail_later, daemon=True)
            thread.start()
            return self.invocation

    class FakeClient:
        def __init__(self, invocation):
            self.service_user = FakeServiceUser(invocation)

    invocation = FakeInvocation()
    result = user._run_tiny_onnx_stream(
        FakeClient(invocation),
        SimpleNamespace(
            automatic_planning_manifest="",
            timeout_ms=1000,
            max_new_tokens=1,
            spec175_fault_case="M08",
        ),
        b"payload", [],
        wire_request_id="spec175-M08-test",
    )
    assert getattr(result, "expected_terminal", False)
    assert invocation.cancel_count == 1

    source = USER_SCRIPT.read_text(encoding="utf-8")
    expected_return = source.index(
        'if getattr(result, "expected_terminal", False):')
    shutdown = source.index("client.shutdown(wait=True)", expected_return)
    assert shutdown < source.index("return 0", expected_return)


def test_normal_stream_owner_has_synchronous_shutdown_finally():
    """Normal completion must not leave native Face teardown to interpreter exit."""
    source = USER_SCRIPT.read_text(encoding="utf-8")
    normal_loop = source.index("    try:\n        while True:", source.index("def main"))
    normal_finally = source.index("    finally:", normal_loop)
    normal_shutdown = source.index("client.shutdown(wait=True)", normal_finally)
    assert normal_shutdown < source.index("    return 0", normal_shutdown)
    assert "_finish_deployment_workflow(client, deployment_workflow, args)" in source[normal_loop:normal_finally]


def test_native_user_stop_releases_gil_while_joining_face_thread():
    """Timeout cleanup must let an in-flight Python callback finish."""
    source = (ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text(
        encoding="utf-8")
    stop = source.index('.def("stop", &NativeServiceUser::stop,')
    binding = source[stop:stop + 180]
    assert "py::call_guard<py::gil_scoped_release>()" in binding


def test_pre_network_expected_rejection_does_not_require_native_handle():
    """A coordinator rejection before Request creation has nothing to cancel."""
    user = _user_module()

    class FakeServiceUser:
        def request_service_streaming(self, *args, **kwargs):
            raise RuntimeError("ConversationCheckpointInvalid")

    class FakeClient:
        service_user = FakeServiceUser()

    args = SimpleNamespace(
        automatic_planning_manifest="",
        timeout_ms=1000,
        max_new_tokens=1,
        spec175_fault_case="M13",
    )
    with pytest.raises(RuntimeError, match="no native invocation handle"):
        user._run_tiny_onnx_stream(
            FakeClient(), args, b"payload", [],
            wire_request_id="spec175-M13-forged-checkpoint",
        )
    result = user._run_tiny_onnx_stream(
        FakeClient(), args, b"payload", [],
        wire_request_id="spec175-M13-forged-checkpoint",
        pre_network_rejection=True,
    )
    assert getattr(result, "expected_terminal", False)
    assert getattr(result, "request_id", "") == ""


def test_spec175_provider_timing_binds_repeated_role_spans(tmp_path: Path):
    pipeline = _pipeline_module()
    log = tmp_path / "provider.log"
    log.write_text(
        "\n".join([
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session=s1 "
            "role=/LLM/Pipeline/Stage/0 start_epoch_ms=100 queue_wait_ms=1",
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=end session=s1 "
            "role=/LLM/Pipeline/Stage/0 start_epoch_ms=100 end_epoch_ms=104 "
            "queue_wait_ms=1 handler_ms=4 total_ms=4",
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session=s1 "
            "role=/LLM/Pipeline/Stage/0 start_epoch_ms=105 queue_wait_ms=0",
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=end session=s1 "
            "role=/LLM/Pipeline/Stage/0 start_epoch_ms=105 end_epoch_ms=111 "
            "queue_wait_ms=0 handler_ms=6 total_ms=6",
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session=s1 "
            "role=/LLM/Pipeline/Stage/1 start_epoch_ms=100 queue_wait_ms=2",
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=end session=s1 "
            "role=/LLM/Pipeline/Stage/1 start_epoch_ms=100 end_epoch_ms=108 "
            "queue_wait_ms=2 handler_ms=8 total_ms=8",
        ]) + "\n",
        encoding="utf-8")
    report_path = pipeline.write_spec175_provider_timing(
        [log], tmp_path,
        required_roles=(
            "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"),
    )
    report = __import__("json").loads(report_path.read_text())
    assert report["schema"] == "ndnsf-di-spec175-provider-timing-v1"
    assert report["spanCount"] == 3
    assert report["byRole"]["/LLM/Pipeline/Stage/0"]["count"] == 2


def test_spec175_provider_timing_rejects_unmatched_or_missing_role(tmp_path: Path):
    pipeline = _pipeline_module()
    log = tmp_path / "provider.log"
    log.write_text(
        "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session=s1 "
        "role=/LLM/Pipeline/Stage/0 start_epoch_ms=100\n",
        encoding="utf-8")
    with pytest.raises(RuntimeError, match="UNMATCHED_START"):
        pipeline.write_spec175_provider_timing(
            [log], tmp_path,
            required_roles=("/LLM/Pipeline/Stage/0",))


def test_spec175_provider_timing_retains_expected_open_fault_span(tmp_path: Path):
    pipeline = _pipeline_module()
    log = tmp_path / "provider.log"
    log.write_text(
        "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session=s1 "
        "role=/LLM/Pipeline/Stage/0 start_epoch_ms=100 queue_wait_ms=2\n",
        encoding="utf-8")
    report_path = pipeline.write_spec175_provider_timing(
        [log], tmp_path,
        required_roles=("/LLM/Pipeline/Stage/0",),
        allow_expected_incomplete=True,
    )
    report = __import__("json").loads(report_path.read_text())
    assert report["timingCompleteness"] == "expected-incomplete"
    assert report["spanCount"] == 0
    assert report["openSpanCount"] == 1
    assert report["openSpans"][0]["status"] == "OPEN_EXPECTED_FAILURE"
    assert report["openSpans"][0]["providerLog"] == "provider.log"


def test_spec175_provider_timing_rejects_malformed_dependency_number(tmp_path: Path):
    pipeline = _pipeline_module()
    log = tmp_path / "provider.log"
    log.write_text(
        "\n".join([
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session=s1 "
            "role=/LLM/Pipeline/Stage/0 start_epoch_ms=100 queue_wait_ms=1",
            "NDNSF_DI_PROVIDER_HANDLER_TIMING event=end session=s1 "
            "role=/LLM/Pipeline/Stage/0 start_epoch_ms=100 end_epoch_ms=104 "
            "handler_ms=4",
            "NDNSF_DI_DEPENDENCY_INPUT_TIMING session=s1 "
            "role=/LLM/Pipeline/Stage/1 producer=/LLM/Pipeline/Stage/0 "
            "scope=s bad_bytes=not-a-number",
        ]) + "\n",
        encoding="utf-8")
    # Unknown fields are ignored by design; the required role span remains
    # valid and the dependency marker is metadata-only.
    report_path = pipeline.write_spec175_provider_timing(
        [log], tmp_path,
        required_roles=("/LLM/Pipeline/Stage/0",))
    assert __import__("json").loads(report_path.read_text())["dependencyInputSpanCount"] == 1

    log.write_text(
        log.read_text(encoding="utf-8").replace(
            "bad_bytes=not-a-number", "bytes=not-a-number"),
        encoding="utf-8")
    with pytest.raises(RuntimeError, match="FIELD_INVALID"):
        pipeline.write_spec175_provider_timing(
            [log], tmp_path,
            required_roles=("/LLM/Pipeline/Stage/0",))


def test_pipeline_sif_prefix_never_uses_host_source(monkeypatch, tmp_path: Path):
    pipeline = _pipeline_module()
    sif = tmp_path / "candidate.sif"
    sif.write_bytes(b"candidate")
    out = tmp_path / "out"
    out.mkdir()
    minindn = tmp_path / "minindn"
    minindn.mkdir()
    monkeypatch.setattr(pipeline, "SIF_RUNTIME_SIF", sif)
    monkeypatch.setattr(
        pipeline, "SIF_RUNTIME_APPTAINER",
        Path("/opt/apptainer/1.5.3/bin/apptainer"))
    monkeypatch.setattr(pipeline, "OUT", out)
    monkeypatch.setattr(pipeline, "MININDN_ROOT", minindn)
    monkeypatch.setattr(
        pipeline._sif_bind_args, "fixture_root",
        ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1", raising=False)
    prefix = pipeline.python_process_prefix({
        "PYTHONPATH": str(ROOT),
        "NDN_LOG": "ndn_service_framework.*=INFO",
    })
    assert "apptainer" in prefix and "exec" in prefix and "--cleanenv" in prefix
    assert "/opt/venv/bin/python" in prefix
    assert f"PYTHONPATH={ROOT}" not in prefix
    assert "--pwd " + str(ROOT) not in prefix
    assert pipeline.runtime_source_path(pipeline.LLM_DIR / "user.py") == (
        "/opt/ndnsf-di/replay/repo/examples/python/NDNSF-DistributedInference/"
        "llm_pipeline/user.py")


def test_stale_nfd_socket_is_removed_only_when_unowned(tmp_path: Path):
    pipeline = _pipeline_module()
    socket_dir = tmp_path / "nfd"
    socket_dir.mkdir()
    stale = socket_dir / "p0.sock"
    stale_listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    stale_listener.bind(str(stale))
    stale_listener.listen(1)
    stale_listener.close()
    assert pipeline.cleanup_unused_nfd_sockets(["p0"], socket_dir) == [
        str(stale.resolve())
    ]
    assert not stale.exists()

    active = socket_dir / "p1.sock"
    active_listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    active_listener.bind(str(active))
    active_listener.listen(1)
    try:
        with pytest.raises(RuntimeError, match="active NFD socket"):
            pipeline.cleanup_unused_nfd_sockets(["p1"], socket_dir)
        assert active.exists()
    finally:
        active_listener.close()
        active.unlink(missing_ok=True)


@pytest.mark.parametrize("case", [f"M{index:02d}" for index in range(1, 15)])
def test_every_case_uses_registered_production_runner_contract(
        case: str, tmp_path: Path):
    gate = _module()
    root = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"
    args = gate.build_parser().parse_args([
        "--case", case, "--seed", "1750002",
        "--output-dir", str(tmp_path / "out"),
        "--tiny-fixture-root", str(root),
    ])
    command = gate.qualification_command(args)
    assert command[command.index("--spec175-case") + 1] == case
    assert command[command.index("--ack-timeout-ms") + 1] == "1500"
    assert command[command.index("--timeout-ms") + 1] == (
        "2500" if case == "M08" else "60000")
    assert command[command.index("--stages") + 1] == "4"
    assert command[command.index("--runtime") + 1] == "tiny-onnx"


def test_m10_rotates_provider_role_ownership_without_duplication():
    pipeline = _pipeline_module()
    baseline = pipeline.spec175_provider_role_indices("M01", 4)
    rotated = pipeline.spec175_provider_role_indices("M10", 4)
    assert baseline == (0, 1, 2, 3)
    assert rotated == (1, 2, 3, 0)
    assert sorted(rotated) == [0, 1, 2, 3]


def test_m10_role_policy_matches_provider_command_role_map(tmp_path: Path):
    pipeline = _pipeline_module()
    pipeline.configure_spec175_host_layout()
    policy = tmp_path / "policy.yaml"
    policy.write_text(
        "services:\n"
        "- name: /AI/LLM/Pipeline/Fake\n"
        "  roles: [/LLM/Pipeline/Stage/0, /LLM/Pipeline/Stage/1, "
        "/LLM/Pipeline/Stage/2, /LLM/Pipeline/Stage/3]\n"
        "  providers:\n"
        "  - identity: /example/llm-pipeline/provider\n"
        "    roles: [/LLM/Pipeline/Stage/0]\n"
        "  - identity: /example/llm-pipeline/provider/1\n"
        "    roles: [/LLM/Pipeline/Stage/1]\n"
        "  - identity: /example/llm-pipeline/provider/2\n"
        "    roles: [/LLM/Pipeline/Stage/2]\n"
        "  - identity: /example/llm-pipeline/provider/3\n"
        "    roles: [/LLM/Pipeline/Stage/3]\n",
        encoding="utf-8")
    pipeline.configure_spec175_role_policy(policy, (1, 2, 3, 0))
    import yaml
    service = yaml.safe_load(policy.read_text(encoding="utf-8"))["services"][0]
    assert [item["roles"] for item in service["providers"]] == [
        ["/LLM/Pipeline/Stage/1"],
        ["/LLM/Pipeline/Stage/2"],
        ["/LLM/Pipeline/Stage/3"],
        ["/LLM/Pipeline/Stage/0"],
    ]


def test_m10_local_artifact_uses_physical_provider_index():
    source = PIPELINE_SCRIPT.read_text(encoding="utf-8")
    assert 'selection_bundle["localArtifacts"][stage_index]' in source
    assert 'selection_bundle["localArtifacts"][role_index]' not in source


def test_spec175_forces_targeted_prefetch_off_for_the_host_gate():
    pipeline = _pipeline_module()
    args = pipeline.build_parser().parse_args([
        "--spec175-case", "M01",
        "--selection-dataflow-v3",
    ])
    base_env = {"NDNSF_SELECTION_TARGETED_PREFETCH": "1"}

    assert not pipeline.apply_selection_targeted_prefetch_policy(
        args, base_env,
        {"NDNSF_SELECTION_TARGETED_PREFETCH": "1"})
    assert base_env["NDNSF_SELECTION_TARGETED_PREFETCH"] == "0"


def test_tiny_cpu_v3_offer_uses_cpu_topology_without_literal_cpu(
        tmp_path: Path):
    """The production tiny-ONNX helper must emit the canonical CPU shape."""
    provider_path = USER_SCRIPT.parent / "provider.py"
    user_dir = str(provider_path.parent)
    if user_dir not in sys.path:
        sys.path.insert(0, user_dir)
    provider_spec = importlib.util.spec_from_file_location(
        "spec175_llm_pipeline_provider", provider_path)
    assert provider_spec is not None and provider_spec.loader is not None
    pipeline_provider = importlib.util.module_from_spec(provider_spec)
    provider_spec.loader.exec_module(pipeline_provider)
    key = tmp_path / "offer.key"
    key.write_bytes(b"k" * 32)

    class FakeProvider:
        provider_boot_epoch = 17

    args = SimpleNamespace(
        selection_dataflow_v3=True,
        provider_identity="/example/provider",
        selection_signing_key_file=str(key),
        device="auto",
        require_cuda=False,
    )
    result = pipeline_provider._selection_v3_for_tiny_onnx(
        FakeProvider(), args)
    issuer = result["selection_offer_issuer_v3"]
    from ndnsf_distributed_inference.sdk.placement import (
        ExecutionDisposition, ProviderOfferV3,
    )
    now_ms = int(__import__("time").time() * 1000)
    decision = issuer.issue(
        request_id="request-1", attempt=1,
        model_digest="sha256:" + "1" * 64,
        deadline_ms=now_ms + 5000,
        accepted_roles=("/LLM/Pipeline/Stage/0",),
        backends=("onnxruntime",),
        execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        preparation_accepted=True,
    )
    offer = ProviderOfferV3.from_bytes(decision.payload)
    assert offer.topology.backend == "cpu"
    assert offer.topology.devices == ()


def test_frozen_topology_has_dedicated_process_nodes_and_link_contract():
    topology = (
        ROOT / "Experiments/Topology/spec175-host-gate.conf").read_text(
            encoding="utf-8")
    for node in ("c", "repo", "u", "a", "p0", "p1", "p2", "p3"):
        assert f"{node}:" in topology
    links = [
        line for line in topology.splitlines()
        if ":a " in line and not line.startswith("[")
    ]
    assert len(links) == 7
    assert all(
        "delay=10ms" in line
        and "bw=100" in line
        and "max_queue_size=1000" in line
        and "loss=0" in line
        for line in links
    )


@pytest.mark.parametrize("case, marker", [
    ("M11", "two-turn-delta-prefill"),
    ("M12", "three-conversation-host-tier-isolation"),
    ("M13", "conversation-negative-fallback"),
    ("M14", "concurrent-parent-cancel-prefetch"),
])
def test_conversation_cases_are_explicitly_named_and_not_fault_aliases(
        case: str, marker: str):
    gate = _module()
    assert gate.CASES[case] == marker
    args = gate.build_parser().parse_args([
        "--case", case, "--seed", "1750001", "--output-dir", "/tmp/spec175-test",
        "--tiny-fixture-root", str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"),
        "--dry-run",
    ])
    command = gate.qualification_command(args)
    assert command[command.index("--spec175-case") + 1] == case
    assert "--spec175-fault-case" not in command


def test_g3_manifest_requires_conversation_evidence_for_m11_to_m14(
        tmp_path: Path):
    g3 = _g3_module()
    run = tmp_path / "m11"
    run.mkdir()
    (run.with_name("m11.runner.log")).write_text("runner\n", encoding="utf-8")
    (run / "llm-pipeline-user.log").write_text(
        "LLM_PIPELINE_SPEC175_CONVERSATION_PASS case=M11\n", encoding="utf-8")
    result = {
        "schema": "ndnsf-di-spec175-minindn-case-result-v1",
        "status": "PASS", "case": "M11", "seed": 1750001,
        "providerCount": 4,
        "admissionControl": False, "runtime": "tiny-onnx",
        "userReturnCode": 0, "campaignId": "spec175-M11-1750001",
        "expectedTerminal": False, "providerRoleIndices": [0, 1, 2, 3],
        "requestId": "spec175-M11-1750001", "artifacts": [],
        "svsGroupFanout": {
            "status": "VERIFIED", "groupPrefix": "/example/llm-pipeline/group",
            "routerNode": "a", "memberCount": 6,
        },
        "nfdRouteSnapshot": {
            "status": "PASS", "missing": {}, "missingNextHops": {},
            "expectedNextHops": {
                "a": {"/example/llm-pipeline/group": [
                    "281", "283", "285", "287", "289", "291"]},
                **{member: {"/example/llm-pipeline/group": ["260"]}
                   for member in ("u", "p0", "p1", "p2", "p3", "repo")},
            },
        },
        "terminalEvidence": {
            "schema": "ndnsf-di-spec175-terminal-evidence-v1",
            "status": "PASS", "resultWrittenAfterProcessExit": True,
            "abortObserved": False, "childExitCodes": {"user.log": 0},
            "intentionalShutdownSignals": {},
            "unexpectedSignalExits": {},
            "survivingOwnedProcesses": [],
        },
        "conversationEvidence": {
            "schema": "ndnsf-di-spec175-conversation-evidence-v1",
            "case": "M11", "status": "PASS", "networkRequests": 2,
            "freshRequestIds": 2, "freshGenerationIds": 2,
            "requestLocalEntriesAfterCleanup": 0,
            "conversationEntries": 8, "stateTensorBytesOnNdn": 0,
            "runnerCallsAfterRejectedValidation": 0,
        },
    }
    (run / "spec175-case-result.json").write_text(
        __import__("json").dumps(result), encoding="utf-8")
    validated = g3.validate_run(run, "M11")
    assert validated["caseResult"]["conversationEvidence"]["networkRequests"] == 2
    del result["conversationEvidence"]
    (run / "spec175-case-result.json").write_text(
        __import__("json").dumps(result), encoding="utf-8")
    with pytest.raises(ValueError, match="missing conversation evidence"):
        g3.validate_run(run, "M11")
    result["conversationEvidence"] = {
        "schema": "ndnsf-di-spec175-conversation-evidence-v1",
        "case": "M11", "status": "PASS", "networkRequests": 2,
        "freshRequestIds": 2, "freshGenerationIds": 2,
        "requestLocalEntriesAfterCleanup": 0, "conversationEntries": 8,
        "stateTensorBytesOnNdn": 0, "runnerCallsAfterRejectedValidation": 0,
    }
    del result["svsGroupFanout"]
    (run / "spec175-case-result.json").write_text(
        __import__("json").dumps(result), encoding="utf-8")
    with pytest.raises(ValueError, match="SVS group fanout is not verified"):
        g3.validate_run(run, "M11")


def test_g3_manifest_rejects_unrecorded_or_wrong_case_seed(
        tmp_path: Path):
    g3 = _g3_module()
    run = tmp_path / "m01"
    run.mkdir()
    (run.with_name("m01.runner.log")).write_text("runner\n", encoding="utf-8")
    (run / "llm-pipeline-user.log").write_text(
        "LLM_PIPELINE_GENERATION_CAMPAIGN_PASS\n", encoding="utf-8")
    result = {
        "schema": "ndnsf-di-spec175-minindn-case-result-v1",
        "status": "PASS", "case": "M01", "providerCount": 4,
        "admissionControl": False, "runtime": "tiny-onnx",
        "userReturnCode": 0, "campaignId": "spec175-M01-1750001",
        "expectedTerminal": False, "providerRoleIndices": [0, 1, 2, 3],
        "requestId": "spec175-M01-1750001", "artifacts": [],
        "terminalEvidence": {
            "schema": "ndnsf-di-spec175-terminal-evidence-v1",
            "status": "PASS", "resultWrittenAfterProcessExit": True,
            "abortObserved": False, "childExitCodes": {"user.log": 0},
            "intentionalShutdownSignals": {},
            "unexpectedSignalExits": {},
            "survivingOwnedProcesses": [],
        },
    }
    path = run / "spec175-case-result.json"
    path.write_text(__import__("json").dumps(result), encoding="utf-8")
    with pytest.raises(ValueError, match="case-seed field mismatch"):
        g3.validate_run(run, "M01")
    result["seed"] = 1750002
    path.write_text(__import__("json").dumps(result), encoding="utf-8")
    with pytest.raises(ValueError, match="case-seed field mismatch"):
        g3.validate_run(run, "M01")


@pytest.mark.parametrize("case", ["M11", "M12", "M13", "M14"])
def test_conversation_case_probe_closes_without_request_local_leak(
        case: str, monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    user = _user_module()
    from ndnsf_distributed_inference.conversation import ConversationCoordinator

    class FakeClient:
        def __init__(self):
            self.conversation_coordinator = ConversationCoordinator(
                signer_key=hashlib.sha256(
                    b"spec175-minindn-test-only-checkpoint-key").digest())

        def begin_conversation_turn(self, *args, **kwargs):
            return self.conversation_coordinator.begin_turn(*args, **kwargs)

        def commit_conversation_turn(self, *args, **kwargs):
            return self.conversation_coordinator.commit_turn(*args, **kwargs)

    class FakeStream:
        _next = 0

        def __init__(self):
            type(self)._next += 1
            self.request_id = f"request-{self._next}"
            self.generation_id = f"{self._next:032x}"
            self.payload = b"{}"
            self.expected_terminal = False

    fake_client = FakeClient()

    def fake_stream(_client, _args, _payload, _expected_tokens, **kwargs):
        continuation = kwargs["conversation"]
        request_id = kwargs["wire_request_id"]
        generation_id = f"{FakeStream._next + 1:032x}"
        turn = fake_client.conversation_coordinator.begin_turn(
            continuation,
            input_payload=b"payload",
            canonical_token_ids=kwargs.get("canonical_token_ids"),
            request_id=request_id,
            generation_id=generation_id,
        )
        value = FakeStream()
        value.conversation_turn = turn
        value.generation_id = generation_id
        return value

    monkeypatch.setattr(user, "_run_tiny_onnx_stream", fake_stream)
    state_root = tmp_path / "app-state"
    state_root.mkdir()
    result = user._run_spec175_conversation_case(
        fake_client, SimpleNamespace(
            spec175_fault_case=case, app_state_root=str(state_root),
            request_id="spec175-test", max_new_tokens=2,
            test_only_conversation_contract_emulation=True),
        b"payload", [1, 2])
    assert result["status"] == "PASS"
    assert result["networkRequests"] >= 1
    assert result["freshRequestIds"] == result["networkRequests"]
    assert result["freshGenerationIds"] == result["networkRequests"]
    assert result["requestLocalEntriesAfterCleanup"] == 0
    assert result["stateTensorBytesOnNdn"] == 0
