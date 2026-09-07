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
