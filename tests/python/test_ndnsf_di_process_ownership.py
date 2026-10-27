"""Real local child/group checks for thin deployment management, not DI qualification."""
import contextlib
import copy
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import uuid

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'ndnsf_processes_test', ROOT / 'Experiments/TigerCluster/runtime/processes.py')
processes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(processes)


@contextlib.contextmanager
def actor(script):
    env = dict(os.environ, NDNSF_DEPLOYMENT_ACTOR_ID='actor',
               NDNSF_DEPLOYMENT_BOOT_ID=uuid.uuid4().hex,
               NDNSF_DEPLOYMENT_CONFIG_SHA256='f' * 64)
    child = subprocess.Popen([sys.executable, '-c', script], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             start_new_session=True, env=env)
    try:
        assert select.select([child.stdout], [], [], 5)[0], 'child did not reach fixture barrier'
        assert child.stdout.read(1) == b'R', 'child failed before fixture barrier'
        record = processes.process_identity(child.pid)
        assert record is not None
        yield child, record
    finally:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=5)
        for stream in (child.stdin, child.stdout, child.stderr):
            stream.close()


SLEEP = "import os,time; os.write(1,b'R'); time.sleep(60)"
REFUSE_TERM = ("import os,time,signal; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
               "os.write(1,b'R'); time.sleep(60)")
GRANDCHILD = """import os,signal,time
ready_read, ready_write = os.pipe()
if os.fork() == 0:
    os.close(ready_read)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    os.write(ready_write,b'Y')
    os.close(ready_write)
    while True:
        time.sleep(1)
os.close(ready_write)
assert os.read(ready_read,1) == b'Y'
os.close(ready_read)
os.write(1,b'R')
os.read(0,1)
"""


def test_normal_owned_termination_and_reap():
    with actor(SLEEP) as (_, record):
        assert processes.owned_state(record) == 'RUNNING'
        result = processes.stop_owned(record, 0.5, 0.5)
        assert result['status'] == 'ZERO', result
        assert result['forced'] is False
        assert result['reaped'] is True
        assert result['exitCode'] == -signal.SIGTERM


def test_wrong_and_truncated_proofs_never_signal_live_process():
    with actor(SLEEP) as (child, record):
        for key, value in [('start_ticks', record['start_ticks'] + 1), ('exe', '/wrong'),
                           ('uid', record['uid'] + 1), ('pgid', record['pid'] + 1)]:
            wrong = dict(record, **{key: value})
            assert processes.stop_owned(wrong, 0.01, 0.01)['status'] == 'UNKNOWN'
            assert child.poll() is None
        wrong = copy.deepcopy(record)
        wrong['bindings']['NDNSF_DEPLOYMENT_BOOT_ID'] = 'wrong'
        assert processes.stop_owned(wrong, 0.01, 0.01)['status'] == 'UNKNOWN'
        assert child.poll() is None
        for key in ('start_ticks', 'exe', 'bindings'):
            incomplete = {k: v for k, v in record.items() if k != key}
            assert processes.stop_owned(incomplete, 0.01, 0.01)['status'] == 'UNKNOWN'
            assert child.poll() is None


def test_term_refusal_escalates_and_reaps():
    with actor(REFUSE_TERM) as (_, record):
        result = processes.stop_owned(record, 0.1, 0.5)
        assert result['status'] == 'ZERO'
        assert result['forced'] is True
        assert result['exitCode'] == -signal.SIGKILL


def test_leader_exit_does_not_hide_live_grandchild():
    with actor(GRANDCHILD) as (child, record):
        child.stdin.write(b'X')
        child.stdin.flush()
        child.wait(timeout=5)
        assert processes.owned_state(record) == 'EXITED'
        incomplete = {key: value for key, value in record.items() if key != 'start_ticks'}
        assert processes.stop_owned(incomplete, 0.1, 0.1)['status'] == 'UNKNOWN'
        assert processes._group_members(record)
        result = processes.stop_owned(record, 0.1, 1)
        assert result['status'] == 'ZERO'
        assert result['forced'] is True


def test_leader_esrch_race_still_closes_group(monkeypatch):
    with actor(GRANDCHILD) as (child, record):
        original_kill = os.kill

        def leader_disappears(pid, sig):
            assert pid == child.pid and sig == signal.SIGTERM
            original_kill(pid, signal.SIGKILL)
            child.wait(timeout=5)
            raise ProcessLookupError('leader disappeared after audit')

        with monkeypatch.context() as patch:
            patch.setattr(processes.os, 'kill', leader_disappears)
            result = processes.stop_owned(record, 0.1, 1)
        assert result['status'] == 'ZERO'
        assert result['forced'] is True


def test_unreadable_group_audit_stays_unknown(monkeypatch):
    with actor(SLEEP) as (child, record):
        def unreadable(_):
            raise PermissionError('injected audit boundary')
        with monkeypatch.context() as patch:
            patch.setattr(processes, '_group_members', unreadable)
            assert processes.stop_owned(record, 0.1, 0.1)['status'] == 'UNKNOWN'
        assert child.poll() is None


