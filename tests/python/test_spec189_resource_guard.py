"""External OS/host supervision only; these tests make no native DI PASS claim."""
import importlib.util
import json
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "spec189_resource_guard", ROOT / "Experiments/native_resource_guard.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def limits(**updates):
    return {"minAvailableBytes": 1, "minSwapFreeBytes": 1,
            "minDiskFreeBytes": 1, "maxOwnedSwapBytes": 2**60,
            "maxSwapIoBytes": 2**60, "timeoutSeconds": 3,
            "stopGraceSeconds": 0.1, **updates}


def run(tmp_path, code, **overrides):
    with (tmp_path / "child.log").open("w") as log:
        return guard.run_guarded(
            [sys.executable, "-c", code], cwd=tmp_path, stdout=log,
            sample_path=tmp_path / "samples.jsonl", limits=limits(**overrides))


@pytest.mark.parametrize("value", [True, 0, -1, float("nan"), float("inf"), 2**1000])
def test_invalid_limits(value):
    with pytest.raises(ValueError):
        guard.validate_limits({"minAvailableBytes": value})


def test_preflight_rejects_without_starting_child(tmp_path):
    result = run(tmp_path, "open('started', 'w').close()", minAvailableBytes=2**62)
    assert result["boundary"] == "RESOURCE_BOUNDARY:MemAvailable"
    assert result["returncode"] is None
    assert not (tmp_path / "started").exists()


def test_normal_child_and_actual_host_samples(tmp_path):
    original_handler = signal.getsignal(signal.SIGTERM)
    result = run(tmp_path, "print('ok')")
    assert result["returncode"] == 0
    assert result["boundary"] is None
    assert result["cleanup"] == "PASS"
    assert signal.getsignal(signal.SIGTERM) == original_handler
    records = [json.loads(line) for line in (tmp_path / "samples.jsonl").read_text().splitlines()]
    assert records[0]["phase"] == "admission"
    assert records[-1]["phase"] == "drained"
    assert all(row["availableBytes"] > 0 and row["diskFreeBytes"] > 0 for row in records)
    assert all("swapIoDeltaBytes" in row and "ownedSwapDeltaBytes" in row and
               "swapFreeBytes" in row for row in records)
    assert all(row["nativeCounters"] is None for row in records)


def test_unowned_global_swap_activity_does_not_stop_workload(tmp_path, monkeypatch):
    original = guard.host_sample

    def noisy_host(path):
        sample = original(path)
        sample["swapIoBytes"] += 16 * 1024 * 1024
        return sample

    monkeypatch.setattr(guard, "host_sample", noisy_host)
    monkeypatch.setattr(guard, "owned_swap_bytes", lambda rows: 0)
    result = run(tmp_path, "print('ok')", maxOwnedSwapBytes=1)
    assert result["boundary"] is None
    assert result["returncode"] == 0


def test_owned_swap_growth_is_diagnostic_only(tmp_path, monkeypatch):
    calls = 0

    def owned_swap(rows):
        nonlocal calls
        calls += 1
        return 0 if calls < 3 else 2

    monkeypatch.setattr(guard, "owned_swap_bytes", owned_swap)
    result = run(tmp_path, "import time; time.sleep(60)",
                 maxOwnedSwapBytes=1, timeoutSeconds=0.3)
    assert result["boundary"] == "DEADLINE_BOUNDARY"
    assert result["cleanup"] == "PASS"


def test_swap_free_floor_stops_workload(tmp_path, monkeypatch):
    original = guard.host_sample

    def depleted_swap(path):
        sample = original(path)
        sample["swapFreeBytes"] = 0
        return sample

    monkeypatch.setattr(guard, "host_sample", depleted_swap)
    result = run(tmp_path, "open('started', 'w').close()", minSwapFreeBytes=1)
    assert result["boundary"] == "RESOURCE_BOUNDARY:SwapFree"
    assert not (tmp_path / "started").exists()


def test_runtime_resource_stop_reaps_real_child(tmp_path, monkeypatch):
    original = guard.host_sample
    calls = 0

    def depleted(path):
        nonlocal calls
        calls += 1
        sample = original(path)
        if calls >= 3:
            sample["availableBytes"] = 0
        return sample

    monkeypatch.setattr(guard, "host_sample", depleted)
    result = run(tmp_path, "import time; time.sleep(60)")
    assert result["boundary"] == "RESOURCE_BOUNDARY:MemAvailable"
    assert result["returncode"] is not None
    assert result["cleanup"] == "PASS"
    assert result["remainingProcesses"] == []


def test_deadline_kills_signal_resistant_child(tmp_path):
    result = run(tmp_path,
                 "import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); "
                 "signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)",
                 timeoutSeconds=0.3)
    assert result["boundary"] == "DEADLINE_BOUNDARY"
    assert result["returncode"] == -signal.SIGKILL
    assert result["cleanup"] == "PASS"


def test_parent_exit_does_not_hide_live_descendant(tmp_path):
    result = run(tmp_path,
                 "import subprocess,sys,time; "
                 "subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); "
                 "time.sleep(0.1)")
    assert result["returncode"] == 0
    assert result["boundary"] == "CHILD_PROCESS_LEAK"
    assert result["cleanup"] == "PASS"


