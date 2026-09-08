"""Local owner composition; explicit provision/rank doubles, not SIF PASS."""
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / 'tests/python'))
from test_spec180_yolo_numerical import reference
from test_spec183_yolo_host_gate import write_receipt
from test_yolo_submit import file_ref, submit_module


@pytest.fixture(scope='module')
def frozen_bundle(tmp_path_factory):
    from tools.spec183_dispatch_plane import _sealed_harness
    root = tmp_path_factory.mktemp('local-frozen')
    receipt = _sealed_harness(root)
    return root / 'harness', receipt['manifestSha256']


@pytest.mark.parametrize('fault', ['none', 'request', 'missing-request'])
@pytest.mark.parametrize('mode', ['local-cpu', 'single-node-gpu'])
def test_local_owner_stops_on_failed_request_and_publishes_only_after_cleanup(
        tmp_path, monkeypatch, reference, frozen_bundle, fault, mode):
    from runtime import yolo_operator as operator, yolo_result, yolo_graph_reference
    from runtime.yolo_worker import assigned_roles
    package, _ = reference
    root = tmp_path / 'run'
    root.mkdir()
    bundle, harness_digest = frozen_bundle
    roles = assigned_roles('local-cpu', 0)
    plan = dict(schema='tiger-yolo-run-plan-v1', runId='local-test', case=mode,
        applicationName='/local-test', output=str(root), nodes=[dict(rank=0, roles=list(roles))],
        identities={role: '/local-test/' + role for role in roles},
        requests=[dict(index=i, warmup=i == 0, requestId='/local-test/' + str(i),
                       output=str(root / 'node0/user/requests' / str(i))) for i in range(2)])
    candidate = 'sha256:'+'a'*64
    prepared = dict(plan=plan, case=mode, runId=plan['runId'], candidateDigest=candidate,
                    bundle=str(bundle), harnessManifestSha256=harness_digest)
    profile = dict(timing=dict(stagingSeconds=5, startupSeconds=5, progressTimeoutSeconds=1,
        requestDeadlineMs=2000, cleanupSeconds=1), cluster=dict(wallTimeSeconds=30, tcpPort=16363),
        storage=dict(peakBytes=10000, marginBytes=100), security=dict(protectionEpoch='epoch-1'),
        oracle=dict(input=file_ref(REPOSITORY / 'tests/fixtures/spec180/yolo26n/fixed-fixture.ppm'),
                    reference=file_ref(package / 'oracle/full-model-output.npy')))
    resolved = dict(package=str(package), runtimeProfile={'sif': '/declared-fixture-only.sif'},
        descriptor=dict(plan=plan, runtimeCandidateDigest=candidate,
                        manifestDigest=file_ref(package / 'manifest.json')['sha256']))
    events = []
    def stage(value, path):
        assert value is resolved
        events.append('stage')
        path.mkdir()
        return 'sha256:'+'b'*64
    def provision(**kwargs):
        events.append('provision')
        assert kwargs['runtime_profile'] == resolved['runtimeProfile']
        assert kwargs['bundle'] == bundle and kwargs['package'] == package
        return dict(receiptDigest='sha256:'+'c'*64, preparation=dict(
            graphDigest='sha256:'+'d'*64, catalogueDigest='sha256:'+'e'*64,
            placementCandidateId='yolo', placementCandidateDigest='sha256:'+'f'*64,
            catalogueDataName='/local-test/catalogue', catalogueSigner='/local-test/controller'))
    def numerical(path, frozen, **kwargs):
        events.append('request-' + path.name)
        assert frozen.manifest_digest == resolved['descriptor']['manifestDigest']
        if fault == 'request':
            raise ValueError('DECLARED_NUMERICAL_FAILURE')
    def graph(path, **kwargs):
        assert path.name == 'graph-reference.json'
        assert kwargs['runtime_candidate_digest'] == candidate
    def rank(**kwargs):
        events.append('rank')
        assert kwargs['mode'] == mode and kwargs['rank'] == 0
        if mode == 'single-node-gpu':
            assert kwargs['gpu_device'] == '0' and kwargs['allocation_expected'] == expected
        assert kwargs['completion_seconds'] == 6
        try:
            if fault != 'missing-request':
                for request in plan['requests']:
                    kwargs['accept_request'](request, Path(request['output']))
        finally:
            events.append('cleanup')
        return {'declared': 'rank fixture'}
    def finish(**kwargs):
        assert events[-1] == 'cleanup'
        assert kwargs['rank_results'] == {0: {'declared': 'rank fixture'}}
        assert kwargs['certified_graph'] == {'graphDigest': 'sha256:'+'d'*64}
        assert len(kwargs['references']) == 2
        events.append('handoff')
        return {'status': 'COLLECTION_READY_FIXTURE'}
    monkeypatch.setattr(operator, 'stage_provision_inputs', stage)
    monkeypatch.setattr(operator, 'provision_run', provision)
    monkeypatch.setattr(operator, 'run_rank', rank)
    monkeypatch.setattr(operator, 'finalize_normal_collection', finish)
    monkeypatch.setattr(yolo_result, 'collect_request_result', numerical)
    monkeypatch.setattr(yolo_graph_reference, 'read_request_reference', graph)
    expected = dict(job_id='123', submission_key='spec183-'+'a'*64,
                    partition='bigTiger', gpu_type='rtx_6000')
    from runtime import yolo_allocation
    def capture(**kwargs):
        assert kwargs == dict(expected, rank=0, node_count=1, seconds=1)
        return {'receipt': {'visible': '0'}}
    monkeypatch.setattr(yolo_allocation, 'capture_task_allocation', capture)
    def execute():
        if mode == 'local-cpu':
            return operator.execute_local_run(prepared=prepared, profile=profile, resolved=resolved)
        return operator.execute_single_gpu_run(prepared=prepared, profile=profile, resolved=resolved,
                                               allocation_expected=expected)
    if fault == 'none':
        assert execute() == {
            'status': 'COLLECTION_READY_FIXTURE'}
        assert events == ['stage', 'provision', 'rank', 'request-0', 'request-1', 'cleanup', 'handoff']
    else:
        with pytest.raises(ValueError):
            execute()
        assert 'handoff' not in events and events[-1] == 'cleanup'
        assert json.loads((root / 'local-execution-failure.json').read_text())['status'] == 'FAIL'
    old_events = list(events)
    with pytest.raises(ValueError, match='LOCAL_RUN_ALREADY_STARTED'):
        execute()
    assert events == old_events


