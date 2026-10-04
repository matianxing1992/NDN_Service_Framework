"""External OS/host supervision only; these tests make no native DI PASS claim."""
import importlib.util
import json
import os
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
    return {"timeoutSeconds": 3, "stopGraceSeconds": 0.1, **updates}


def run(tmp_path, code, **overrides):
    with (tmp_path / "child.log").open("w") as log:
        return guard.run_guarded(
            [sys.executable, "-c", code], cwd=tmp_path, stdout=log,
            sample_path=tmp_path / "samples.jsonl", limits=limits(**overrides))


@pytest.mark.parametrize("value", [True, 0, -1, float("nan"), float("inf"), 2**1000])
def test_invalid_limits(value):
    with pytest.raises(ValueError):
        guard.validate_limits({"timeoutSeconds": value})


def test_process_scan_tolerates_pid_exit_without_nested_proc_glob(monkeypatch):
    stat = Path(f"/proc/{os.getpid()}/stat").read_text()
    directories = [Path("/proc/100"), Path("/proc/200"), Path("/proc/self")]
    original_iterdir = Path.iterdir
    original_read = Path.read_text

    def iterate(path):
        if path == Path("/proc"):
            return iter(directories)
        return original_iterdir(path)

    def read(path, *args, **kwargs):
        if path == Path("/proc/100/stat"):
            raise ProcessLookupError("process exited during observation")
        if path == Path("/proc/200/stat"):
            return stat
        return original_read(path, *args, **kwargs)

    def reject_nested_glob(path, pattern):
        raise ProcessLookupError("nested /proc traversal raced with exit")

    monkeypatch.setattr(Path, "iterdir", iterate)
    monkeypatch.setattr(Path, "read_text", read)
    monkeypatch.setattr(Path, "glob", reject_nested_glob)
    table = guard.process_table()
    assert list(table) == [200]
    assert table[200]["pid"] == 200
    assert table[200]["start"] > 0


def test_host_capacity_thresholds_do_not_prevent_workload_start(tmp_path):
    result = run(tmp_path, "open('started', 'w').close()",
                 minAvailableBytes=2**62, minDiskFreeBytes=2**62,
                 minSwapFreeBytes=2**62)
    assert result["boundary"] is None
    assert result["returncode"] == 0
    assert (tmp_path / "started").exists()


def test_normal_child_records_owned_process_lifecycle_only(tmp_path):
    original_handler = signal.getsignal(signal.SIGTERM)
    result = run(tmp_path, "print('ok')")
    assert result["returncode"] == 0
    assert result["boundary"] is None
    assert result["cleanup"] == "PASS"
    assert signal.getsignal(signal.SIGTERM) == original_handler
    records = [json.loads(line) for line in (tmp_path / "samples.jsonl").read_text().splitlines()]
    assert records[0]["phase"] == "admission"
    assert records[-1]["phase"] == "drained"
    assert all("availableBytes" not in row and "diskFreeBytes" not in row and
               "swapFreeBytes" not in row for row in records)
    assert all(row["nativeCounters"] is None for row in records)


def test_host_capacity_sample_does_not_read_proc_or_disk(tmp_path, monkeypatch):
    original = Path.read_text

    def deny_host_capacity(path, *args, **kwargs):
        if str(path) in {"/proc/meminfo", "/proc/pressure/memory", "/proc/vmstat"}:
            raise AssertionError("host capacity must not be inspected")
        return original(path, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", deny_host_capacity)
    sample = guard.host_sample(tmp_path)
    assert sample == {}


def test_legacy_swap_limits_are_ignored_without_reading_swap(tmp_path, monkeypatch):
    original = Path.read_text

    def reject_swap_reads(path, *args, **kwargs):
        path_text = str(path)
        if path_text == "/proc/vmstat" or (
                path_text.startswith("/proc/") and path_text.endswith("/status")):
            raise AssertionError("experiment supervisor must not read swap state")
        return original(path, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", reject_swap_reads)
    result = run(tmp_path, "open('started', 'w').close()",
                 minSwapFreeBytes=1, maxOwnedSwapBytes=1, maxSwapIoBytes=1)
    assert result["boundary"] is None
    assert result["returncode"] == 0
    assert (tmp_path / "started").exists()


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


def test_memory_and_disk_floors_are_ignored_in_legacy_profiles(tmp_path):
    result = run(tmp_path, "open('started','w').close()",
                 minAvailableBytes=2**62, minDiskFreeBytes=2**62)
    assert result["boundary"] is None
    assert result["returncode"] == 0
    assert (tmp_path / "started").exists()


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


def test_direct_cli_ignores_host_capacity_and_reports_actual_input_failure(tmp_path):
    completed = subprocess.run(
        [sys.executable, str(ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"),
         "--stage-manifest", str(tmp_path / "missing-model.json"),
         "--node-mapping", str(tmp_path / "missing-mapping.json"),
         "--run-root", str(tmp_path / "run"),
         "--resource-limits-json", json.dumps(limits(minAvailableBytes=2**62))],
        text=True, capture_output=True, timeout=10)
    assert completed.returncode == 1, completed.stderr
    receipt = json.loads((tmp_path / "run/supervisor.json").read_text())
    assert receipt["boundary"] is None
    assert receipt["returncode"] not in (None, 0)
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
