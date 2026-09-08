"""Real process exits at the shared MiniNDN cleanup helper; no network run."""
import importlib
import io
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Experiments'))
owner = importlib.import_module('NDNSF_DI_Yolo2x2_Minindn')


def child(tmp_path, name, ignore=False):
    path = tmp_path / (name + '.log')
    log = path.open('wb')
    script = ('import signal,time; signal.signal(signal.SIGINT, signal.SIG_IGN); '
              if ignore else 'import time; ')
    script += 'print("ready",flush=True); time.sleep(60)'
    proc = subprocess.Popen([sys.executable, '-c', script], stdout=log, stderr=log)
    deadline = time.monotonic() + 5
    while 'ready' not in path.read_text():
        if proc.poll() is not None or time.monotonic() > deadline:
            proc.kill(); proc.wait(); log.close()
            pytest.fail('child did not start')
        time.sleep(.01)
    return proc, log, path


@pytest.mark.parametrize('ignore', [False, True])
def test_cleanup_reaps_real_child_without_touching_unowned_child(tmp_path, ignore):
    owned = child(tmp_path, 'owned', ignore)
    other = child(tmp_path, 'other', True)
    try:
        started = time.monotonic()
        rows = owner.stop_process_group([owned], seconds=1)
        assert time.monotonic() - started < 2
        assert rows[0]['reaped'] and rows[0]['pid'] == owned[0].pid
        assert rows[0]['forced'] is ignore
        assert rows[0]['exitStatus'] == (-signal.SIGKILL if ignore else -signal.SIGINT)
        assert owned[0].wait(timeout=0) == rows[0]['exitStatus']
        assert owned[1].closed and other[0].poll() is None
    finally:
        for proc, log, _path in (owned, other):
            if proc.poll() is None: proc.kill()
            proc.wait(timeout=3); log.close()


def test_unreaped_child_retains_handle_and_reports_failure(tmp_path):
    class Stuck:
        pid = 987654
        def poll(self): return None
        def send_signal(self, value): assert value == signal.SIGINT
        def kill(self): pass
        def wait(self, timeout): raise subprocess.TimeoutExpired('fixture', timeout)
    stream = io.BytesIO()
    entries = [(Stuck(), stream, tmp_path/'stuck.log')]
    with pytest.raises(RuntimeError, match='CHILD_CLEANUP_INCOMPLETE') as observed:
        owner.stop_process_group(entries, seconds=.01)
    assert observed.value.records[0]['reaped'] is False
    assert observed.value.records[0]['forced'] is True
    assert not stream.closed and len(entries) == 1


def test_invalid_budget_signals_no_child():
    for budget in (0, -1, True, float('inf'), float('nan')):
        with pytest.raises(ValueError, match='CHILD_CLEANUP_BUDGET'):
            owner.stop_process_group([], seconds=budget)
