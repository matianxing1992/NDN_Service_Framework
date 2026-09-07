"""Native argv wiring at the process boundary; not model/SIF qualification."""
import subprocess
import sys

import pytest

from test_yolo_worker import prepared
from runtime.yolo_worker import NodeRuntime


def application_inputs(tmp_path, rank, mode='two-node-gpu'):
    inputs = prepared(tmp_path, rank=rank, mode=mode)
    for name in ('native-execution-plan.json', 'service-manifest.json', 'trust-schema.conf'):
        (inputs['public'] / name).write_text('test fixture, not a qualified document')
    for home in inputs['homes'].values():
        (home / 'offer.pem').write_text('test fixture, not a key')
    return inputs


@pytest.mark.parametrize('cpu', [False, True])
@pytest.mark.parametrize('role,rank', [('BackboneNeck', 0), ('Merge', 0),
                                    ('DetectShard0', 1), ('DetectShard1', 1)])
def test_native_provider_argv_reaches_process_boundary(tmp_path, monkeypatch, role, rank, cpu):
    inputs = application_inputs(tmp_path, 0 if cpu else rank,
                                'local-cpu' if cpu else 'two-node-gpu')
    worker = NodeRuntime(**inputs)
    original = subprocess.Popen
    observed = []

    def boundary(argv, **kwargs):
        observed.append(argv)
        return original([sys.executable, '-c', 'import time; time.sleep(60)'], **kwargs)

    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        worker.start_provider(role, identity='/run/actual/' + role,
                              service='/Detection/YOLO', group='/run/sync',
                              controller='/run/controller', permission_wait_ms=12000)
        argv, = observed
        assert argv[argv.index('--provider') + 1] == '/run/actual/' + role
        assert argv[argv.index('--roles') + 1] == role
        assert argv[argv.index('--artifact-cache-dir') + 1] == '/output/artifact-cache'
        assert argv[argv.index('--selection-offer-key-file') + 1] == '/identities/' + role + '/offer.pem'
        assert argv[argv.index('--permission-wait-ms') + 1] == '12000'
        assert argv[argv.index('--offer-device') + 1] == ('cpu' if cpu or role == 'Merge' else 'cuda:0')
        assert ('--nv' in argv) == (not cpu and role != 'Merge')
        assert '--offer-has-model' not in argv
        assert not any(':/artifacts:' in arg for arg in argv)
    finally:
        assert all(row['reaped'] for row in worker.close())


@pytest.mark.parametrize('fault', ['missing-plan', 'missing-key', 'foreign-key',
                                  'wrong-role', 'bad-name', 'bool-budget', 'long-budget'])
def test_provider_rejects_bad_inputs_before_process_or_lease(tmp_path, fault):
    inputs = application_inputs(tmp_path, 0)
    worker = NodeRuntime(**inputs)
    options = dict(role='BackboneNeck', identity='/run/provider', service='/YOLO',
                   group='/run/sync', controller='/run/controller', permission_wait_ms=12000)
    if fault == 'missing-plan':
        (inputs['public'] / 'native-execution-plan.json').unlink()
    elif fault in ('missing-key', 'foreign-key'):
        key = inputs['homes']['BackboneNeck'] / 'offer.pem'
        key.unlink()
        if fault == 'foreign-key':
            key.symlink_to(inputs['homes']['Merge'] / 'offer.pem')
    elif fault == 'wrong-role':
        options['role'] = 'DetectShard0'
    elif fault == 'bad-name':
        options['identity'] = '/bad\nname'
    elif fault == 'bool-budget':
        options['permission_wait_ms'] = True
    else:
        options['permission_wait_ms'] = 120001
    try:
        with pytest.raises(ValueError, match='WORKER_'):
            worker.start_provider(**options)
        assert worker.launches == [] and worker.leases == {}
    finally:
        worker.close()


def scheduled_inputs(tmp_path, mode='two-node-gpu'):
    inputs = application_inputs(tmp_path, 0, mode)
    for name in ('case.json', 'catalogue-registry.json', 'offer-trust-root.json', 'offer-public-key-map.json'):
        (inputs['public'] / name).write_text('synthetic fixture')
    (inputs['homes']['user'] / 'request-envelope.key').write_bytes(b'x' * 32)
    package = tmp_path / 'package'
    package.mkdir()
    worker = NodeRuntime(**inputs)
    count = 4 if mode == 'two-node-gpu' else 2
    plan = {'case': mode, 'requests': [
        dict(index=i, warmup=i == 0, requestId='/run/request/' + str(i),
             output=str(worker.output / 'user' / 'requests' / str(i))) for i in range(count)]}
    return worker, plan, dict(package=package, catalog_data_name='/run/catalog/v=1',
        catalog_signer='/run/controller', permission_wait_ms=1000,
        request_deadline_ms=5000, process_timeout_seconds=10)


