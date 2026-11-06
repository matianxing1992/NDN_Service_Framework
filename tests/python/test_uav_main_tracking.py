"""Main launcher contract checks; native C++ tests own execution/admission semantics."""
import copy
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Experiments/UAV'))
import run_multicamera_tracking_demo as runner
from person_run_receipt import build_main_run_receipt, validate_main_telemetry, cross_camera_matches, validate_main_motion_csv


def main_plan(tmp_path, monkeypatch, name="run-1", epoch=42, prepared=False):
    topology = tmp_path / 'topology.conf'
    topology.write_bytes((ROOT / 'Experiments/UAV/topology.conf').read_bytes())
    monkeypatch.setattr(runner, 'TOPOLOGY', topology)
    binaries = tmp_path / 'installed'
    binaries.mkdir(exist_ok=True)
    for binary_name in ('UavDroneApp', 'UavGroundStationApp', 'App_ServiceController', 'tracking_worker.py'):
        (binaries / binary_name).write_text('installed fixture ' + binary_name)
    monkeypatch.setattr(runner, '_installed_program', lambda name: str(binaries / name))
    monkeypatch.setattr(runner, '_installed_tracking_script', lambda name: binaries / name)
    model, python, trust = [tmp_path / name for name in ('model.pt', 'python3', 'trust.conf')]
    for path in (model, python, trust):
        path.write_text(path.name)
    runner._write_uav_trust_schema(trust)
    output = tmp_path / name
    specs = runner._main_process_specs(ROOT, output, model=model, headless=True, window_count=2,
        request_timeout_ms=180000, source_ids={f'UAV{i}': f'source-{i}' for i in range(1, 4)},
        source_epoch=epoch, observation_profile={'deliveryMode': 'replay', 'worldFrameId': 'simulated',
        'acquisitionKind': 'simulated'}, source_generations_path=output / 'private/config/generations.json',
        trust_schema=trust, person_tracking=None, worker_python=python)
    plan = {**runner._main_plan_metadata(specs, output), 'processes': [s.public_dict() for s in specs]}
    plan.update(mode='headless', topology={'nodes': []}, settings={
        'nfdCsMaxPackets': 4096,
        'globalDeadlineMs': 180000, 'trackingHostRequestTimeoutMs': 180000,
        'windowCount': 2, 'fault': 'none'}, security={
        'trustSchema': str(trust), 'trustSchemaDigest': runner.digest_file(trust),
        'credentialsPrepared': False})
    if prepared:
        for reference in plan['trackingMain']['providerConfigs'].values():
            catalog = json.loads(Path(reference['path']).read_bytes())
            for run in catalog['runs']:
                if run['role'] == 'source':
                    directory = Path(run['observationStreamDirectory'])
                    directory.mkdir(parents=True, exist_ok=True)
                    for index in range(run['windowCount']):
                        (directory / f'window-{index}.jsonl').write_text('{}\n')
        runner._freeze_main_sources(plan)
    return output, plan


def test_actual_main_actor_catalogs_have_one_explicit_identity_map(tmp_path, monkeypatch):
    _, plan = main_plan(tmp_path, monkeypatch)
    runner._verify_main_plan(plan)
    assert len(plan['processes']) == 6
    assert {Path(s['command'][0]).name for s in plan['processes']} == {
        'UavDroneApp', 'UavGroundStationApp', 'App_ServiceController'}
    catalogs = plan['trackingMain']['providerConfigs']
    for name, ref in catalogs.items():
        value = json.loads(Path(ref['path']).read_bytes())
        assert value['producerIdentity'] == runner.TRACKING_ACTORS[name][1]
        run = value['runs'][0]
        assert run['requestTimeoutMs'] == 180000
        assert run['worldFrameId'] == 'simulated'
        assert run['acquisitionKind'] == 'simulated'
        assert run['deliveryMode'] == 'replay'
        assert run['role'] == ('compute' if name == 'compute' else 'source')
        assert (run['model'] is not None) == (name == 'compute')
    compute = json.loads(Path(catalogs['compute']['path']).read_bytes())
    assert compute['producerIdentity'] == '/example/uav/compute'
    assert compute['droneId'] == 'compute'