def test_sampling_error_stops_child_instead_of_allowing_run(tmp_path, monkeypatch):
    original = guard.host_sample
    calls = 0

    def broken(path):
        nonlocal calls
        calls += 1
        if calls > 1:
            raise OSError("probe failed")
        return original(path)

    monkeypatch.setattr(guard, "host_sample", broken)
    result = run(tmp_path, "import time; time.sleep(60)")
    assert result["boundary"] == "RESOURCE_MONITOR_ERROR:OSError"
    assert result["returncode"] is not None
    assert result["cleanup"] == "PASS"


def test_previous_sample_evidence_is_never_overwritten(tmp_path):
    path = tmp_path / "samples.jsonl"
    path.write_text("previous evidence\n")
    result = run(tmp_path, "open('started', 'w').close()")
    assert result["boundary"] == "EVIDENCE_CONFLICT"
    assert result["cleanup"] == "NOT_STARTED"
    assert path.read_text() == "previous evidence\n"
    assert not (tmp_path / "started").exists()


def test_double_fork_setsid_cannot_escape_or_poison_unrelated_child(tmp_path):
    unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    try:
        result = run(tmp_path,
                     "import os,time; "
                     "pid=os.fork(); "
                     "os._exit(0) if pid else None; "
                     "os.setsid(); "
                     "pid=os.fork(); "
                     "os._exit(0) if pid else None; "
                     "time.sleep(60)")
        assert result["boundary"] == "CHILD_PROCESS_LEAK"
        assert result["cleanup"] == "PASS"
        assert not result["remainingProcesses"]
        assert unrelated.poll() is None
    finally:
        unrelated.terminate()
        unrelated.wait(timeout=3)


def test_disk_floor_prevents_workload(tmp_path):
    result = run(tmp_path, "open('started','w').close()", minDiskFreeBytes=2**62)
    assert result["boundary"] == "RESOURCE_BOUNDARY:diskFree"
    assert not (tmp_path / "started").exists()


def test_broken_process_sampler_emergency_reaps_workload(tmp_path, monkeypatch):
    def broken():
        time.sleep(0.2)
        raise OSError("process sampling unavailable")

    monkeypatch.setattr(guard, "process_table", broken)
    result = run(tmp_path,
                 "import os,time; open('child.pid','w').write(str(os.getpid())); time.sleep(60)")
    assert result["boundary"] == "SUPERVISOR_ERROR:OSError"
    assert result["cleanup"] == "UNOBSERVED"
    assert result["emergencyCleanup"]["remainingPids"] == []
    pid = int((tmp_path / "child.pid").read_text())
    assert not Path(f"/proc/{pid}").exists()


def test_supervisor_sigterm_cancels_and_drains(tmp_path):
    code = (
        "import sys,json,pathlib; sys.path.insert(0," + repr(str(ROOT / "Experiments")) + "); "
        "from native_resource_guard import run_guarded; "
        "result=run_guarded([sys.executable,'-c',"
        + repr("import time; open('ready','w').close(); time.sleep(60)") + "],"
        "cwd='.',stdout=open('child.log','w'),sample_path='samples.jsonl',limits="
        + repr(limits()) + "); pathlib.Path('result.json').write_text(json.dumps(result))")
    proc = subprocess.Popen([sys.executable, "-c", code], cwd=tmp_path)
    try:
        deadline = time.monotonic() + 5
        while not (tmp_path / "ready").exists() and time.monotonic() < deadline:
            assert proc.poll() is None
            time.sleep(0.02)
        assert (tmp_path / "ready").exists()
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=5) == 0
        result = json.loads((tmp_path / "result.json").read_text())
        assert result["boundary"] == "CANCELLED"
        assert result["cleanup"] == "PASS"
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)


def test_direct_cli_admission_precedes_model_or_root_checks(tmp_path):
    completed = subprocess.run(
        [sys.executable, str(ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"),
         "--stage-manifest", str(tmp_path / "missing-model.json"),
         "--node-mapping", str(tmp_path / "missing-mapping.json"),
         "--run-root", str(tmp_path / "run"),
         "--resource-limits-json", json.dumps(limits(minAvailableBytes=2**62))],
        text=True, capture_output=True, timeout=10)
    assert completed.returncode == 1, completed.stderr
    receipt = json.loads((tmp_path / "run/supervisor.json").read_text())
    assert receipt["boundary"] == "RESOURCE_BOUNDARY:MemAvailable"
    assert receipt["returncode"] is None
    assert not (tmp_path / "run/requester").exists()
    assert not (tmp_path / "run/run-record.json").exists()
    assert (tmp_path / "run").stat().st_mode & 0o777 == 0o700
    assert (tmp_path / "run/supervisor.json").stat().st_mode & 0o777 == 0o600


def test_direct_cli_preserves_previous_receipt(tmp_path):
    run_root = tmp_path / "run"
    run_root.mkdir()
    receipt = run_root / "supervisor.json"
    receipt.write_text("prior evidence\n")
    completed = subprocess.run(
        [sys.executable, str(ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"),
         "--stage-manifest", "missing", "--node-mapping", "missing",
         "--run-root", str(run_root)], text=True, capture_output=True, timeout=10)
    assert completed.returncode == 1
    assert '"boundary": "EVIDENCE_CONFLICT"' in completed.stdout
    assert receipt.read_text() == "prior evidence\n"
    assert not (run_root / "resource-samples.jsonl").exists()
