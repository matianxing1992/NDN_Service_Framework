"""Actual process-group shutdown, isolated by an outer test timeout."""
import json
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("seconds", [True, 0, -1, float("nan"), float("inf"), "30"])
def test_invalid_cleanup_budget_does_not_drop_ownership(tmp_path, seconds):
    sys.path.insert(0, str(ROOT))
    from runtime.baseline import Processes
    children = Processes(tmp_path / "logs")
    with pytest.raises(ValueError, match="CLEANUP_BUDGET"):
        children.close(seconds=seconds)


def test_failed_kill_retains_owner_and_other_groups_are_still_reaped(tmp_path):
    script = r'''
import json, os, signal, sys, time
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
from runtime.baseline import Processes
root = Path(sys.argv[2])
children = Processes(root / "logs")
killpg = os.killpg
try:
    healthy = children.start("healthy", [sys.executable, "-c", "import time;time.sleep(60)"])
    stuck = children.start("stuck", [sys.executable, "-c",
        "import signal,time;from pathlib import Path;"
        "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
        "Path(" + repr(str(root / 'ready')) + ").touch();time.sleep(60)"])
    deadline = time.monotonic() + 4
    while not (root / 'ready').exists():
        children.check()
        if time.monotonic() > deadline:
            raise RuntimeError("TEST_CHILD_NOT_READY")
        time.sleep(0.01)
    def denied(pid, sig):
        if pid == stuck.pid:
            raise PermissionError("simulated signal denial")
        return killpg(pid, sig)
    with patch("os.killpg", denied):
        rows = children.close(seconds=0.3)
    retained = [name for name, _, _ in children.children]
    retry = children.close(seconds=0.3)
    print(json.dumps({"rows": rows, "retained": retained, "retry": retry}))
finally:
    for _, child, log in children.children:
        try:
            killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=1)
        log.close()
'''
    done = subprocess.run([sys.executable, "-c", script, str(ROOT), str(tmp_path)],
                          capture_output=True, text=True, timeout=8)
    assert done.returncode == 0, done.stderr
    result = json.loads(done.stdout)
    by_name = {row["name"]: row for row in result["rows"]}
    assert by_name["healthy"]["reaped"]
    assert by_name["stuck"]["cleanupError"] == "PermissionError"
    assert by_name["stuck"]["cleanupTimedOut"] and not by_name["stuck"]["reaped"]
    assert result["retained"] == ["stuck"]
    assert result["retry"][0]["forced"] and result["retry"][0]["reaped"]


def test_stubborn_services_share_one_cleanup_deadline(tmp_path):
    script = r'''
import json, os, signal, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from runtime.baseline import Processes
root = Path(sys.argv[2])
children = Processes(root / "logs")
try:
    for index in range(4):
        ready = root / str(index)
        children.start(str(index), [sys.executable, "-c",
            "import signal,time;from pathlib import Path;"
            "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
            "Path(" + repr(str(ready)) + ").touch();time.sleep(60)"])
    deadline = time.monotonic() + 4
    while len(list(root.glob('[0-9]'))) < 4:
        children.check()
        if time.monotonic() > deadline:
            raise RuntimeError("TEST_CHILD_NOT_READY")
        time.sleep(0.01)
    start = time.monotonic()
    rows = children.close(seconds=0.4)
    print(json.dumps({"elapsed": time.monotonic() - start, "rows": rows,
                      "secondClose": children.close(seconds=0.4)}))
finally:
    # Red-test cleanup must also be bounded, before the new API exists.
    for _, child, log in children.children:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=1)
        log.close()
'''
    done = subprocess.run([sys.executable, "-c", script, str(ROOT), str(tmp_path)],
                          capture_output=True, text=True, timeout=8)
    assert done.returncode == 0, done.stderr
    result = json.loads(done.stdout)
    assert result["elapsed"] < 1.2  # Scheduler slack, not four per-child budgets.
    assert len(result["rows"]) == 4
    assert all(row["forced"] and row["reaped"] and row["exitCode"] == -9
               and not row["exitedBeforeCleanup"] for row in result["rows"])
    assert result["secondClose"] == []