@pytest.mark.parametrize('mutation', ['binary', 'identity', 'node', 'duplicate'])
def test_main_plan_rejects_actor_misbinding(tmp_path, monkeypatch, mutation):
    _, plan = main_plan(tmp_path, monkeypatch)
    if mutation == 'binary':
        plan['processes'][1]['command'][0] = '/usr/local/bin/UavTrackingGroundStation'
    elif mutation == 'identity':
        plan['processes'][-1]['identity'] = '/example/uav/drone/compute'
    elif mutation == 'node':
        plan['processes'][-1]['node'] = 'uav1'
    else:
        plan['processes'].append(copy.deepcopy(plan['processes'][0]))
    with pytest.raises(runner.RunnerError, match='actor|six'):
        runner._verify_main_plan(plan)


def test_main_freeze_rejects_asset_and_config_changes(tmp_path, monkeypatch):
    _, plan = main_plan(tmp_path, monkeypatch)
    path = Path(plan['trackingMain']['providerConfigs']['compute']['path'])
    path.write_text(path.read_text() + '\n')
    with pytest.raises(runner.RunnerError, match='changed'):
        runner._verify_main_plan(plan)


def test_main_sources_must_be_prepared_before_execution(tmp_path, monkeypatch):
    _, plan = main_plan(tmp_path, monkeypatch)
    with pytest.raises(runner.RunnerError, match='source files'):
        runner._verify_main_plan(plan, prepared=True)


def test_main_interpreter_requires_its_actual_regular_venv_path(tmp_path):
    real = tmp_path / 'python3'
    real.write_text('regular venv interpreter')
    link = tmp_path / 'python'
    link.symlink_to(real)
    with pytest.raises(runner.RunnerError):
        runner._main_asset(link, interpreter=True)
    assert runner._main_asset(real, interpreter=True)['path'] == str(real)


@pytest.mark.parametrize('kwargs', [{'fault': 'cancel'}, {'legacy_mode': True},
                                  {'source_switch_after_window': (1,)},
                                  {'observation_mode': 'live-push'}])
def test_main_rejects_compatibility_only_paths_before_asset_access(tmp_path, kwargs):
    with pytest.raises(runner.RunnerError, match='fixed prepared replay'):
        runner.build_plan(source=tmp_path/'missing', model=tmp_path/'missing.pt',
            output=tmp_path/'run', license_text='fixture', application_mode='main', **kwargs)


def test_main_draw_requires_all_three_exact_result_bindings():
    lines = [f'SPEC228_TRACKING_FRAME_SHOWN run=run window=2 camera=UAV{i} result=sha256:abc'
             for i in range(1, 4)]
    assert runner._main_frame_shown('\n'.join(lines), 'run', 2, 'sha256:abc')
    assert not runner._main_frame_shown('\n'.join(lines[:2]), 'run', 2, 'sha256:abc')
    assert not runner._main_frame_shown('\n'.join(lines), 'run', 2, 'sha256:other')


@pytest.mark.parametrize('actor,flag,value', [
    ('ground-station', '--ground-station-identity', '/wrong/gs'),
    ('controller', '--controller-prefix', '/wrong/controller'),
    ('ground-station', '--tracking-job-config', '/wrong/job.json'),
    ('controller', '--trust-schema', '/wrong/trust.conf')])
def test_main_preflight_checks_actual_commands(tmp_path, monkeypatch, actor, flag, value):
    _, plan = main_plan(tmp_path, monkeypatch)
    command = next(item['command'] for item in plan['processes'] if item['name'] == actor)
    plan['security'] = {'trustSchema': command[command.index('--trust-schema') + 1]}
    command[command.index(flag) + 1] = value
    with pytest.raises(runner.RunnerError, match='command'):
        runner._verify_main_plan(plan)


