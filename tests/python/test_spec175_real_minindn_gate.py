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
REPO_BOOTSTRAP_SCRIPT = ROOT / "Experiments/spec175_repo_bootstrap.py"
G3_SCRIPT = ROOT / "scripts/spec175_g3_manifest.py"


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


def test_spec175_native_timing_records_use_one_ndn_log_sink():
    root = ROOT
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
    assert "writeRuntimeEvidence(record, false);" in sink
    assert "NDN_LOG_WARN(singleLine(record));" in sink
    assert "NDN_LOG_WARN(line);" in sink
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


@pytest.mark.parametrize(("accepted_attempt", "expected_attempts"), [(1, 1), (2, 2), (0, 3)])
def test_spec175_repo_route_probe_uses_native_discovery_with_bounded_attempts(
        tmp_path, accepted_attempt, expected_attempts):
    repo = _repo_bootstrap_module()
    repo_node = "/NDNSF/DistributedRepo/Node/0"
    repo_provider = "/example/repo"
    service_prefix = "/NDNSF/DistributedRepo/Object/v1"
    expected = SimpleNamespace(
        repo_node=repo_node, provider_identity=repo_provider, service_prefix=service_prefix,
        ack_wire_digest="sha256:" + "a" * 64,
        signer_key_locator=repo_provider + "/KEY/key",
        observed_at_ms=123,
    )
    wrong_provider = SimpleNamespace(
        repo_node=repo_node, provider_identity="/other/repo", service_prefix=service_prefix)
    requirements = object()
    calls = []

    def discover(requirements_arg, timeout_ms):
        assert requirements_arg is requirements
        assert 0 < timeout_ms <= 20
        calls.append(timeout_ms)
        candidates = [expected] if len(calls) == accepted_attempt else [wrong_provider]
        return SimpleNamespace(request_id=f"/probe/{len(calls)}", candidates=candidates)

    output = tmp_path / "probe.json"
    args = SimpleNamespace(
        user="/publisher", repo_provider=repo_provider, repo_node=repo_node,
        repo_service_prefix=service_prefix, probe_timeout_ms=100, probe_attempt_timeout_ms=20,
        probe_retries=2, probe_retry_backoff_ms=0, probe_output=output,
    )
    report = repo.run_live_service_probe(
        SimpleNamespace(discover_repo_candidates=discover), requirements, args)
    assert len(calls) == expected_attempts
    assert report["attemptCount"] == expected_attempts
    assert [item["probeId"] for item in report["attempts"]] == [
        f"/probe/{index}" for index in range(1, expected_attempts + 1)]
    assert report["route"]["service"] == service_prefix + "/CAPABILITY"
    if accepted_attempt:
        assert report["status"] == "PASS"
        assert report["route"]["provider"] == repo_provider
        assert report["ack"]["wireDigest"] == expected.ack_wire_digest
    else:
        assert report["status"] == "FAIL"
        assert report["failureReason"] == "REPO_SERVICE_ROUTE_NOT_READY"
    assert json.loads(output.read_text()) == report


def test_native_user_stop_releases_gil_while_joining_face_thread():
    """Timeout cleanup must let an in-flight Python callback finish."""
    source = (ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text(
        encoding="utf-8")
    stop = source.index('.def("stop", &NativeServiceUser::stop,')
    binding = source[stop:stop + 180]
    assert "py::call_guard<py::gil_scoped_release>()" in binding


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