@pytest.mark.parametrize('mode,count', [('two-node-gpu', 4), ('single-node-gpu', 2), ('local-cpu', 2)])
def test_schedule_runs_finite_users_preserving_live_provider(tmp_path, monkeypatch, mode, count):
    from apps.yolo import run_requests
    worker, plan, options = scheduled_inputs(tmp_path, mode)
    native = subprocess.Popen
    calls, accepted = [], []

    def boundary(argv, **kwargs):
        calls.append(argv)
        return native([sys.executable, '-c', 'pass'], **kwargs)

    try:
        provider = worker.start_service('BackboneNeck', [sys.executable, '-c', 'import time;time.sleep(60)'])
        monkeypatch.setattr(subprocess, 'Popen', boundary)
        def accept(request, output):
            assert provider.poll() is None
            assert output.is_dir()
            accepted.append(request['requestId'])
        run_requests(worker, plan, accept_request=accept, **options)
        assert len(calls) == len(accepted) == count
        assert len({a[a.index('--request-id') + 1] for a in calls}) == count
        assert len({a[a.index('--lifecycle-output-dir') + 1] for a in calls}) == count
        assert all('--offline-oracle' not in a and '--sequential-requests' not in a for a in calls)
        assert all(a[a.index('--generated-policy-dir') + 1].startswith('/output/requests/') for a in calls)
        with pytest.raises(ValueError, match='WORKER_INVOCATION_REUSED'):
            run_requests(worker, plan, accept_request=accept, **options)
        assert len(calls) == count
    finally:
        rows = worker.close()
        assert len([r for r in rows if r.get('kind') == 'finite']) == count
        assert all(r['reaped'] and not r['forced'] for r in rows)


@pytest.mark.parametrize('failure', ['process', 'evidence', 'peer'])
def test_schedule_stops_at_first_failure_and_preserves_output(tmp_path, monkeypatch, failure):
    from apps.yolo import run_requests
    worker, plan, options = scheduled_inputs(tmp_path)
    native, calls = subprocess.Popen, []
    peer = tmp_path / 'peer-failed.json'

    def boundary(argv, **kwargs):
        calls.append(argv)
        return native([sys.executable, '-c', 'raise SystemExit(7)' if failure == 'process' else 'pass'], **kwargs)

    def accept(*_):
        if failure == 'peer':
            peer.write_text('peer failure fixture')
        else:
            raise RuntimeError('NUMERIC_MISMATCH')

    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        with pytest.raises(RuntimeError, match='APP_EXIT|NUMERIC_MISMATCH|PEER_FAILED'):
            run_requests(worker, plan, accept_request=accept, peer_failure=peer, **options)
        assert len(calls) == 1
        assert (worker.output / 'user/requests/0').is_dir()
        assert not (worker.output / 'user/requests/1').exists()
    finally:
        assert all(r['reaped'] for r in worker.close())


@pytest.mark.parametrize('fault', ['bare-id', 'duplicate-id', 'wrong-output', 'missing-key', 'wrong-count'])
def test_invalid_schedule_rejects_before_user_launch(tmp_path, fault):
    from apps.yolo import run_requests
    worker, plan, options = scheduled_inputs(tmp_path)
    if fault == 'bare-id':
        plan['requests'][0]['requestId'] = 'barehash'
    elif fault == 'duplicate-id':
        plan['requests'][1]['requestId'] = plan['requests'][0]['requestId']
    elif fault == 'wrong-output':
        plan['requests'][0]['output'] = str(tmp_path / 'wrong')
    elif fault == 'missing-key':
        (worker.homes['user'] / 'request-envelope.key').unlink()
    else:
        plan['requests'].pop()
    try:
        with pytest.raises(ValueError, match='YOLO_'):
            run_requests(worker, plan, accept_request=lambda *_: None, **options)
        assert worker.launches == []
        assert not (worker.output / 'user').exists()
    finally:
        worker.close()