def test_early_main_startup_failure_keeps_first_boundary_and_writes_receipt(tmp_path, monkeypatch):
    output, plan = main_plan(tmp_path, monkeypatch)
    failure = {'terminal': False, 'outcome': 'launcher-failed', 'error': 'source not ready'}
    (output / 'lifecycle.json').write_text(json.dumps(failure))
    assert runner._collect_main_run(plan, output, 1) == 1
    receipt = json.loads((output / 'main-run-receipt.json').read_bytes())
    assert receipt['complete'] is False
    assert receipt['lifecycle'] == failure
    assert receipt['errors']


def test_main_receipt_reports_resource_truncation_and_absent_status(tmp_path, monkeypatch):
    output, plan = main_plan(tmp_path, monkeypatch)
    plan['topology'] = {'nodes': []}
    for name, value in {
        'lifecycle.json': {'terminal': False, 'outcome': 'launcher-failed'},
        'main-cleanup.json': {'remainingOwnedProcesses': 0, 'unreapedOwnedProcesses': 0,
                             'applicationExitCodes': {'ground-station': 1}},
        'main-resources.json': {'artifactTruncated': True, 'samples': [], 'errors': ['bound exceeded']},
    }.items():
        (output / name).write_text(json.dumps(value))
    receipt = build_main_run_receipt(output, plan, closure_verified=True)
    assert receipt['complete'] is False
    assert receipt['resourceMeasurementsComplete'] is False
    assert receipt['resourceErrors']
    assert any('main-observations' in error for error in receipt['errors'])
    assert any('tracking-status' in error for error in receipt['errors'])
    assert receipt['jobs'][0]['verifiedWindows'] == 0


@pytest.mark.parametrize('content', [None, '{invalid'])
def test_failure_receipt_survives_missing_or_invalid_lifecycle(tmp_path, monkeypatch, content):
    output, plan = main_plan(tmp_path, monkeypatch)
    if content is not None:
        (output / 'lifecycle.json').write_text(content)
    assert runner._collect_main_run(plan, output, 1) == 1
    value = json.loads((output / 'main-run-receipt.json').read_bytes())
    assert value['complete'] is False
    assert value['lifecycle']['outcome'] == 'missing-or-invalid-lifecycle'


def test_three_jobs_share_one_main_actor_set_without_mutating_inputs(tmp_path, monkeypatch):
    children = [main_plan(tmp_path, monkeypatch, f'run-{i}', 42+i, True)[1] for i in range(3)]
    before = copy.deepcopy(children)
    campaign = runner.combine_main_plans(children, output=tmp_path/'campaign', global_deadline_ms=540000)
    assert children == before
    assert len(campaign['processes']) == 6
    assert len(campaign['trackingMain']['jobs']) == 3
    assert campaign['settings']['windowCount'] == 6
    command = next(s['command'] for s in campaign['processes'] if s['name'] == 'ground-station')
    assert command.count('--tracking-job-config') == 3
    for reference in campaign['trackingMain']['providerConfigs'].values():
        catalog = json.loads(Path(reference['path']).read_bytes())
        assert len(catalog['runs']) == 3
        assert all(run['requestTimeoutMs'] == 180000 for run in catalog['runs'])
    runner._verify_main_plan(campaign, prepared=True)
    runner._mark_main_network_attempt(campaign, tmp_path/'campaign')
    assert all((Path(p)/'main-network-attempt.json').is_file()
               for p in campaign['trackingMain']['childRunRoots'])
    with pytest.raises(runner.RunnerError, match='already exists'):
        runner._mark_main_network_attempt(campaign, tmp_path/'campaign')


@pytest.mark.parametrize('fault', ['epoch', 'attempted', 'topology', 'source-drift', 'budget'])
def test_sequence_rejects_reuse_misbinding_and_frozen_input_changes(tmp_path, monkeypatch, fault):
    _, left = main_plan(tmp_path, monkeypatch, 'left', 42, True)
    right_root, right = main_plan(tmp_path, monkeypatch, 'right', 42 if fault == 'epoch' else 43, True)
    if fault == 'attempted':
        (right_root/'main-network-attempt.json').write_text('{}')
    elif fault == 'topology':
        right['topology'] = {'nodes': ['wrong']}
    elif fault == 'source-drift':
        Path(right['trackingMain']['sourceFiles'][0]['path']).write_text('changed')
    with pytest.raises(runner.RunnerError):
        runner.combine_main_plans([left, right], output=tmp_path/'campaign',
                                  global_deadline_ms=100000 if fault == 'budget' else 540000)


