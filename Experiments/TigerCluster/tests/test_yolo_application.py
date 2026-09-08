"""Native argv wiring at the process boundary; not model/SIF qualification."""
import subprocess
import json
import sys

import pytest

from test_yolo_worker import prepared
from runtime.yolo_worker import NodeRuntime


def test_reference_cli_forwards_bound_user_arguments(monkeypatch, tmp_path):
    import apps.yolo as app
    observed = []
    monkeypatch.setattr(app, 'run_user_with_reference',
        lambda argv, **kwargs: observed.append((argv, kwargs)) or 0)
    prefix = ['user', '--reference-backend', 'CPUExecutionProvider',
        '--reference-run-id', 'run', '--reference-request-id', '/run/0',
        '--reference-candidate-digest', 'sha256:'+'a'*64,
        '--reference-placement-candidate-digest', 'sha256:'+'b'*64,
        '--reference-output', str(tmp_path), '--']
    forwarded = ['--request-id', '/run/0', '--lifecycle-output-dir', str(tmp_path)]
    assert app.main(prefix + forwarded) == 0
    assert observed[0][0] == forwarded
    assert observed[0][1]['request_id'] == '/run/0'
    for bad in [forwarded + ['--request-id', '/different'],
                forwarded + ['--request-id=/different'],
                ['--request-id', '/different', '--lifecycle-output-dir', str(tmp_path)]]:
        assert app.main(prefix + bad) != 0
    assert len(observed) == 1


def application_inputs(tmp_path, rank, mode='two-node-gpu'):
    inputs = prepared(tmp_path, rank=rank, mode=mode)
    for name in ('native-execution-plan.json', 'service-manifest.json', 'trust-schema.conf'):
        (inputs['public'] / name).write_text('test fixture, not a qualified document')
    for home in inputs['homes'].values():
        (home / 'offer.pem').write_text('test fixture, not a key')
        (home / 'recipient.pem').write_text('fixture, not a parsed key')
        (home / 'recipient.pem').chmod(0o600)
        (home / 'recipient-map.json').write_text(json.dumps({
            '/run/actual/' + home.name: '/identities/' + home.name + '/recipient.pem'}))
    (inputs['public'] / 'contracts').mkdir()
    for name in ('trust-root-registry-v1.json', 'authority.pub'):
        (inputs['public'] / 'contracts' / name).write_text('fixture, not authenticated')
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
        assert '--withhold-v3-output' not in argv
        assert 'SPEC181_GRANT_AUTHORITY_PUBLIC_KEY=/config/contracts/authority.pub' in argv
        assert ('SPEC181_PROVIDER_RECIPIENT_KEY_MAP=/identities/' + role + '/recipient-map.json') in argv
        assert not any(':/artifacts:' in arg for arg in argv)
    finally:
        assert all(row['reaped'] for row in worker.close())


@pytest.mark.parametrize('role',['DetectShard0','DetectShard1'])
def test_negative_provider_selects_only_bound_head0_output(tmp_path,monkeypatch,role):
    worker=NodeRuntime(**application_inputs(tmp_path,1,'negative-dependency'))
    worker._preparation_binding=({'case':'negative-dependency','requests':[
        {'index':0,'warmup':False,'requestId':'/run/one-negative'}]},'fixture','fixture')
    observed=[]
    monkeypatch.setattr(worker,'_start_service',lambda role,argv:observed.append(argv))
    try:
        worker.start_provider(role,identity='/run/actual/'+role,service='/Detection/YOLO',
            group='/run/sync',controller='/run/controller',permission_wait_ms=12000)
        if role=='DetectShard0':
            assert observed[0][-4:]==['--withhold-v3-output','/run/one-negative','DetectShard0','Merge']
        else: assert '--withhold-v3-output' not in observed[0]
    finally: worker.close()


def test_negative_merge_enables_native_failure_evidence_at_process_boundary(tmp_path, monkeypatch):
    worker = NodeRuntime(**application_inputs(tmp_path, 0, 'negative-dependency'))
    original, observed = subprocess.Popen, []
    def boundary(argv, **kwargs):
        observed.append(argv)
        return original([sys.executable, '-c', 'import time; time.sleep(60)'], **kwargs)
    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        worker._start_service('Merge', ['/fixture-native'])
        assert 'NDNSF_DI_RUNTIME_TIMING=1' in observed[0]
        assert '--withhold-v3-output' not in observed[0]
    finally:
        assert all(row['reaped'] and not row['forced'] for row in worker.close())