def test_uncertain_identity_after_term_never_escalates(monkeypatch):
    with actor(REFUSE_TERM) as (child, record):
        original = processes.owned_state
        checks = []

        def becomes_unreadable(value):
            checks.append(value)
            return original(value) if len(checks) == 1 else 'UNKNOWN'

        with monkeypatch.context() as patch:
            patch.setattr(processes, 'owned_state', becomes_unreadable)
            result = processes.stop_owned(record, 0.1, 0.1)
        assert result['status'] == 'UNKNOWN'
        assert result['forced'] is False
        assert len(checks) > 2
        assert child.poll() is None


@pytest.mark.parametrize('persistent', [False, True])
def test_post_term_group_read_failure_is_bounded_without_unknown_signals(monkeypatch, persistent):
    with actor(REFUSE_TERM if persistent else SLEEP) as (child, record):
        original = processes._group_members
        audits = []

        def group_read(value):
            audits.append(value)
            # Initial ownership/group audit succeeds before the first TERM.
            if len(audits) > 1 and (persistent or len(audits) <= 3):
                raise PermissionError('injected post-TERM exit race')
            return original(value)

        with monkeypatch.context() as patch:
            patch.setattr(processes, '_group_members', group_read)
            result = processes.stop_owned(record, 0.5 if not persistent else 0.1, 0.1)
        assert len(audits) >= 4
        assert result['forced'] is False
        if persistent:
            assert result['status'] == 'UNKNOWN' and child.poll() is None
        else:
            assert result['status'] == 'ZERO' and result['reaped'] is True
            # stop_owned owns waitpid's status; Popen only observes it was reaped.
            assert result['exitCode'] == -signal.SIGTERM and child.poll() is not None


def test_existing_group_is_not_an_external_cleanup_target():
    with actor(SLEEP) as (_, owned), actor(SLEEP) as (external_child, external):
        wrong = dict(owned, pid=external['pid'], pgid=external['pgid'])
        assert processes.stop_owned(wrong, 0.1, 0.1)['status'] == 'UNKNOWN'
        assert external_child.poll() is None


def test_post_term_missing_proc_field_retries_without_forcing(monkeypatch):
    script = '''
import os, signal, time
def stop(signum, frame):
    time.sleep(0.2)
    raise SystemExit(0)
signal.signal(signal.SIGTERM, stop)
os.write(1, b'R')
while True:
    time.sleep(1)
'''
    with actor(script) as (child, record):
        identity = processes.process_identity
        group = processes._group_members
        kill = processes.os.kill
        flags = dict(in_group=False, signalled=False, injected=False)

        def read_identity(pid):
            if (flags['in_group'] and flags['signalled'] and not flags['injected'] and
                    pid == child.pid):
                flags['injected'] = True
                raise FileNotFoundError('injected post-TERM executable-field race')
            return identity(pid)

        def read_group(value):
            flags['in_group'] = True
            try:
                return group(value)
            finally:
                flags['in_group'] = False

        def signal_child(pid, sig):
            result = kill(pid, sig)
            if pid == child.pid and sig == signal.SIGTERM:
                flags['signalled'] = True
            return result

        with monkeypatch.context() as patch:
            patch.setattr(processes, 'process_identity', read_identity)
            patch.setattr(processes, '_group_members', read_group)
            patch.setattr(processes.os, 'kill', signal_child)
            result = processes.stop_owned(record, 2, 0.1)
        assert flags['injected'] is True
        assert result['status'] == 'ZERO' and result['reaped'] is True
        assert result['forced'] is False and result['exitCode'] == 0
        assert child.poll() is not None


def test_atomic_operation_record_is_private(tmp_path):
    path = tmp_path / 'state.json'
    processes.write_json(path, {'generation': 1})
    processes.write_json(path, {'generation': 2})
    assert json.loads(path.read_text()) == {'generation': 2}
    assert path.stat().st_mode & 0o777 == 0o600
    assert not list(tmp_path.glob('*.tmp'))


@pytest.mark.parametrize('exit_code,expected', [(7, 'APP_EXIT:finite:7'), (0, 'APP_CLEANUP:finite:')])
def test_finite_first_failure_and_unknown_cleanup_are_retained(tmp_path, monkeypatch,
                                                               exit_code, expected):
    cleanup = []

    def unreadable_group(*_):
        raise PermissionError('injected group audit boundary')

    with monkeypatch.context() as patch:
        patch.setattr(processes.os, 'killpg', unreadable_group)
        with pytest.raises(RuntimeError, match=expected):
            processes.run_finite_application(
                'finite', [sys.executable, '-c', 'raise SystemExit(' + str(exit_code) + ')'],
                tmp_path / 'finite.log', cleanup, seconds=5)
    assert cleanup[0]['exitCode'] == exit_code
    assert cleanup[0]['reaped'] is True
    assert cleanup[0]['error'] == 'injected group audit boundary'
