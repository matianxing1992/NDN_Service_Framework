"""Systemd ownership boundary doubles; real probes are retained separately."""
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import host_minindn as owner


@pytest.mark.parametrize('fault', ['none', 'exit', 'timeout', 'foreign', 'members', 'state'])
def test_host_owner_retains_identity_deadline_and_cleanup_boundary(tmp_path, monkeypatch, fault):
    calls, launches = [], []
    root = tmp_path/'cgroup'
    root.mkdir()
    monkeypatch.setattr(owner, '_cgroup_roots', lambda: [root])
    def state(unit):
        calls.append(('show', unit))
        count = sum(c[0] == 'show' for c in calls)
        if count == 1:
            return dict(LoadState='not-found', ActiveState='inactive')
        if fault == 'state': raise RuntimeError('state unavailable')
        if count == 2:
            description = 'Spec183 MiniNDN '+unit[len('ndnsf-spec183-minindn-'):-len('.service')]
            return dict(LoadState='loaded', ActiveState='failed',
                        Description='foreign' if fault == 'foreign' else description)
        return dict(LoadState='not-found', ActiveState='inactive')
    monkeypatch.setattr(owner, '_state', state)
    def control(argv):
        calls.append(tuple(argv))
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(owner, '_control', control)
    def finite(name, argv, log, cleanup, **kwargs):
        launches.append((argv,kwargs))
        assert Path(log).stat().st_mode & 0o777 == 0o600
        cleanup.append(dict(reaped=True, exitCode=0))
        if fault == 'members':
            unit = next(v.split('=',1)[1] for v in argv if v.startswith('--unit='))
            group = root/'system.slice'/unit/'child'
            group.mkdir(parents=True)
            (group/'cgroup.procs').write_text('123\n')
        if fault == 'timeout': raise subprocess.TimeoutExpired('client', 18)
        return 7 if fault == 'exit' else 0
    monkeypatch.setattr(owner, 'run_finite_application', finite)
    output = tmp_path/'run'
    arguments = (['/usr/bin/python3','-c','pass'], {'PATH':'/usr/bin:/bin'}, output)
    if fault in ('foreign','members','state'):
        with pytest.raises(RuntimeError, match='HOST_PROCESS_CLEANUP_INCOMPLETE'):
            owner.supervise(*arguments, cwd=tmp_path, seconds=2, cleanup_seconds=1)
    elif fault == 'timeout':
        with pytest.raises(subprocess.TimeoutExpired):
            owner.supervise(*arguments, cwd=tmp_path, seconds=2, cleanup_seconds=1)
    else:
        assert owner.supervise(*arguments, cwd=tmp_path, seconds=2, cleanup_seconds=1) == (7 if fault=='exit' else 0)
    record = json.loads((output/'result.json').read_text())
    assert record['qualification'] == 'NOT_EVALUATED'
    assert record['processCleanup'] == ('INCOMPLETE' if fault in ('foreign','members','state') else 'CLEAN')
    assert bool([c for c in calls if c[0]=='stop']) == (fault not in ('foreign','state'))
    argv, options = launches[0]
    assert '--property=RuntimeMaxSec=2' in argv
    assert '--property=TimeoutStartSec=10' in argv
    assert '--property=TimeoutStopSec=1' in argv
    assert '--property=KillMode=mixed' in argv and '--property=Restart=no' in argv
    assert argv[argv.index('/usr/bin/env'):] == [
        '/usr/bin/env', '-i', 'PATH=/usr/bin:/bin', '/usr/bin/python3', '-c', 'pass']
    assert options['seconds'] == 18
    before = len(launches)
    with pytest.raises(FileExistsError):
        owner.supervise(*arguments, cwd=tmp_path, seconds=2, cleanup_seconds=1)
    assert len(launches) == before


@pytest.mark.parametrize('seconds', [0, -1, True, float('inf'), float('nan')])
def test_host_invalid_budget_creates_no_owner(tmp_path, seconds):
    with pytest.raises(ValueError, match='HOST_SUPERVISOR_BUDGET'):
        owner.supervise(['/usr/bin/true'], {}, tmp_path/'run', cwd=tmp_path,
                        seconds=seconds, cleanup_seconds=1)
    assert not (tmp_path/'run').exists()
