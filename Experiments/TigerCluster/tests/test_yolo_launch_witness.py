"""Real exec/PID namespace preservation; not exact-SIF qualification."""
import json
from pathlib import Path
import subprocess
import sys
import shutil

import pytest

SCRIPT = Path(__file__).resolve().parents[1]/'runtime/yolo_launch_witness.py'


def test_witness_and_exec_program_have_same_process_identity():
    nonce = 'a'*64
    child = subprocess.Popen([sys.executable, str(SCRIPT), '--nonce', nonce,
        '--role', 'BackboneNeck', '--', sys.executable, '-c',
        'import os; print("APP_PID "+str(os.getpid()))'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = child.communicate(timeout=5)
    assert child.returncode == 0, stderr
    lines = stdout.splitlines()
    assert len(lines) == 2 and lines[0].startswith('TIGER_PROVIDER_PROCESS_STARTED ')
    row = json.loads(lines[0].split(' ', 1)[1])
    assert row == dict(schema='tiger-provider-process-v1', nonce=nonce,
                       role='BackboneNeck', pid=child.pid)
    assert lines[1] == 'APP_PID '+str(row['pid'])


def test_real_pid_namespace_distinguishes_host_launcher_and_provider():
    unshare = shutil.which('unshare')
    if unshare is None:
        pytest.skip('unshare unavailable; exact-SIF PID gate still required')
    child = subprocess.Popen([unshare, '--user', '--map-root-user', '--pid',
        '--fork', '--mount-proc', sys.executable, str(SCRIPT),
        '--nonce', 'b'*64, '--role', 'Merge', '--', sys.executable,
        '-c', 'import os; print("APP_PID "+str(os.getpid()))'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = child.communicate(timeout=5)
    if child.returncode != 0 and 'Operation not permitted' in stderr:
        pytest.skip('user/PID namespaces disabled; exact-SIF PID gate still required')
    assert child.returncode == 0, stderr
    lines = stdout.splitlines()
    row = json.loads(lines[0].split(' ', 1)[1])
    assert row['pid'] != child.pid
    assert row['nonce'] == 'b'*64 and row['role'] == 'Merge'
    assert lines[1] == 'APP_PID '+str(row['pid'])


@pytest.mark.parametrize('extra', [[], ['--nonce', 'bad', '--role', 'Merge', '--', '/bin/true'],
    ['--nonce', 'a'*64, '--role', 'Merge'],
    ['--nonce', 'a'*64, '--role', 'unknown', '--', '/bin/true']])
def test_invalid_identity_cannot_emit_witness(extra):
    child = subprocess.run([sys.executable, str(SCRIPT), *extra],
        capture_output=True, text=True, timeout=5)
    assert child.returncode != 0
    assert 'TIGER_PROVIDER_PROCESS_STARTED' not in child.stdout


@pytest.mark.parametrize('fault', ['none', 'nonce', 'role', 'missing', 'duplicate', 'pid-bool'])
def test_witness_parser_rejects_unbound_or_ambiguous_pid(fault):
    sys.path.insert(0, str(SCRIPT.parents[1]))
    from runtime.yolo_launch_witness import namespace_pid_from_log
    row = dict(schema='tiger-provider-process-v1', nonce='a'*64, role='Merge', pid=1)
    if fault == 'nonce': row['nonce'] = 'b'*64
    if fault == 'role': row['role'] = 'BackboneNeck'
    if fault == 'pid-bool': row['pid'] = True
    data = ('TIGER_PROVIDER_PROCESS_STARTED '+json.dumps(row)+'\n').encode()
    if fault == 'missing': data = b''
    if fault == 'duplicate': data *= 2
    if fault == 'none':
        assert namespace_pid_from_log(data, nonce='a'*64, role='Merge') == 1
    else:
        with pytest.raises(ValueError): namespace_pid_from_log(data, nonce='a'*64, role='Merge')


def test_native_reader_uses_observed_namespace_pid_not_host_pid(tmp_path):
    sys.path.insert(0, str(SCRIPT.parents[1]))
    from runtime.yolo_result import read_native_observation
    from test_yolo_native_observation import observation
    unshare = shutil.which('unshare')
    if unshare is None: pytest.skip('unshare unavailable')
    code = ('import os,sys,json; row=json.loads(sys.argv[1]); '
            'row["processId"]=str(os.getpid()); '
            'print("NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED "+json.dumps(row))')
    child = subprocess.Popen([unshare, '--user', '--map-root-user', '--pid', '--fork',
        '--mount-proc', sys.executable, str(SCRIPT), '--nonce', 'c'*64,
        '--role', 'BackboneNeck', '--', sys.executable, '-c', code, json.dumps(observation())],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = child.communicate(timeout=5)
    if child.returncode != 0 and 'Operation not permitted' in stderr:
        pytest.skip('user/PID namespaces disabled')
    assert child.returncode == 0, stderr
    log = tmp_path/'provider.log'
    log.write_text(stdout)
    binding = dict(provider='/app/worker-a', role='BackboneNeck', request_id='/app/request/1',
        attempt=1, plan_digest='sha256:'+'1'*64, pid=child.pid, runner_kind='onnxruntime-cuda')
    evidence = read_native_observation(log, launch_nonce='c'*64, **binding)
    assert evidence['hostProcessId'] == child.pid
    assert evidence['observation']['processId'] != child.pid
    with pytest.raises(ValueError): read_native_observation(log, **binding)
