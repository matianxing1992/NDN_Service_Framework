from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import signal
import sys
import time
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"
PLAN_DIGEST = "sha256:" + "a" * 64
OTHER_PLAN_DIGEST = "sha256:" + "b" * 64


def load_module():
    spec = importlib.util.spec_from_file_location(
        "spec189_qwen06b_native_minindn_failure_cleanup", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class LiveProcess:
    returncode = None

    def poll(self):
        return None


def test_sigterm_routes_through_cleanup_exception():
    module = load_module()
    with pytest.raises(KeyboardInterrupt, match="received signal"):
        module._raise_keyboard_interrupt(signal.SIGTERM, None)


def test_native_round_aborts_on_provider_terminal_failure(tmp_path: Path):
    module = load_module()
    requester = tmp_path / "requester.log"
    provider = tmp_path / "provider-0.log"
    provider.write_text(
        "NDNSF_DI_PROVIDER_STAGE stage=TERMINAL status=failed "
        "reason=DI_NATIVE_ONNX_GRAPH\n",
        encoding="utf-8",
    )

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="Provider terminal failure"):
        module.wait_for_native_round(LiveProcess(), requester, [provider], 30)
    assert time.monotonic() - started < 1


def test_native_round_returns_after_request_success(tmp_path: Path):
    module = load_module()
    requester = tmp_path / "requester.log"
    requester.write_text("NATIVE_REQUEST_SUCCEEDED\n", encoding="utf-8")

    process = SimpleNamespace(returncode=0, poll=lambda: 0)
    text = module.wait_for_native_round(
        process, requester, [tmp_path / "provider-0.log"], 1)
    assert "NATIVE_REQUEST_SUCCEEDED" in text


def test_native_provider_log_diagnostics_use_bounded_tail(tmp_path: Path):
    module = load_module()
    log = tmp_path / "provider.log"
    log.write_bytes(b"a" * (300 * 1024) + b"b" * (256 * 1024) + b"RUNNER_READY\n")
    tail = module._read_log_tail(log)
    assert len(tail.encode()) <= 256 * 1024
    assert tail.endswith("RUNNER_READY\n")
    assert "a" not in tail
    assert tail.startswith("b")


def test_native_turn_split_preserves_round_zero_preamble():
    module = load_module()
    text = (
        "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER enabled=true protectedPublication=skipped\n"
        "NATIVE_CONVERSATION_TURN_START index=0 request=/r-0\n"
        "NATIVE_CONVERSATION_TURN_COMPLETE index=0\n"
        "NATIVE_REQUEST_SUCCEEDED request=/r-0\n"
        "NATIVE_CONVERSATION_TURN_START index=1 request=/r-1\n"
        "NATIVE_CONVERSATION_TURN_COMPLETE index=1\n"
        "NATIVE_REQUEST_SUCCEEDED request=/r-1\n"
    )
    turns = module.split_native_turn_logs(text, 2)
    assert "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER" in turns[0]
    assert "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER" in turns[1]


def test_native_large_data_cache_purge_records_each_node_and_prefix(tmp_path: Path):
    module = load_module()
    calls = []

    class Node:
        def __init__(self, name):
            self.name = name

        def cmd(self, command):
            calls.append((self.name, command))
            return "NDNSF_CS_ERASE_RC=0\n"

    run_root = tmp_path / "run"
    run_root.mkdir()
    module.purge_minindn_large_data_cache([Node("ucla"), Node("arizona")], run_root)
    record = json.loads((run_root / "ndn-cache-purge.json").read_text())
    assert record["trigger"] == "all-conversation-turns-complete"
    assert len(record["records"]) == 4
    assert len(calls) == 4
    assert all("nfdc cs erase" in command for _, command in calls)


def test_native_round_success_marker_does_not_skip_exit_timeout(monkeypatch, tmp_path: Path):
    module = load_module()
    requester = tmp_path / "requester.log"
    requester.write_text("NATIVE_REQUEST_SUCCEEDED\n", encoding="utf-8")
    ticks = iter([0.0, 0.0, 2.0])
    monkeypatch.setattr(module, "time", SimpleNamespace(
        monotonic=lambda: next(ticks), sleep=lambda _: None))
    with pytest.raises(RuntimeError, match="timeout waiting for native request"):
        module.wait_for_native_round(LiveProcess(), requester, [], 1)