def test_actor_rejects_an_alternate_binary_with_same_basename(tmp_path, monkeypatch):
    _, plan = main_plan(tmp_path, monkeypatch)
    plan['processes'][0]['command'][0] = '/alternate/App_ServiceController'
    with pytest.raises(runner.RunnerError, match='actor executable'):
        runner._verify_main_plan(plan)


def telemetry_document(count=3):
    job = {'runId': 'run', 'windowCount': count, 'sources': [
        {'producerIdentity': runner.TRACKING_ACTORS[name][1],
         'droneId': runner.TRACKING_ACTORS[name][2]}
        for name in ['uav1-camera', 'uav2-camera', 'uav3-camera']],
        'compute': {'producerIdentity': '/example/uav/compute', 'droneId': 'compute'}}
    phases = [0, 1, 2] if count > 1 else [0, 2]
    document = {'schema': 'uav-main-tracking-telemetry-v1', 'runId': 'run',
        'clockDomain': 'CLOCK_MONOTONIC_MS', 'complete': True, 'failed': False,
        'phaseStates': [2, 2, 2] if count > 1 else [2, 0, 2], 'reports': []}
    for phase in phases:
        for name in ['uav1-camera', 'uav2-camera', 'uav3-camera', 'compute']:
            _, producer, drone = runner.TRACKING_ACTORS[name]
            start = 10000 + phase*3000
            document['reports'].append({'phase': phase, 'runId': 'run', 'producerIdentity': producer,
                'droneId': drone, 'verified': True, 'reason': '',
                'nativeStateAtRequest': 'COMPLETED' if phase == 2 else 'RUNNING',
                'verifiedWindowsAtRequest': count if phase == 2 else phase,
                'startedMonotonicMs': start, 'completedMonotonicMs': start+10, 'elapsedMs': 10})
    return job, document


@pytest.mark.parametrize('count', [1, 3])
def test_main_telemetry_audit_accepts_exact_native_phases(count):
    job, document = telemetry_document(count)
    assert validate_main_telemetry(document, job)['complete']


@pytest.mark.parametrize('fault', ['run', 'producer', 'duplicate', 'late', 'elapsed',
                                   'clock', 'verified-type', 'truncated', 'missed'])
def test_main_telemetry_audit_rejects_invalid_qualification(fault):
    job, document = telemetry_document()
    if fault == 'run': document['runId'] = 'other'
    elif fault == 'producer': document['reports'][0]['producerIdentity'] = '/wrong'
    elif fault == 'duplicate': document['reports'][1] = copy.deepcopy(document['reports'][0])
    elif fault == 'late': document['reports'][4]['nativeStateAtRequest'] = 'COMPLETED'
    elif fault == 'elapsed':
        document['reports'][0].update(completedMonotonicMs=12001, elapsedMs=2001)
    elif fault == 'clock': document['clockDomain'] = 'SYSTEM'
    elif fault == 'verified-type': document['reports'][0]['verified'] = 1
    elif fault == 'truncated': document['reports'].pop()
    else: document['phaseStates'][1] = 3
    assert not validate_main_telemetry(document, job)['complete']


@pytest.mark.parametrize('document', [None, [], False, 'not an object'])
def test_nonobject_native_telemetry_keeps_a_failed_audit(document):
    job, _ = telemetry_document()
    assert not validate_main_telemetry(document, job)['complete']


def test_main_freeze_binds_the_actual_minindn_topology_file(tmp_path, monkeypatch):
    _, plan = main_plan(tmp_path, monkeypatch)
    runner.TOPOLOGY.write_text('changed topology')
    with pytest.raises(runner.RunnerError, match='changed'):
        runner._verify_main_plan(plan)



