"""Installed-runner ownership checks; native C++ tests own commit-authority semantics."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import psutil

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments/UAV"))
from person_run_receipt import audit_person_processes


def test_external_wrapper_argument_is_not_owned_but_pinned_identity_is(tmp_path):
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)', str(tmp_path)])
    try:
        created = psutil.Process(child.pid).create_time()
        audit = audit_person_processes(tmp_path)
        assert child.pid not in {p['pid'] for p in audit['remaining']}
        audit = audit_person_processes(tmp_path, [
            {'pid': child.pid, 'creationTimeUnixSeconds': created}])
        found = next(p for p in audit['remaining'] if p['pid'] == child.pid)
        assert found['ownership'] == 'pinned-identity'
        # A reused PID with a different creation identity cannot inherit ownership.
        audit = audit_person_processes(tmp_path, [
            {'pid': child.pid, 'creationTimeUnixSeconds': created - 1}])
        assert child.pid not in {p['pid'] for p in audit['remaining']}
    finally:
        child.terminate()
        child.wait(timeout=3)


def test_orphan_without_root_argv_still_has_its_run_marker(tmp_path):
    code = ("import subprocess,sys,json,psutil; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'],"
            "start_new_session=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
            "print(json.dumps([p.pid,psutil.Process(p.pid).create_time()]),flush=True)")
    env = dict(os.environ, NDNSF_SPEC191_RUN_ROOT=str(tmp_path.resolve()))
    parent = subprocess.run([sys.executable, '-c', code], env=env, capture_output=True,
                            text=True, timeout=5, check=True)
    pid, created = json.loads(parent.stdout)
    try:
        audit = audit_person_processes(tmp_path)
        found = next(p for p in audit['remaining'] if p['pid'] == pid)
        assert found['ownership'] == 'run-environment-marker'
        assert str(tmp_path) not in found['command']
    finally:
        try:
            process = psutil.Process(pid)
            if process.create_time() == created:
                process.kill()
        except psutil.NoSuchProcess:
            pass


def test_unreaped_owned_zombie_is_not_reported_as_clean(tmp_path):
    child = subprocess.Popen([sys.executable, '-c', 'pass'])
    try:
        process = psutil.Process(child.pid)
        created = process.create_time()
        deadline = time.monotonic() + 3
        while process.status() != psutil.STATUS_ZOMBIE and time.monotonic() < deadline:
            time.sleep(.01)
        assert process.status() == psutil.STATUS_ZOMBIE
        audit = audit_person_processes(tmp_path, [
            {'pid': child.pid, 'creationTimeUnixSeconds': created}])
        assert audit['remainingOwnedProcesses'] >= 1
        assert audit['unreapedOwnedProcesses'] == 1
        assert audit['zombies'][0]['pid'] == child.pid
    finally:
        child.wait(timeout=3)


def test_vehicle_cli_uses_frozen_global_deadline_instead_of_media_duration(tmp_path, monkeypatch):
    import run_multicamera_tracking_demo as runner
    calls = {}
    def build(**kwargs):
        calls['plan'] = kwargs
        return {'settings': {'windowCount': kwargs['window_count']}}
    def execute(plan, output, *, headless, wall_timeout_seconds):
        calls['wait'] = wall_timeout_seconds
        return 0
    monkeypatch.setattr(runner, 'build_plan', build)
    monkeypatch.setattr(runner, 'preflight', lambda *a, **k: {'ok': True})
    monkeypatch.setattr(runner, '_run_tracking', execute)
    assert runner.main(['--run', '--headless', '--output-dir', str(tmp_path / 'run'),
                        '--run-seconds', '1', '--window-count', '2',
                        '--global-deadline-ms', '180000']) == 0
    assert calls['wait'] == 180.0
    assert calls['plan']['run_seconds'] == 1.0
    assert calls['plan']['window_count'] == 2
