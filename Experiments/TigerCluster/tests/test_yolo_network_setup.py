"""Real finite children/UNIX sockets; nfd/nfdc are explicit OS-boundary doubles."""
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps.yolo import configure_network
from runtime.baseline import BIN, route_commands
from runtime.yolo_worker import NodeRuntime, StartupBarrier, assigned_roles
from test_yolo_worker import prepared


def test_route_commands_use_explicit_app_sync_without_legacy_group():
    calls = route_commands('/run', '192.0.2.2', 16463, sync_prefix='/run/yolo-app/sync')
    assert calls[2] == ['strategy', 'set', 'prefix', '/run/yolo-app/sync',
                        'strategy', '/localhost/nfd/strategy/multicast']
    assert all('/run/group' not in row for row in calls)
    with pytest.raises(ValueError):
        route_commands('/run', '192.0.2.2', True, sync_prefix='/run/app/sync')
    with pytest.raises(ValueError):
        route_commands('/run', '192.0.2.2', 16463, sync_prefix='/foreign/sync')


@pytest.mark.parametrize('mode,fault', [
    ('two-node-gpu', 'none'), ('local-cpu', 'none'),
    ('two-node-gpu', 'command'), ('local-cpu', 'socket-file'),
    ('two-node-gpu', 'wrong-port'), ('two-node-gpu', 'same-address')])
def test_network_setup_uses_owned_commands_and_stops_on_failure(tmp_path, monkeypatch, mode, fault):
    ranks = (0,) if mode == 'local-cpu' else (0, 1)
    shared = tmp_path / 'barriers'
    shared.mkdir()
    plan = dict(runId='network-run', namespace='/run', applicationName='/run/app',
                nodes=[{'rank': i, 'roles': list(assigned_roles(mode, i))} for i in ranks],
                effectiveBehavior={'profile': {'cluster': {'tcpPort': 16463}}})
    endpoints = [{'rank': i, 'address': '127.0.0.1' if len(ranks) == 1 else '192.0.2.' + str(i + 1),
                  'port': 16463} for i in ranks]
    if fault == 'wrong-port':
        endpoints[0]['port'] += 1
    if fault == 'same-address':
        endpoints[1]['address'] = endpoints[0]['address']
    workers, barriers = [], []
    for rank in ranks:
        worker = NodeRuntime(**prepared(tmp_path / str(rank), rank=rank, mode=mode))
        worker._preparation_binding = (plan, 'receipt-fixture', 'sha256:' + 'b' * 64)
        monkeypatch.setattr(worker, '_verify_prepared_boundary', lambda: None)
        (worker.public / 'case.json').write_text(json.dumps({
            'group': '/run/app/sync', 'runtime': {'application_name': '/run/app'}}))
        workers.append(worker)
        barriers.append(StartupBarrier(shared, run_id=plan['runId'], probe_id='a' * 32,
            candidate_digest='sha256:' + 'b' * 64, ranks=ranks, rank=rank, seconds=5, check=worker.check))
    original, calls = subprocess.Popen, []
    def boundary(argv, **kwargs):
        calls.append(argv)
        if BIN + '/nfd' in argv:
            node = next(arg.split(':/node:')[0] for arg in argv if ':/node:' in arg)
            code = ('import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); time.sleep(60)'
                    if fault != 'socket-file' else
                    'import pathlib,sys,time; pathlib.Path(sys.argv[1]).touch(); time.sleep(60)')
            return original([sys.executable, '-c', code, str(Path(node) / 'nfd.sock')], **kwargs)
        assert BIN + '/nfdc' in argv
        assert '--nv' not in argv and not any(':/artifacts:' in a for a in argv)
        home = argv[argv.index('--home') + 1]
        assert home.endswith(':/identities/BackboneNeck') or home.endswith(':/identities/DetectShard0')
        rc = 4 if fault == 'command' and 'route' in argv and 'add' in argv else 0
        return original([sys.executable, '-c', 'import sys; sys.exit(' + str(rc) + ')'], **kwargs)
    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(ranks)) as pool:
            futures = [pool.submit(configure_network, w, b, endpoints=endpoints)
                       for w, b in zip(workers, barriers)]
            if fault == 'none':
                rows = [f.result(timeout=8) for f in futures]
                assert all(len(row) == (4 if len(ranks) == 1 else 6) for row in rows)
                assert len(barriers[0].wait('routes-ready')) == len(ranks)
                for w in workers:
                    receipt = json.loads((w.output / 'network-setup.json').read_text())
                    assert receipt['syncPrefix'] == '/run/app/sync'
                    assert receipt['qualification'] == 'NOT_EVALUATED'
                    assert set(w.leases) == {'nfd' + str(w.rank)}
            else:
                for f in futures:
                    with pytest.raises((ValueError, RuntimeError)):
                        f.result(timeout=8)
                assert not list(shared.glob('routes-ready-*.json'))
                if fault in ('wrong-port', 'same-address'):
                    assert not calls  # Preflight rejected before any process launch.
    finally:
        for worker in workers:
            assert all(row['reaped'] and not row['forced'] for row in worker.close())