def test_negative_provider_requires_single_nonwarmup_prepared_request(tmp_path,monkeypatch):
    worker=NodeRuntime(**application_inputs(tmp_path,1,'negative-dependency'))
    monkeypatch.setattr(worker,'_start_service',lambda *a:pytest.fail('unbound fault launched'))
    try:
        for plan in (None,{'case':'negative-dependency','requests':[]},
                     {'case':'two-node-gpu','requests':[{'index':0,'warmup':False,'requestId':'/r'}]},
                     {'case':'negative-dependency','requests':[{'index':0,'warmup':True,'requestId':'/r'}]}):
            worker._preparation_binding=None if plan is None else (plan,'fixture','fixture')
            with pytest.raises(ValueError,match='WORKER_FAULT_'):
                worker.start_provider('DetectShard0',identity='/run/actual/DetectShard0',service='/Detection/YOLO',
                    group='/run/sync',controller='/run/controller',permission_wait_ms=12000)
    finally: worker.close()


@pytest.mark.parametrize('fault', ['missing-plan', 'missing-key', 'foreign-key',
                                  'wrong-role', 'bad-name', 'bool-budget', 'long-budget',
                                  'recipient-missing', 'recipient-permissions', 'recipient-peer',
                                  'recipient-duplicate', 'recipient-oversized', 'registry-missing'])
