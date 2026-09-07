"""Real file barriers with explicit application/Worker test doubles."""
import concurrent.futures
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo
from runtime.yolo_worker import StartupBarrier, PROVIDER_ROLES, assigned_roles
from test_yolo_publication_readiness import documents


def barrier(root, rank=0, seconds=3):
    return StartupBarrier(root, run_id='test-run', probe_id='a' * 32,
        candidate_digest='sha256:' + 'b' * 64, ranks=(0, 1), rank=rank, seconds=seconds, check=lambda: None)


@pytest.mark.parametrize('fault', ['none', 'stale', 'symlink', 'failed', 'missing'])
def test_run_bound_barrier_rejects_invalid_or_missing_records(tmp_path, fault):
    a, b = barrier(tmp_path, seconds=0.1), barrier(tmp_path, 1, seconds=0.1)
    a.publish('routes-ready', {'syncPrefix': '/app/sync'})
    if fault != 'missing':
        b.publish('routes-ready', {'syncPrefix': '/app/sync'})
    path = tmp_path / 'routes-ready-1.json'
    if fault == 'stale':
        record = json.loads(path.read_text())
        record['probeId'] = 'c' * 32
        path.write_text(json.dumps(record))
    if fault == 'symlink':
        path.rename(tmp_path / 'original')
        path.symlink_to(tmp_path / 'original')
    if fault == 'failed':
        b.publish('failed', {'errorType': 'TestFailure'})
    if fault == 'none':
        assert len(a.wait('routes-ready')) == 2
        with pytest.raises(FileExistsError):
            a.publish('routes-ready', {})
        assert not list(tmp_path.glob('.stage-*'))
    else:
        with pytest.raises((ValueError, RuntimeError, TimeoutError)):
            a.wait('routes-ready')


@pytest.mark.parametrize('fault', ['none', 'wrong-sync', 'wrong-peer', 'provider-failed'])
def test_two_rank_startup_orders_actual_application_seams(tmp_path, monkeypatch, fault):
    namespace = '/run/test'
    identities = {r: namespace + '/' + r for i in (0, 1) for r in assigned_roles('two-node-gpu', i)}
    plan = dict(runId='test-run', namespace=namespace, applicationName=namespace + '/app', identities=identities,
                nodes=[{'rank': i, 'roles': list(assigned_roles('two-node-gpu', i))} for i in (0, 1)])
    public, shared = tmp_path / 'public', tmp_path / 'barriers'
    public.mkdir(); shared.mkdir()
    config = dict(controller=identities['controller'], group=namespace + '/app/sync',
                  runtime={'application_name': namespace + '/app'},
                  services=[{'name': '/YOLO', 'roles': sorted(PROVIDER_ROLES)}])
    (public / 'case.json').write_text(json.dumps(config))
    expected, publication = documents()
    (public / 'runtime-publication.json').write_text(json.dumps(expected))
    barriers = [barrier(shared, i) for i in (0, 1)]
    for b in barriers:
        b.publish('routes-ready', {'namespace': namespace, 'syncPrefix': config['group'] if fault != 'wrong-sync' else namespace + '/group'})
    events = []
    def network(worker, **kwargs):
        assert not any(e[0] == 'controller' for e in events)
        own, peer = ('BackboneNeck', 'DetectShard0') if worker.rank == 0 else ('DetectShard0', 'BackboneNeck')
        result = dict(schema='tiger-yolo-network-readiness-v1', probeId=kwargs['probe_id'],
            producer=identities[own], peer=identities[peer],
            receivedName=identities[peer] + '/SPEC183-NETWORK/' + kwargs['probe_id'] + '/data',
            status='READY', qualification='NOT_EVALUATED')
        if fault == 'wrong-peer' and worker.rank == 1:
            result['peer'] = '/foreign'
        events.append(('network', worker.rank))
        return result
    monkeypatch.setattr(yolo, 'wait_network_ready', network)
    def controller(worker):
        assert sorted(e[1] for e in events if e[0] == 'network') == [0, 1]
        events.append(('controller', 0))
    monkeypatch.setattr(yolo, 'start_controller', controller)
    monkeypatch.setattr(yolo, 'wait_controller_publication', lambda worker, **kw: publication)
    monkeypatch.setattr(yolo, 'start_repo', lambda worker, **kw: events.append(('repo', 0)))
    monkeypatch.setattr(yolo, 'wait_repo_ready', lambda worker, **kw:
                        {'status': 'READY', 'repo': identities['repo']})
    def start(role, **kwargs):
        assert ('repo', 0) in events
        assert kwargs['group'] == namespace + '/app/sync'
        events.append(('provider', role))
    def ready(worker, role, **kwargs):
        if fault == 'provider-failed' and role == 'DetectShard0':
            raise RuntimeError('test permission failure')
    monkeypatch.setattr(yolo, 'wait_provider_ready', ready)
    workers = [SimpleNamespace(rank=i, mode='two-node-gpu', roles=assigned_roles('two-node-gpu', i),
        _preparation_binding=(plan, 'receipt', 'sha256:' + 'b' * 64), public=public,
        cleanup_seconds=0.01, start_provider=start) for i in (0, 1)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(yolo.start_workload, workers[i], barriers[i], repo_free_bytes=100,
                              permission_wait_ms=1000, network_probe_seconds=0.1) for i in (0, 1)]
        if fault == 'none':
            results = [f.result(timeout=5) for f in futures]
            assert all(r['status'] == 'RUNTIME_READY' for r in results)
            assert {e[1] for e in events if e[0] == 'provider'} == PROVIDER_ROLES
        else:
            for f in futures:
                with pytest.raises((ValueError, RuntimeError)):
                    f.result(timeout=5)
            assert list(shared.glob('failed-*.json'))