@pytest.mark.parametrize('fault', ['none', 'source', 'missing-extension', 'generic-pass'])
def test_host_receipt_consumption_binds_existing_owner_to_native_source(tmp_path, fault):
    module = submit_module()
    source, path, value = write_receipt(tmp_path)
    native = dict(schemaVersion='spec170-container-native-build-v1', status='PASS',
        buildBoundary='container-runtime-in-sif', sourceSealSha256=file_ref(source)['sha256'],
        artifacts=[dict(name=name, sha256='sha256:'+'a'*64, finalSha256='sha256:'+'a'*64)
                   for name in ['provider', 'faultProvider', 'controller', 'framework',
                                'ndn-svs', 'nac-abe', 'ndn-sd', 'extension', 'repoExtension']])
    if fault == 'source':
        native['sourceSealSha256'] = 'sha256:'+'b'*64
    elif fault == 'missing-extension':
        native['artifacts'].pop()
    elif fault == 'generic-pass':
        value = {'status': 'PASS', 'qualification': 'READY'}
    path.write_text(json.dumps(value))
    native_path = tmp_path / 'native.json'
    native_path.write_text(json.dumps(native))
    runtime = tmp_path / 'runtime.json'
    runtime.write_text(json.dumps({'files': {'nativeManifest': file_ref(native_path)}}))
    profile = {'release': {'gates': {'hostMinindn': file_ref(path)}, 'runtime': file_ref(runtime)}}
    if fault == 'none':
        result = module._gate_receipt(tmp_path / 'profile.json', profile, 'hostMinindn')
        assert result['receipt']['qualification'] == 'YOLO_HOST_GATE_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError, match='GATE_HOST_SOURCE_BINDING'):
            module._gate_receipt(tmp_path / 'profile.json', profile, 'hostMinindn')


@pytest.mark.parametrize('action', ['local', 'collect'])
def test_operator_enters_frozen_cli_and_preserves_its_exit_code(tmp_path, monkeypatch, frozen_bundle, action):
    import subprocess
    module = submit_module()
    bundle, digest = frozen_bundle
    prepared = dict(bundle=str(bundle), harnessManifestSha256=digest)
    args = SimpleNamespace(profile=tmp_path / 'profile.json', output=tmp_path / 'results',
                           run_id='local-test', case='local-cpu')
    seen = []
    def boundary(argv, **kwargs):
        assert Path(argv[2]) == bundle / 'jobs/yolo/submit.py'
        assert argv[3] == action and kwargs['cwd'] == bundle
        assert ('--case' in argv) == (action == 'local')
        seen.append(argv)
        return SimpleNamespace(returncode=17)
    monkeypatch.setattr(subprocess, 'run', boundary)
    assert module._enter_frozen(args, prepared, action) == 17
    assert len(seen) == 1 and not args.output.exists()
    monkeypatch.setattr(module, 'BUNDLE', bundle)
    assert module._enter_frozen(args, prepared, action) is None
    with pytest.raises(ValueError, match='HARNESS_MANIFEST_DIGEST'):
        module._enter_frozen(args, dict(prepared, harnessManifestSha256='sha256:'+'0'*64), action)
    assert len(seen) == 1


def test_local_missing_host_gate_never_enters_runner(tmp_path, monkeypatch):
    module = submit_module()
    profile_digest = 'sha256:'+'a'*64
    monkeypatch.setattr(module, '_dispatch_report', lambda _: (
        dict(integrity='VERIFIED', qualification='NOT_EVALUATED', documentDigest=profile_digest),
        {'release': {'gates': {}}}))
    monkeypatch.setattr(module, '_load_prepared', lambda *args: dict(case='local-cpu', profileDigest=profile_digest))
    monkeypatch.setattr(module, '_execute_local', lambda *args: pytest.fail('unqualified local execution'))
    with pytest.raises(ValueError, match='GATE_MISSING:hostMinindn'):
        module._local(SimpleNamespace(profile=tmp_path / 'profile.json', output=tmp_path / 'out',
                                    run_id='local-test', case='local-cpu'))
    assert not (tmp_path / 'out').exists()


def test_dispatch_rejects_profile_changed_after_content_check(tmp_path, monkeypatch):
    from runtime import yolo_profile
    module = submit_module()
    monkeypatch.setattr(module, 'check_operator_profile', lambda *args, **kwargs: dict(documentDigest='sha256:'+'a'*64))
    monkeypatch.setattr(yolo_profile, 'load_operator_profile', lambda *args, **kwargs: dict(documentDigest='sha256:'+'b'*64, profile={}))
    with pytest.raises(ValueError, match='PROFILE_CHANGED_DURING_DISPATCH'):
        module._dispatch_report(tmp_path / 'profile.json')
