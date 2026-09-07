"""Real launch/receipt boundaries; native Repo/User are explicit test doubles.

These are component regressions, not permission, SIF or inference qualification.
"""
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo
from runtime import identities, yolo_profile
from runtime.yolo_worker import NodeRuntime
from test_yolo_worker import prepared


@pytest.mark.parametrize('fault', ['none', 'rpc-once', 'wrong-repo', 'close', 'start', 'timeout', 'late'])
def test_repo_probe_actual_api_contract_and_cleanup(monkeypatch, fault):
    if fault in ('timeout', 'late'):
        import time
        ticks = iter([0, 0.1, 0.2, 2.0] if fault == 'timeout' else [0, 0.1, 2.0])
        monkeypatch.setattr(time, 'monotonic', lambda: next(ticks))
        monkeypatch.setattr(time, 'sleep', lambda duration: None)
    names = {role: '/run/test/' + role for role in ('user', 'controller', 'repo')}
    config = dict(group='/run/test/sync', controller=names['controller'],
                  trust={'anchor_file': '/config/root.cert'}, runtime=dict(
                      provider_prefix='/run/test', user_identity=names['user'], identities=names))
    monkeypatch.setattr(yolo_profile, '_read_plane', lambda path: config)
    events, documents = [], []
    monkeypatch.setattr(identities, '_credential_document', lambda path, value: documents.append(value))

    class User:
        def __init__(self, **kwargs):
            assert kwargs['adaptive_admission'] is False
            assert kwargs['user'] == names['user']
            assert kwargs['trust_schema'] == '/config/trust-schema.conf'
        def start(self):
            events.append('start')
            if fault == 'start':
                raise RuntimeError('test start')
        def stop(self):
            events.append('stop')

    class Repo:
        def __init__(self, **kwargs):
            assert kwargs['control_mode'] == 'normal'
            assert kwargs['enable_targeted_fallback'] is False
        def capability(self, **kwargs):
            assert 0 < kwargs['timeout_ms'] <= 3000
            events.append('capability')
            if fault == 'timeout' or fault == 'rpc-once' and events.count('capability') == 1:
                raise RuntimeError('test startup transient')
            return {'repoNode': '/wrong' if fault == 'wrong-repo' else names['repo']}
        def close(self):
            events.append('close')
            if fault == 'close':
                raise RuntimeError('test close')

    monkeypatch.setitem(sys.modules, 'ndnsf', SimpleNamespace(ServiceUser=User))
    monkeypatch.setitem(sys.modules, 'py_repoclient.orchestration',
                        SimpleNamespace(NetworkDistributedRepoClient=Repo))
    if fault in ('wrong-repo', 'close', 'start', 'timeout', 'late'):
        with pytest.raises((ValueError, RuntimeError, TimeoutError)):
            yolo.probe_repo_in_container('a' * 32, 1)
        assert documents == []
    else:
        result = yolo.probe_repo_in_container('a' * 32, 1)
        assert result['qualification'] == 'NOT_EVALUATED'
        assert result['attempts'] == (2 if fault == 'rpc-once' else 1)
        assert documents == [result]
    assert events[-1] == 'stop'
    if fault != 'start':
        assert events[-2] == 'close'


@pytest.mark.parametrize('fault', ['none', 'stale', 'exit', 'wrong-node', 'symlink'])
def test_parent_consumes_fresh_bound_receipt(tmp_path, fault):
    names = dict(user='/run/test/user', repo='/run/test/repo')
    worker = SimpleNamespace(rank=0, _preparation_binding=({'identities': names},),
        cleanup_seconds=2, output=tmp_path, _verify_prepared_boundary=lambda: None, check=lambda: None)
    def run(invocation, argv, **kwargs):
        assert invocation == 'repo-readiness'
        assert kwargs['package'] is None  # No model mount for readiness.
        assert kwargs['seconds'] == 3
        probe_id = argv[argv.index('--probe-id') + 1]
        receipt = dict(schema='tiger-yolo-repo-readiness-v1', probeId=probe_id, **names,
            status='READY', qualification='NOT_EVALUATED', attempts=1,
            capability={'repoNode': names['repo']})
        if fault == 'stale':
            receipt['probeId'] = 'old'
        if fault == 'wrong-node':
            receipt['capability']['repoNode'] = '/foreign'
        path = tmp_path / 'user/requests/repo-readiness/receipt.json'
        path.parent.mkdir(parents=True)
        if fault == 'symlink':
            other = tmp_path / 'other.json'
            other.write_text(json.dumps(receipt))
            path.symlink_to(other)
        else:
            path.write_text(json.dumps(receipt))
        return 2 if fault == 'exit' else 0
    worker.run_user = run
    if fault == 'none':
        assert yolo.wait_repo_ready(worker, seconds=1)['status'] == 'READY'
    else:
        with pytest.raises((ValueError, RuntimeError)):
            yolo.wait_repo_ready(worker, seconds=1)


def test_finite_probe_owns_user_home_without_model_mount(tmp_path, monkeypatch):
    worker = NodeRuntime(**prepared(tmp_path, rank=0))
    original, calls = subprocess.Popen, []
    def boundary(argv, **kwargs):
        calls.append(argv)
        return original([sys.executable, '-c', 'pass'], **kwargs)
    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        assert worker.run_user('repo-readiness', ['python3', '-m', 'apps.yolo', 'repo-probe'],
                               package=None, seconds=3) == 0
        assert not any(':/artifacts:' in arg for arg in calls[0])
        assert worker.leases == {}
        with pytest.raises(ValueError, match='REUSED'):
            worker.run_user('repo-readiness', ['python3'], package=None, seconds=3)
    finally:
        assert all(row['reaped'] and not row['forced'] for row in worker.close())


@pytest.mark.parametrize('seconds', [True, 0, -1, float('inf'), 301])
def test_probe_rejects_invalid_budget_before_native_import(seconds):
    with pytest.raises(ValueError, match='ARGUMENTS'):
        yolo.probe_repo_in_container('a' * 32, seconds)