def test_cross_camera_witness_requires_a_native_match_with_different_origin():
    event = {'status': 'MATCHED', 'cameraId': 'UAV2', 'globalId': 7,
             'origin': {'localKey': {'cameraId': 'UAV1'}}}
    assert cross_camera_matches({'associationEvents': [event]}) == [event]
    for mutation in [dict(event, status='CONTINUED'), dict(event, cameraId='UAV1'),
                     dict(event, origin=None)]:
        assert not cross_camera_matches({'associationEvents': [mutation]})


def native_csv_fixture():
    import csv
    import io
    fields = ('schema sourceId cameraId sourceEpoch sequence localTrackId trackGeneration '
        'globalId observationName observationDigest captureTimeUs estimateTimeUs rangeTimeUs '
        'velocityEastMps velocityNorthMps speedMps courseDeg horizontalRangeM slantRangeM '
        'sigmaSpeedMps sigmaCourseDeg sigmaRangeM speedStatus bearingStatus rangeStatus quality '
        'reasonCodes acquisitionKind deliveryMode geometryValidity accuracyQualification '
        'uncertaintyKind anchorKind calibrationRevision anchorApproximation').split()
    track = {'sourceId': 'source', 'cameraId': 'UAV1', 'sourceEpoch': 5,
             'localTrackId': 2, 'trackGeneration': 1}
    measurement = {'trackKey': track, 'frameKey': {'sequence': 3},
        'observationRef': {'name': '/producer/observation', 'digest': 'sha256:abc'},
        'globalId': 7, 'speedMps': 2.125, 'speedStatus': 'valid',
        'courseDeg': None, 'bearingStatus': 'unknown',
        'reasonCodes': ['unavailable', 'quoted,reason'], 'anchorApproximation': True}
    row = dict(measurement, **track, sequence=3, observationName='/producer/observation',
               observationDigest='sha256:abc')
    stream = io.StringIO()
    writer = csv.writer(stream)
    writer.writerow(fields)
    writer.writerow(['' if row.get(key) is None else row[key] if isinstance(row[key], str)
                     else json.dumps(row[key]) for key in fields])
    return {'motion': {'measurements': [measurement]}}, stream.getvalue().encode()


@pytest.mark.parametrize('mutation', [None, 'speed', 'id', 'null', 'rows', 'header', 'malformed'])
def test_native_csv_agreement_checks_values_keys_and_unknowns(mutation):
    result, raw = native_csv_fixture()
    if mutation == 'speed': raw = raw.replace(b'2.125', b'3.125')
    elif mutation == 'id': result['motion']['measurements'][0]['trackKey']['sourceEpoch'] = 6
    elif mutation == 'null': result['motion']['measurements'][0]['courseDeg'] = 0
    elif mutation == 'rows': result['motion']['measurements'].clear()
    elif mutation == 'header': raw = raw.replace(b'speedMps', b'wrongUnit')
    elif mutation == 'malformed': raw += b'"unterminated'
    if mutation is None:
        validate_main_motion_csv(result, raw)
    else:
        with pytest.raises(ValueError):
            validate_main_motion_csv(result, raw)


@pytest.mark.parametrize("value", [None, True, 0, 4096.0, 65536])
def test_prepared_network_rejects_unfrozen_cache_profiles(tmp_path, monkeypatch, value):
    _, plan = main_plan(tmp_path, monkeypatch)
    plan["settings"]["nfdCsMaxPackets"] = value
    with pytest.raises(runner.RunnerError, match="NFD cache size"):
        runner._nfd_cache_packets(plan)


def test_prepared_network_has_one_bounded_native_cache_profile(tmp_path, monkeypatch):
    _, plan = main_plan(tmp_path, monkeypatch)
    assert runner._nfd_cache_packets(plan) == 4096
    del plan["settings"]["nfdCsMaxPackets"]
    with pytest.raises(runner.RunnerError, match="NFD cache size"):
        runner._nfd_cache_packets(plan)