def barrier_fixture(monkeypatch, tmp_path):
    module = load_module()
    paths = [tmp_path / f"provider-{index}.log" for index in range(2)]
    for path in paths:
        path.write_text("old round ignored\n")
    offsets = module.snapshot_provider_logs(paths)
    placement = {path: (f"/provider/{index}", f"/stage/{index}")
                 for index, path in enumerate(paths)}
    clock = [0.0]
    monkeypatch.setattr(module, "time", SimpleNamespace(
        monotonic=lambda: clock[0], sleep=lambda seconds: clock.__setitem__(0, clock[0] + seconds)))
    request = (f"NATIVE_REQUEST_SUCCEEDED request=/r plan={PLAN_DIGEST}\n"
               "1790044168.187586 WARN: [ndnsf.di.RuntimeEvidence] "
               "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId=/r attemptEpoch=7 "
               f"epochMs=1790044168187 planDigest={PLAN_DIGEST}\n")
    def line(index, stage=None, status="observed"):
        marker = ("NDNSF_DI_NATIVE_SELECTION_ACCEPTED" if stage is None else
                  f"NDNSF_DI_PROVIDER_STAGE stage={stage} status={status}")
        prefix = "1790044168.187586 WARN: [ndnsf.di.RuntimeEvidence] "
        if stage is None:
            return (f"{prefix}{marker} requestId=/r attemptEpoch=7 epochMs=1790044168187 "
                    f"provider=/provider/{index} role=/stage/{index} planDigest={PLAN_DIGEST} "
                    f"manifestDigest=sha256:{'c' * 64} graphDigest=sha256:{'d' * 64} "
                    f"initializerDigest= artifactDigest=sha256:{'e' * 64} "
                    f"layerBegin={index * 14} layerEnd={(index + 1) * 14}\n")
        return (f"{prefix}{marker} requestId=/r provider=/provider/{index} "
                f"role=/stage/{index} planDigest={PLAN_DIGEST} attemptEpoch=7\n")
    records = [line(0) + line(0, "EXECUTION_COMPLETED", "stopped"),
               line(1) + line(1, "EXECUTION_COMPLETED") + line(1, "TERMINAL")]
    def write():
        for path, value in zip(paths, records):
            path.write_text("old round ignored\n" + value)
    def wait():
        return module.wait_for_provider_round_barrier(request, offsets, placement, "/stage/1", 1.0)
    return SimpleNamespace(module=module, paths=paths, offsets=offsets, placement=placement,
                           clock=clock, request=request, line=line, records=records, write=write, wait=wait)


