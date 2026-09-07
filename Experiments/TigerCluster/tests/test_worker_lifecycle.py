"""Real finite-process lifecycle tests, without containers or Slurm."""
import json
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
SUPERVISOR = r'''
import ctypes
import json
import os
from pathlib import Path
import signal
import sys
import time

sys.path.insert(0, sys.argv[1])
from runtime.worker import run_finite_application

root, mode, exit_code = Path(sys.argv[2]), sys.argv[3], int(sys.argv[4])
cleanup = []
failure = None
child_status = None
if mode == "orphan":
    # Reap orphaned descendants inside this isolated supervisor, including on
    # hosts whose PID 1 does not promptly reap adopted zombies.
    assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0
    code = ("import subprocess,sys;from pathlib import Path;"
            "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']);"
            "Path(sys.argv[1]).write_text(str(p.pid));sys.exit(int(sys.argv[2]))")
    argv = [sys.executable, "-c", code, str(root / "child.pid"), str(exit_code)]
else:
    code = "import time;time.sleep(60)" if mode == "timeout" else "import sys;sys.exit(" + str(exit_code) + ")"
    argv = [sys.executable, "-c", code]
try:
    run_finite_application("probe", argv, root / "probe.log", cleanup,
                           seconds=0.05 if mode == "timeout" else 3,
                           allowed_exits=(0, exit_code) if mode == "allowed" else (0,))
except Exception as exc:
    failure = type(exc).__name__ + ":" + str(exc)
finally:
    if mode == "orphan" and (root / "child.pid").exists():
        pid = int((root / "child.pid").read_text())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            reaped, status = os.waitpid(pid, os.WNOHANG)
            if reaped:
                child_status = status
                break
            time.sleep(0.01)
        if child_status is None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
print(json.dumps({"cleanup": cleanup, "failure": failure,
                  "childStatus": child_status}))
'''


def observe(tmp_path, mode, exit_code):
    completed = subprocess.run([sys.executable, "-c", SUPERVISOR, str(ROOT),
                                str(tmp_path), mode, str(exit_code)],
                               capture_output=True, text=True, check=True, timeout=12)
    return json.loads(completed.stdout)


@pytest.mark.parametrize("mode,exit_code", [("normal", 0), ("normal", 7), ("allowed", 134)])
def test_finite_exit_is_recorded_without_false_premature_death(tmp_path, mode, exit_code):
    result = observe(tmp_path, mode, exit_code)
    row, = result["cleanup"]
    assert row["exitCode"] == exit_code and row["reaped"]
    assert row["kind"] == "finite" and not row["forced"] and not row["exitedBeforeCleanup"]
    if mode == "normal" and exit_code:
        assert result["failure"] == "RuntimeError:APP_EXIT:probe:7"
    else:
        assert result["failure"] is None


@pytest.mark.parametrize("exit_code", [0, 7])
def test_exited_leader_with_live_descendant_is_killed_and_cannot_qualify(tmp_path, exit_code):
    result = observe(tmp_path, "orphan", exit_code)
    row, = result["cleanup"]
    assert row["reaped"] and row["forced"]
    assert row["exitCode"] == exit_code
    assert result["childStatus"] == 9  # Actual SIGKILL, reaped by the supervisor.
    assert (result["failure"] is not None) == bool(exit_code)


def test_timeout_remains_failure_after_application_is_reaped(tmp_path):
    result = observe(tmp_path, "timeout", 0)
    row, = result["cleanup"]
    assert result["failure"].startswith("TimeoutExpired:")
    assert row["reaped"] and not row["forced"]


def test_finite_application_uses_cwd_and_custom_log_with_shared_cleanup(tmp_path):
    sys.path.insert(0, str(ROOT))
    from runtime.worker import run_finite_application
    work = tmp_path / "bundle"
    work.mkdir()
    (work / "input.txt").write_text("bounded-finite-input")
    cleanup = []
    logfile = tmp_path / "request-0001.stdout"
    assert run_finite_application("user", [sys.executable, "-c",
        "from pathlib import Path; print(Path('input.txt').read_text())"], logfile,
        cleanup, seconds=3, cwd=work, cleanup_seconds=0.5) == 0
    assert logfile.read_text().strip() == "bounded-finite-input"
    assert cleanup[0]["name"] == "user" and cleanup[0]["kind"] == "finite"
    assert cleanup[0]["reaped"] and not cleanup[0]["forced"]
    assert not cleanup[0]["exitedBeforeCleanup"]


@pytest.mark.parametrize("seconds", [None, True, 0, -1, float("nan"), float("inf")])
def test_finite_application_requires_bounded_deadline_before_launch(tmp_path, seconds):
    sys.path.insert(0, str(ROOT))
    from runtime.worker import run_finite_application
    cleanup = []
    logfile = tmp_path / "should-not-exist.log"
    with pytest.raises(ValueError, match="APP_DEADLINE"):
        run_finite_application("user", [sys.executable, "-c", "pass"],
                               logfile, cleanup, seconds=seconds)
    assert not logfile.exists() and cleanup == []


def test_finite_user_aborts_on_service_failure_and_retains_cleanup(tmp_path):
    sys.path.insert(0, str(ROOT))
    from runtime.worker import run_finite_application
    cleanup, calls = [], []

    def check():
        calls.append(1)
        if len(calls) == 3:
            raise RuntimeError('PROVIDER_DIED')

    with pytest.raises(RuntimeError, match='PROVIDER_DIED'):
        run_finite_application('user', [sys.executable, '-c', 'import time; time.sleep(60)'],
                               tmp_path / 'user.log', cleanup, seconds=5, check=check,
                               cleanup_seconds=0.5)
    assert cleanup[0]['reaped'] and cleanup[0]['kind'] == 'finite'