def test_provider_rejects_bad_inputs_before_process_or_lease(tmp_path, fault):
    inputs = application_inputs(tmp_path, 0)
    worker = NodeRuntime(**inputs)
    options = dict(role='BackboneNeck', identity='/run/actual/BackboneNeck', service='/YOLO',
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
    elif fault == 'long-budget':
        options['permission_wait_ms'] = 120001
    elif fault == 'registry-missing':
        (inputs['public'] / 'contracts/trust-root-registry-v1.json').unlink()
    else:
        home = inputs['homes']['BackboneNeck']
        if fault == 'recipient-missing':
            (home / 'recipient.pem').unlink()
        elif fault == 'recipient-permissions':
            (home / 'recipient.pem').chmod(0o644)
        elif fault == 'recipient-peer':
            (home / 'recipient-map.json').write_text(json.dumps({options['identity']:
                '/identities/Merge/recipient.pem'}))
        elif fault == 'recipient-duplicate':
            (home / 'recipient-map.json').write_text(
                '{"/run/actual/BackboneNeck":"x","/run/actual/BackboneNeck":"y"}')
        elif fault == 'recipient-oversized':
            (home / 'recipient-map.json').write_bytes(b'x' * 4097)
    try:
        with pytest.raises(ValueError, match='WORKER_'):
            worker.start_provider(**options)
        assert worker.launches == [] and worker.leases == {}
    finally:
        worker.close()


def scheduled_inputs(tmp_path, mode='two-node-gpu'):
    inputs = application_inputs(tmp_path, 0, mode)
    for name in ('case.json', 'catalogue-registry.json', 'offer-trust-root.json', 'offer-public-key-map.json', 'recipient-public-keys.json'):
        (inputs['public'] / name).write_text('synthetic fixture')
    (inputs['public'] / 'offer-trust-root.json').write_text(json.dumps({
        'schema': 'spec180-provider-offer-trust-v1', 'candidateId': 'test-candidate',
        'candidateDigest': 'sha256:' + 'a' * 64}))
    (inputs['homes']['user'] / 'request-envelope.key').write_bytes(b'x' * 32)
    (inputs['homes']['user'] / 'requester.key').write_bytes(b'y' * 32)
    (inputs['homes']['user'] / 'authority').mkdir()
    (inputs['homes']['user'] / 'authority/artifact-policy-authority.key').write_text('fixture, not a key')
    package = tmp_path / 'package'
    package.mkdir()
    worker = NodeRuntime(**inputs)
    count = 4 if mode == 'two-node-gpu' else 2
    plan = {'runId': 'run', 'case': mode, 'requests': [
        dict(index=i, warmup=i == 0, requestId='/run/request/' + str(i),
             output=str(worker.output / 'user' / 'requests' / str(i))) for i in range(count)]}
    worker._preparation_binding = (plan, 'receipt-fixture', 'sha256:' + 'c' * 64)
    (inputs['public'] / 'preparation.json').write_text(json.dumps({
        'candidateDigest': 'sha256:' + 'c' * 64,
        'placementCandidateId': 'test-candidate', 'placementCandidateDigest': 'sha256:' + 'a' * 64}))
    worker._verify_prepared_boundary = lambda: None  # Not a qualified SIF/credential fixture.
    worker.verify_runtime(seconds=2)  # Actual tiny fake-runtime version command, no container.
    if mode != 'local-cpu':
        worker.allocation = {'fixture': True}
        worker.gpu_probe = {'fixture': True}
    return worker, plan, dict(package=package, catalog_data_name='/run/catalog/v=1',
        catalog_signer='/run/controller', permission_wait_ms=1000,
        request_deadline_ms=5000, process_timeout_seconds=10, protection_epoch='spec183-test-v1')


@pytest.mark.parametrize('fault', ['bad-id', 'wrong-digest', 'unprepared',
                                  'wrong-valid-id', 'runtime-as-placement',
                                  'wrong-runtime-receipt', 'missing-placement'])
def test_schedule_rejects_unbound_candidate_before_launch(tmp_path, fault):
    from apps.yolo import run_requests
    worker, plan, options = scheduled_inputs(tmp_path, 'local-cpu')
    path = worker.public / 'offer-trust-root.json'
    value = json.loads(path.read_text())
    if fault == 'bad-id':
        value['candidateId'] = ''
    elif fault == 'wrong-digest':
        value['candidateDigest'] = 'sha256:' + 'b' * 64
    elif fault == 'wrong-valid-id':
        value['candidateId'] = 'another-valid-candidate'
    elif fault == 'runtime-as-placement':
        value['candidateDigest'] = worker._preparation_binding[2]
    elif fault in ('wrong-runtime-receipt', 'missing-placement'):
        receipt_path = worker.public / 'preparation.json'
        receipt = json.loads(receipt_path.read_text())
        if fault == 'wrong-runtime-receipt':
            receipt['candidateDigest'] = 'sha256:' + 'd' * 64
        else:
            del receipt['placementCandidateDigest']
        receipt_path.write_text(json.dumps(receipt))
    else:
        worker._preparation_binding = None
    path.write_text(json.dumps(value))
    try:
        with pytest.raises(ValueError, match='CANDIDATE|PREPARATION'):
            run_requests(worker, plan, accept_request=lambda *_: None, **options)
        assert not worker.launches
    finally:
        worker.close()


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
        assert all('SPEC181_PROTECTION_EPOCH=spec183-test-v1' in a for a in calls)
        assert all('SPEC180_CANDIDATE_ID=test-candidate' in a for a in calls)
        assert all('SPEC180_CANDIDATE_DIGEST=sha256:' + 'a' * 64 in a for a in calls)
        assert all('--retain-numerical-response' in a for a in calls)
        assert all('--retain-public-assignments' in a for a in calls)
        assert all(a[a.index('--reference-run-id') + 1] == plan['runId'] for a in calls)
        assert all(a[a.index('--reference-request-id') + 1] == a[a.index('--request-id') + 1] for a in calls)
        assert all(a[a.index('--reference-output') + 1] == a[a.index('--lifecycle-output-dir') + 1] for a in calls)
        assert all(('--nv' in a) == (mode != 'local-cpu') for a in calls)
        assert all('NDNSF_DI_RECIPIENT_PUBLIC_KEY_MAP=/config/recipient-public-keys.json' in a for a in calls)
        assert not any(any(arg.startswith('SPEC181_PROVIDER_RECIPIENT_KEY_MAP=') for arg in a) for a in calls)
        assert all(a[a.index('--generated-policy-dir') + 1].startswith('/output/requests/') for a in calls)
        with pytest.raises(ValueError, match='WORKER_INVOCATION_REUSED'):
            run_requests(worker, plan, accept_request=accept, **options)
        assert len(calls) == count
    finally:
        rows = worker.close()
        assert len([r for r in rows if r.get('kind') == 'finite']) == count
        from runtime.yolo_result import validate_worker_cleanup
        assert validate_worker_cleanup(worker, rows)['childCount'] == count + 1
        assert all(r['reaped'] and not r['forced'] for r in rows)


def test_gpu_reference_requires_preflight_before_user_launch(tmp_path):
    worker, _, options = scheduled_inputs(tmp_path, 'single-node-gpu')
    worker.gpu_probe = None
    try:
        with pytest.raises(ValueError, match='WORKER_REFERENCE_GPU_NOT_QUALIFIED'):
            worker.run_user('0', ['unused'], package=options['package'], seconds=1,
                            reference_gpu=True)
        assert not worker.launches
    finally:
        worker.close()


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


@pytest.mark.parametrize('fault', ['bare-id', 'duplicate-id', 'wrong-output', 'missing-key', 'wrong-count', 'plaintext', 'empty-epoch'])
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
    elif fault in ('plaintext', 'empty-epoch'):
        options['protection_epoch'] = 'plaintext-v1' if fault == 'plaintext' else ''
    else:
        plan['requests'].pop()
    try:
        with pytest.raises(ValueError, match='YOLO_'):
            run_requests(worker, plan, accept_request=lambda *_: None, **options)
        assert worker.launches == []
        assert not (worker.output / 'user').exists()
    finally:
        worker.close()


def test_controller_and_repo_use_real_entrypoints_and_role_writable_outputs(tmp_path, monkeypatch):
    import json
    from apps.yolo import start_controller, start_repo
    worker, _, _ = scheduled_inputs(tmp_path)
    (worker.public / 'case.json').write_text(json.dumps({
        'controller': '/run/controller', 'runtime': {'provider_prefix': '/run'}}))
    (worker.public / 'runtime-publication.json').write_text('{}')
    original, calls = subprocess.Popen, []

    def boundary(argv, **kwargs):
        calls.append(argv)
        return original([sys.executable, '-c', 'import time;time.sleep(60)'], **kwargs)

    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        start_controller(worker)
        start_repo(worker, identity='/run/repo', free_bytes=1000000)
        assert len(calls) == 2
        for argv, script in zip(calls, ('controller.py', 'repo_node.py')):
            assert any(a.endswith('/yolo_2x2/' + script) for a in argv)
            assert argv[argv.index('--config') + 1] == '/config/case.json'
            assert argv[argv.index('--generated-policy-dir') + 1] == '/output/generated-policy'
            assert '--nv' not in argv
            assert not any(':/artifacts:' in a for a in argv)
        assert calls[0][calls[0].index('--spec180-runtime-publication-file') + 1] == '/config/runtime-publication.json'
        assert calls[0][calls[0].index('--spec180-runtime-receipt-file') + 1] == '/output/runtime-publication-receipt.json'
        assert calls[1][calls[1].index('--provider-id') + 1] == 'repo'
        assert calls[1][calls[1].index('--storage-dir') + 1] == '/output/repo-store'
        assert calls[1][calls[1].index('--free-bytes') + 1] == '1000000'
    finally:
        assert all(r['reaped'] for r in worker.close())


@pytest.mark.parametrize('fault', ['missing-publication', 'duplicate-json', 'repo-identity', 'capacity'])
def test_control_plane_bad_input_fails_before_launch(tmp_path, fault):
    import json
    from apps.yolo import start_controller, start_repo
    worker, _, _ = scheduled_inputs(tmp_path)
    (worker.public / 'case.json').write_text(json.dumps({
        'controller': '/run/controller', 'runtime': {'provider_prefix': '/run'}}))
    try:
        with pytest.raises(ValueError):
            if fault in ('missing-publication', 'duplicate-json'):
                if fault == 'duplicate-json':
                    (worker.public / 'runtime-publication.json').write_text('{"a":1,"a":2}')
                start_controller(worker)
            else:
                start_repo(worker, identity='/run/other' if fault == 'repo-identity' else '/run/repo',
                           free_bytes=True if fault == 'capacity' else 1000)
        assert worker.launches == [] and worker.leases == {}
    finally:
        worker.close()


class _LifecycleBarrier:
    def __init__(self, directory, binding):
        self.directory = directory
        self.binding = binding
        self.rank = 0
        self.ranks = (0,)
        self.events = []
        self.check = lambda: None

    def remaining(self):
        self.events.append(('remaining',))
        return 5.0

    def publish(self, stage, payload):
        self.events.append(('publish', stage, payload))

    def wait(self, stage, ranks=None):
        self.events.append(('wait', stage, ranks))
        if stage == 'workload-complete':
            return {0: {'requestCount': 2}}
        return {0: {}}


def test_run_normal_node_owns_ordered_lifecycle_and_request_callback(tmp_path, monkeypatch):
    from apps import yolo

    plan = {
        'runId': 'operator-test-01', 'case': 'local-cpu',
        'requests': [
            {'index': 0, 'warmup': True, 'requestId': '/run/request/0'},
            {'index': 1, 'warmup': False, 'requestId': '/run/request/1'},
        ],
    }

    class Worker:
        mode, rank, cleanup_seconds = 'local-cpu', 0, 0.0

        def __init__(self):
            self._preparation_binding = (plan, 'sha256:' + '1' * 64, 'sha256:' + '2' * 64)
            self.output = tmp_path / 'node0'
            self.output.mkdir()
            self.events = []

        def verify_runtime(self, **kwargs):
            self.events.append(('version',))

        def close(self):
            self.events.append(('close',))
            return []

    worker = Worker()
    binding = {'runId': plan['runId'], 'probeId': 'a' * 32,
               'candidateDigest': 'sha256:' + '2' * 64}
    startup = _LifecycleBarrier(tmp_path / 'startup', binding)
    completion = _LifecycleBarrier(tmp_path / 'completion', binding)
    events, accepted = [], []

    monkeypatch.setattr(yolo, 'configure_network',
                        lambda *_args, **_kwargs: events.append('network'))
    monkeypatch.setattr(yolo, 'start_workload',
                        lambda *_args, **_kwargs: events.append('workload'))

    def requests(worker_arg, plan_arg, *, accept_request, **_kwargs):
        assert worker_arg is worker and plan_arg is plan
        events.append('requests')
        for request in plan_arg['requests']:
            accept_request(request, tmp_path / ('request-' + str(request['index'])))

    monkeypatch.setattr(yolo, 'run_requests', requests)
    monkeypatch.setattr('runtime.yolo_result.write_worker_receipt',
                        lambda worker_arg, rows: {'status': 'NODE_COMPONENT'})

    result = yolo.run_normal_node(
        worker, startup, completion_factory=lambda: completion,
        endpoints=[{'rank': 0, 'address': '127.0.0.1', 'port': 16380}],
        startup_options={'repo_free_bytes': 4096, 'permission_wait_ms': 100,
                         'network_probe_seconds': 1.0},
        request_options={'package': tmp_path, 'catalog_data_name': '/catalogue/v1',
                         'catalog_signer': '/controller', 'permission_wait_ms': 100,
                         'request_deadline_ms': 2000, 'process_timeout_seconds': 2.0,
                         'protection_epoch': 'epoch-1'},
        accept_request=lambda request, output: accepted.append((request['index'], output)),
    )

    assert result == {'status': 'NODE_COMPONENT'}
    assert events == ['network', 'workload', 'requests']
    assert [index for index, _ in accepted] == [0, 1]
    assert ('publish', 'workload-complete', {'requestCount': 2}) in completion.events
    assert ('wait', 'workload-complete', None) in completion.events
    assert worker.events == [('version',), ('close',)]


def test_run_normal_node_records_startup_failure_and_closes_worker(tmp_path, monkeypatch):
    from apps import yolo

    plan = {'runId': 'operator-test-01', 'case': 'local-cpu', 'requests': []}

    class Worker:
        mode, rank, cleanup_seconds = 'local-cpu', 0, 0.0

        def __init__(self):
            self._preparation_binding = (plan, 'sha256:' + '1' * 64, 'sha256:' + '2' * 64)
            self.output = tmp_path / 'node0'
            self.output.mkdir()
            self.closed = False

        def verify_runtime(self, **kwargs):
            pass

        def close(self):
            self.closed = True
            return []

    worker = Worker()
    startup = _LifecycleBarrier(tmp_path / 'startup', {
        'runId': plan['runId'], 'probeId': 'a' * 32,
        'candidateDigest': 'sha256:' + '2' * 64})
    monkeypatch.setattr(yolo, 'configure_network',
                        lambda *_args, **_kwargs: (_ for _ in ()).throw(RuntimeError('network')))
    monkeypatch.setattr('runtime.yolo_result.write_worker_receipt',
                        lambda *_args: pytest.fail('receipt must not be written'))

    with pytest.raises(RuntimeError, match='network'):
        yolo.run_normal_node(worker, startup, completion_factory=lambda: None,
                             endpoints=[], startup_options={}, request_options={},
                             accept_request=lambda *_: None)
    assert worker.closed
    assert [event[1] for event in startup.events if event[0] == 'publish'] == ['failed', 'failed']