def test_provider_barrier_preserves_stopped_as_observation(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.write()
    result = fixture.wait()
    assert result["observation"] == "finalization-only"
    assert result["attemptEpoch"] == "7"
    assert result["roles"]["/stage/0"] == {
        "provider": "/provider/0", "completionStatus": "stopped", "terminalObserved": False}
    assert result["roles"]["/stage/1"]["terminalObserved"] is True
    assert "PASS" not in str(result)


@pytest.mark.parametrize("mutation", ["old", "half", "generic", "missing-tail"])
def test_provider_barrier_incomplete_evidence_cannot_satisfy(monkeypatch, tmp_path, mutation):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    if mutation == "half":
        fixture.records[1] = fixture.records[1].rstrip("\n")
    elif mutation == "generic":
        fixture.records[0] = fixture.line(0) + "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED\n"
    elif mutation == "missing-tail":
        fixture.records[1] = fixture.line(1) + fixture.line(1, "EXECUTION_COMPLETED")
    fixture.write()
    if mutation == "old":
        fixture.offsets.update(fixture.module.snapshot_provider_logs(fixture.paths))
    with pytest.raises(RuntimeError, match="timeout waiting for Provider"):
        fixture.wait()


@pytest.mark.parametrize("mutation", ["request", "plan", "attempt", "provider", "role",
                                     "order", "tail-stopped", "duplicate", "non-tail-terminal"])
def test_provider_barrier_conflicting_complete_line_fails_closed(monkeypatch, tmp_path, mutation):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    replacement = {"request": ("requestId=/r", "requestId=/old"),
                   "plan": (f"planDigest={PLAN_DIGEST}", f"planDigest={OTHER_PLAN_DIGEST}"),
                   "attempt": ("attemptEpoch=7", "attemptEpoch=6"),
                   "provider": ("provider=/provider/1", "provider=/wrong"),
                   "role": ("role=/stage/1", "role=/wrong")}
    if mutation in replacement:
        fixture.records[1] = fixture.records[1].replace(*replacement[mutation])
    elif mutation == "order":
        fixture.records[1] = fixture.line(1) + fixture.line(1, "TERMINAL") + fixture.line(1, "EXECUTION_COMPLETED")
    elif mutation == "tail-stopped":
        fixture.records[1] = fixture.line(1) + fixture.line(1, "EXECUTION_COMPLETED", "stopped")
    elif mutation == "duplicate":
        fixture.records[0] += fixture.line(0, "EXECUTION_COMPLETED")
    else:
        fixture.records[0] += fixture.line(0, "TERMINAL")
    fixture.write()
    with pytest.raises(RuntimeError, match="Provider barrier"):
        fixture.wait()
    assert fixture.clock[0] == 0.0


def test_provider_barrier_failure_wins_over_all_completions(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.records[1] += fixture.line(1, "TERMINAL", "failed")
    fixture.write()
    with pytest.raises(RuntimeError, match="terminal failure"):
        fixture.wait()


def test_provider_barrier_budget_is_not_reset(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.write()
    fixture.clock[0] = 1.0
    with pytest.raises(RuntimeError, match="timeout waiting for Provider"):
        fixture.wait()


@pytest.mark.parametrize("mutation", ["missing", "mismatch", "duplicate", "half"])
def test_provider_barrier_requires_request_selection_binding(monkeypatch, tmp_path, mutation):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.write()
    request = fixture.request
    if mutation == "missing":
        request = request.splitlines(keepends=True)[0]
    elif mutation == "mismatch":
        request = request.replace(f"plan={PLAN_DIGEST}", f"plan={OTHER_PLAN_DIGEST}")
    elif mutation == "duplicate":
        request += request.splitlines(keepends=True)[1]
    else:
        request = request.rstrip("\n")
    with pytest.raises(RuntimeError, match="Provider barrier"):
        fixture.module.wait_for_provider_round_barrier(
            request, fixture.offsets, fixture.placement, "/stage/1", 1.0)


def test_provider_barrier_rejects_truncated_log(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.write()
    fixture.paths[0].write_text("")
    with pytest.raises(RuntimeError, match="replaced/truncated"):
        fixture.wait()


def test_provider_barrier_waits_for_complete_tail_line(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.records[1] = fixture.records[1].rstrip("\n")
    fixture.write()
    def finish_line(seconds):
        fixture.clock[0] += seconds
        with fixture.paths[1].open("a") as stream:
            stream.write("\n")
    monkeypatch.setattr(fixture.module.time, "sleep", finish_line)
    assert fixture.wait()["roles"]["/stage/1"]["terminalObserved"]
    assert fixture.clock[0] > 0


def test_provider_barrier_discards_line_started_before_round(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.paths[0].write_text(fixture.line(0).rstrip("\n"))
    fixture.offsets.update(fixture.module.snapshot_provider_logs(fixture.paths))
    with fixture.paths[0].open("a") as stream:
        stream.write("\n" + fixture.line(0, "EXECUTION_COMPLETED", "stopped"))
    fixture.paths[1].write_text("old round ignored\n" + fixture.records[1])
    with pytest.raises(RuntimeError, match="completion before Selection"):
        fixture.wait()


def test_provider_barrier_rejects_ambiguous_tail(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.placement[fixture.paths[0]] = ("/provider/0", "/stage/1")
    with pytest.raises(RuntimeError, match="invalid placement"):
        fixture.wait()


def test_provider_barrier_failure_priority_across_provider_logs(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.records[0] = fixture.records[0].replace("attemptEpoch=7", "attemptEpoch=6")
    fixture.records[1] += "NDNSF_DI_NATIVE_FAILURE reason=finalization\n"
    fixture.write()
    with pytest.raises(RuntimeError, match="terminal failure"):
        fixture.wait()


def test_round_fields_preserve_optional_empty_and_reject_duplicate_keys():
    module = load_module()
    marker = "NDNSF_DI_NATIVE_SELECTION_ACCEPTED"
    fields = module._round_fields(
        f"WARN: {marker} initializerDigest= canonicalInitializerDigest= requestId=/r", marker)
    assert fields == {"initializerDigest": "", "canonicalInitializerDigest": "", "requestId": "/r"}
    with pytest.raises(RuntimeError, match="conflicting fields"):
        module._round_fields(f"{marker} initializerDigest= initializerDigest=", marker)


@pytest.mark.parametrize("stage", [None, "EXECUTION_COMPLETED", "TERMINAL"])
@pytest.mark.parametrize("key,value", [("requestId", "/r"), ("attemptEpoch", "7"),
                                       ("planDigest", PLAN_DIGEST),
                                       ("provider", "/provider/1"), ("role", "/stage/1")])
def test_provider_barrier_rejects_required_empty_fields(monkeypatch, tmp_path, stage, key, value):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    original = fixture.line(1, stage)
    fixture.records[1] = fixture.records[1].replace(original, original.replace(f"{key}={value}", f"{key}="))
    fixture.write()
    with pytest.raises(RuntimeError, match="conflicting Provider identity"):
        fixture.wait()


@pytest.mark.parametrize("key,value", [("request", "/r"), ("plan", PLAN_DIGEST),
                                       ("requestId", "/r"), ("planDigest", PLAN_DIGEST),
                                       ("attemptEpoch", "7")])
def test_provider_barrier_rejects_requester_empty_identity(monkeypatch, tmp_path, key, value):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.write()
    request = fixture.request.replace(f"{key}={value}", f"{key}=")
    with pytest.raises(RuntimeError, match="requester/Selection identity mismatch"):
        fixture.module.wait_for_provider_round_barrier(
            request, fixture.offsets, fixture.placement, "/stage/1", 1.0)


@pytest.mark.parametrize("failed", [False, True])
def test_provider_barrier_feedback_empty_digest_is_not_finalization(monkeypatch, tmp_path, failed):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    status = "failed" if failed else "observed"
    feedback = fixture.line(0, "DEPENDENCY_FETCH", status).replace(f"planDigest={PLAN_DIGEST}", "planDigest=")
    fixture.records[0] = fixture.line(0) + feedback + fixture.line(0, "EXECUTION_COMPLETED", "stopped")
    fixture.write()
    if failed:
        with pytest.raises(RuntimeError, match="terminal failure"):
            fixture.wait()
    else:
        assert fixture.wait()["roles"]["/stage/0"]["completionStatus"] == "stopped"


def test_provider_barrier_feedback_failure_precedes_other_identity_conflict(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.records[0] = fixture.records[0].replace("attemptEpoch=7", "attemptEpoch=6")
    fixture.records[1] += fixture.line(1, "DEPENDENCY_FETCH", "failed").replace(
        f"planDigest={PLAN_DIGEST}", "planDigest=")
    fixture.write()
    with pytest.raises(RuntimeError, match="terminal failure"):
        fixture.wait()


@pytest.mark.parametrize("initializer", ["", "sha256:" + "f" * 64])
def test_provider_barrier_selection_optional_material_digest(monkeypatch, tmp_path, initializer):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    fixture.records = [record.replace("initializerDigest= ", f"initializerDigest={initializer} ")
                       for record in fixture.records]
    fixture.write()
    assert fixture.wait()["observation"] == "finalization-only"


def test_provider_barrier_intermediate_stage_cannot_replace_completion(monkeypatch, tmp_path):
    fixture = barrier_fixture(monkeypatch, tmp_path)
    feedback = fixture.line(0, "DEPENDENCY_FETCH").replace(f"planDigest={PLAN_DIGEST}", "planDigest=")
    fixture.records[0] = fixture.line(0) + feedback
    fixture.write()
    with pytest.raises(RuntimeError, match="timeout waiting for Provider"):
        fixture.wait()
