"""Host-only systemd ownership for a bounded MiniNDN process tree.

This controls process lifetime, not protocol, numerical or network-interface
qualification. It never issues global MiniNDN cleanup or retries a workload.
"""
from __future__ import annotations

import math
import os
from pathlib import Path
import subprocess
import re
import time
import uuid

from runtime.identities import _credential_document
from runtime.worker import run_finite_application


def _prefix():
    return [] if os.geteuid() == 0 else ['sudo', '-n']


def _control(arguments):
    return subprocess.run(_prefix() + ['/usr/bin/systemctl', '--no-ask-password'] + arguments,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=10)


def _state(unit):
    fields = ('LoadState', 'ActiveState', 'SubState', 'Description', 'ControlGroup',
              'Result', 'ExecMainCode', 'ExecMainStatus')
    result = _control(['show', unit] + ['--property=' + field for field in fields])
    values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    if result.returncode != 0 and values.get('LoadState') != 'not-found':
        raise RuntimeError('HOST_UNIT_STATE_UNAVAILABLE')
    if 'LoadState' not in values:
        raise RuntimeError('HOST_UNIT_STATE_MISSING')
    return values


def _cgroup_roots():
    # systemd 245's hybrid hierarchy and unified v2. Do not mistake a missing
    # or inaccessible cgroup filesystem for proof that the workload is gone.
    choices = (Path('/sys/fs/cgroup/systemd'), Path('/sys/fs/cgroup/unified'))
    roots = [p for p in choices if (p/'cgroup.procs').is_file()]
    if Path('/sys/fs/cgroup/cgroup.controllers').is_file():
        roots.append(Path('/sys/fs/cgroup'))
    if not roots:
        raise RuntimeError('HOST_CGROUP_UNAVAILABLE')
    return roots


def _members(roots, unit):
    observations = []
    for root in roots:
        directory = root/'system.slice'/unit
        pids = set()
        if directory.exists():
            for path in directory.rglob('cgroup.procs'):
                try:
                    pids.update(int(value) for value in path.read_text().split())
                except FileNotFoundError:
                    # A concurrently removed subgroup has no remaining member.
                    pass
        observations.append(dict(path=str(directory), pids=sorted(pids)))
    return observations


def supervise(argv, env, output: Path, *, cwd: Path, seconds: float,
              cleanup_seconds: float) -> int:
    """Run once in a fresh system unit and retain process-tree cleanup evidence.

    systemd owns the hard deadline even if the Python supervisor disappears.
    The finite client owner separately bounds/reaps systemd-run. All control
    queries are bounded, and stop is permitted only for our exact unit identity.
    """
    for value in (seconds, cleanup_seconds):
        if (isinstance(value, bool) or not isinstance(value, (int, float))
                or not math.isfinite(value) or value <= 0):
            raise ValueError('HOST_SUPERVISOR_BUDGET')
    if (not argv or any(not isinstance(v, str) or '\0' in v for v in argv)
            or not Path(argv[0]).is_absolute() or not Path(cwd).is_absolute()
            or not Path(output).is_absolute()
            or any(not isinstance(k, str) or not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*', k)
                   or not isinstance(v, str) or '\0' in v for k,v in env.items())):
        raise ValueError('HOST_SUPERVISOR_INPUT')
    roots = _cgroup_roots()
    output = Path(output)
    output.mkdir(mode=0o700, exist_ok=False)
    nonce = uuid.uuid4().hex
    unit = 'ndnsf-spec183-minindn-' + nonce + '.service'
    description = 'Spec183 MiniNDN ' + nonce
    # Environment belongs to this invocation; neither the manager's environment
    # nor shell expansion may change the prepared application configuration.
    application = ['/usr/bin/env', '-i'] + [str(k)+'='+str(v) for k,v in sorted(env.items())] + list(argv)
    command = _prefix() + ['/usr/bin/systemd-run', '--no-ask-password', '--wait', '--pipe',
        '--unit=' + unit, '--description=' + description, '--service-type=exec',
        '--slice=system.slice', '--working-directory=' + str(cwd),
        '--property=TimeoutStartSec=10',
        '--property=RuntimeMaxSec=' + str(seconds),
        '--property=TimeoutStopSec=' + str(cleanup_seconds),
        '--property=KillMode=mixed', '--property=SendSIGKILL=yes',
        '--property=Restart=no', '--property=UMask=0077', '--'] + application
    _credential_document(output/'owner.json', dict(schema='spec183-host-owner-v1',
        unit=unit, description=description, argv=list(argv), cwd=str(cwd),
        environmentKeys=sorted(env), runtimeSeconds=seconds, cleanupSeconds=cleanup_seconds,
        clientWaitSeconds=seconds+cleanup_seconds+15, controlCallSeconds=10,
        killMode='mixed', restart='no', qualification='NOT_EVALUATED'))
    descriptor = os.open(str(output/'driver.log'), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(descriptor)
    before = _state(unit)
    if before['LoadState'] != 'not-found':
        raise RuntimeError('HOST_UNIT_ALREADY_EXISTS')
    started = time.monotonic()
    cleanup, errors = [], []
    code, failure, state, terminal = None, None, {}, {}
    try:
        code = run_finite_application('minindn-systemd-client', command,
            output/'driver.log', cleanup, seconds=seconds+cleanup_seconds+15,
            cleanup_seconds=10, allowed_exits=tuple(range(256)), cwd=cwd)
    except BaseException as exc:
        failure = exc
    finally:
        try:
            state = _state(unit)
            if state['LoadState'] != 'not-found':
                if state.get('Description') != description:
                    raise RuntimeError('HOST_UNIT_OWNERSHIP_MISMATCH')
                stopped = _control(['stop', unit])
                if stopped.returncode != 0:
                    raise RuntimeError('HOST_UNIT_STOP_FAILED')
                terminal = _state(unit)
            else:
                terminal = state
            if terminal.get('ActiveState') not in ('inactive', 'failed'):
                raise RuntimeError('HOST_UNIT_NOT_TERMINAL')
        except Exception as exc:
            errors.append(type(exc).__name__ + ':' + str(exc))
        try:
            groups = _members(roots, unit)
            if any(row['pids'] for row in groups):
                errors.append('HOST_CGROUP_NOT_EMPTY')
        except Exception as exc:
            groups = []
            errors.append(type(exc).__name__ + ':' + str(exc))
        _credential_document(output/'result.json', dict(schema='spec183-host-owner-result-v1',
            unit=unit, description=description, returncode=code,
            failure=type(failure).__name__ if failure else None,
            elapsedSeconds=time.monotonic()-started, unitState=state,
            terminalState=terminal,
            clientCleanup=cleanup, cgroups=groups, errors=errors,
            processCleanup='CLEAN' if not errors else 'INCOMPLETE',
            qualification='NOT_EVALUATED'))
    if failure is not None:
        raise failure
    if errors:
        raise RuntimeError('HOST_PROCESS_CLEANUP_INCOMPLETE')
    return code
