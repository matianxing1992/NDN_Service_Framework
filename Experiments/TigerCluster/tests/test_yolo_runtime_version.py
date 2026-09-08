"""Version launch/retained boundaries with real tiny OS children, not SIF gates."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_worker import verify_runtime_version, read_runtime_version, NodeRuntime


def retained_version(output, *, run_id, candidate, rank, version='1.5.3'):
    """Synthetic retained fixture for collector tests; never actual qualification."""
    directory = output / 'runtime-version'
    directory.mkdir(parents=True)
    payload = ('apptainer version ' + version + '\n').encode()
    (directory / 'version.log').write_bytes(payload)
    (directory / 'version.log').chmod(0o600)
    record = dict(schema='tiger-yolo-runtime-version-v1', status='MATCH',
        binding=dict(runId=run_id, candidateDigest=candidate, rank=rank), expected=version,
        argv=['/fixture/apptainer', '--version'], cleanup=[dict(name='apptainer-version',
            pid=12345, kind='finite', exitCode=0, reaped=True, forced=False)],
        log=dict(path='version.log', bytes=len(payload), sha256='sha256:'+hashlib.sha256(payload).hexdigest()))
    (directory / 'receipt.json').write_text(json.dumps(record))
    (directory / 'receipt.json').chmod(0o600)
    return read_runtime_version(output, expected_version=version, binding=record['binding'])


def launcher(tmp_path, behavior='match'):
    path = tmp_path / 'version-command'
    path.write_text('#!/usr/bin/python3\nimport sys,time,subprocess\n'
        "assert sys.argv[1:] == ['--version']\n"
        f"behavior={behavior!r}\n"
        "if behavior == 'timeout': time.sleep(10)\n"
        "if behavior == 'exit': sys.exit(7)\n"
        "if behavior == 'descendant': subprocess.Popen([sys.executable,'-c','import time;time.sleep(10)'])\n"
        "print('apptainer version '+('1.3.4' if behavior == 'wrong' else '1.5.3'))\n")
    path.chmod(0o700)
    return dict(apptainer=str(path), apptainerVersion='1.5.3')


BINDING = dict(runId='version-run', candidateDigest='sha256:'+'a'*64, rank=0)


def test_matching_version_retains_reanalyzable_observation_without_repeat(tmp_path):
    profile = launcher(tmp_path)
    observed = verify_runtime_version(profile, tmp_path, binding=BINDING, seconds=2, cleanup_seconds=1)
    assert observed['version'] == '1.5.3'
    assert observed['qualification'] == 'RUNTIME_VERSION_COMPONENT_ONLY'
    assert (tmp_path/'runtime-version/version.log').stat().st_mode & 0o777 == 0o600
    Path(profile['apptainer']).unlink()  # Offline reader never launches the command.
    assert read_runtime_version(tmp_path, expected_version='1.5.3', binding=BINDING) == observed
    with pytest.raises(FileExistsError):
        verify_runtime_version(profile, tmp_path, binding=BINDING, seconds=2, cleanup_seconds=1)


@pytest.mark.parametrize('behavior', ['wrong', 'exit', 'timeout', 'descendant'])
def test_version_failure_retains_log_and_owned_cleanup(tmp_path, behavior):
    profile = launcher(tmp_path, behavior)
    with pytest.raises((ValueError, RuntimeError, subprocess.TimeoutExpired)):
        verify_runtime_version(profile, tmp_path, binding=BINDING,
                               seconds=.1 if behavior == 'timeout' else 2, cleanup_seconds=1)
    record = json.loads((tmp_path/'runtime-version/receipt.json').read_text())
    assert record['status'] == 'FAIL'
    assert record['cleanup'] and all(row['reaped'] for row in record['cleanup'])
    with pytest.raises(ValueError):
        read_runtime_version(tmp_path, expected_version='1.5.3', binding=BINDING)


@pytest.mark.parametrize('fault', ['version', 'run', 'candidate', 'rank', 'cleanup', 'log', 'marker', 'root'])
def test_reader_rejects_wrong_binding_or_semantics_even_with_matching_hash(tmp_path, fault):
    retained_version(tmp_path, run_id=BINDING['runId'], candidate=BINDING['candidateDigest'], rank=0)
    path = tmp_path/'runtime-version/receipt.json'
    record = json.loads(path.read_text())
    if fault == 'version': record['expected'] = '1.3.4'
    elif fault == 'run': record['binding']['runId'] = 'other-run'
    elif fault == 'candidate': record['binding']['candidateDigest'] = 'sha256:'+'b'*64
    elif fault == 'rank': record['binding']['rank'] = 1
    elif fault == 'cleanup': record['cleanup'][0]['forced'] = True
    elif fault == 'marker': record = {'status': 'MATCH'}
    elif fault == 'root': record = [None]
    else:
        payload = b'apptainer version 1.3.4\n'
        (path.parent/'version.log').write_bytes(payload)
        record['log'].update(bytes=len(payload), sha256='sha256:'+hashlib.sha256(payload).hexdigest())
    path.write_text(json.dumps(record))
    with pytest.raises(ValueError):
        read_runtime_version(tmp_path, expected_version='1.5.3', binding=BINDING)


def test_prepared_worker_cannot_launch_without_observed_version(tmp_path):
    from test_yolo_prepared_worker import worker_inputs
    kwargs, plan, receipt = worker_inputs(tmp_path)
    worker = NodeRuntime.from_preparation(plan, expected_receipt_digest=receipt,
        candidate_digest='sha256:'+'a'*64, **kwargs)
    try:
        with pytest.raises(ValueError, match='WORKER_RUNTIME_VERSION_REQUIRED'):
            worker.start_forwarder(6363)
        assert not worker.launches and not worker.started
        worker.verify_runtime(seconds=2)
        worker.profile['apptainerVersion'] = '1.3.4'
        with pytest.raises(ValueError, match='WORKER_RUNTIME_VERSION_REQUIRED'):
            worker.start_forwarder(6363)
        assert not worker.launches and not worker.started
    finally:
        worker.close()


@pytest.mark.parametrize('behavior', ['wrong', 'exit', 'timeout'])
def test_issuer_bad_version_probe_starts_no_preparation_container(tmp_path, behavior):
    from test_yolo_provision import fixture
    from runtime.yolo_operator import provision_run
    kwargs = fixture(tmp_path)
    kwargs['runtime_profile'].update(launcher(tmp_path, behavior))
    if behavior == 'timeout': kwargs['seconds'] = .15
    with pytest.raises((ValueError, RuntimeError, subprocess.TimeoutExpired)):
        provision_run(**kwargs)
    assert not (kwargs['output']/'logs/prepare.log').exists()
    assert not any(kwargs['public'].iterdir())
    assert json.loads((kwargs['output']/'runtime-version/receipt.json').read_text())['status'] == 'FAIL'


@pytest.mark.parametrize('behavior', ['wrong', 'exit', 'timeout'])
def test_rank_bad_version_probe_starts_no_role(tmp_path, behavior):
    from test_yolo_prepared_worker import worker_inputs
    kwargs, plan, receipt = worker_inputs(tmp_path)
    kwargs['profile'].update(launcher(tmp_path, behavior))
    worker = NodeRuntime.from_preparation(plan, expected_receipt_digest=receipt,
        candidate_digest='sha256:'+'a'*64, **kwargs)
    try:
        with pytest.raises((ValueError, RuntimeError, subprocess.TimeoutExpired)):
            worker.verify_runtime(seconds=.1 if behavior == 'timeout' else 2)
        with pytest.raises(ValueError, match='WORKER_RUNTIME_VERSION_REQUIRED'):
            worker.start_forwarder(6363)
        assert not worker.launches and not worker.started and not worker.leases
    finally:
        worker.close()


@pytest.mark.parametrize('local_override', [False, True])
def test_normal_public_reanalysis_requires_this_issuer_and_rank_version(tmp_path, monkeypatch, local_override):
    from test_yolo_submit import submit_module
    from runtime import yolo_bundle, yolo_result
    module = submit_module()
    prepared = dict(runId='version-run', case='local-cpu', candidateDigest=BINDING['candidateDigest'],
        bundle=str(tmp_path/'bundle'), harnessManifestSha256='fixture', plan={'effectiveBehavior': {'profile': {
            'runtime': {'apptainerVersion': '1.5.3'}, 'workload': {}, 'oracle': {}}}})
    if local_override:
        prepared['plan']['effectiveBehavior']['profile']['runtime'] = {
            'apptainerVersion': '1.3.4-1.el9', 'local': {'apptainerVersion': '1.5.3'}}
    node = tmp_path/'node0'
    collection = dict(kind='normal', nodes={0: {'root': str(node)}}, references=[], certifiedGraph={},
        runtimeCandidateDigest='fixture', placementCandidateId='fixture', placementCandidateDigest='fixture',
        graphDigest='fixture', catalogueDigest='fixture', providersByRole={})
    monkeypatch.setattr(module, '_collection_file', lambda root: root/'collection-input.json')
    monkeypatch.setattr(module, '_load_collection_input', lambda *a, **k: (collection, 'fixture'))
    monkeypatch.setattr(yolo_bundle, 'verify_harness', lambda *a, **k: None)
    monkeypatch.setattr(yolo_bundle, 'reference_owner', lambda *a: object())
    calls = []
    monkeypatch.setattr(yolo_result, 'collect_normal_verdict',
        lambda *a, **k: calls.append(True) or {'status': 'PASS'})  # Join test, not numerical qualification.
    retained_version(tmp_path/'prepare-output', run_id='version-run', candidate=BINDING['candidateDigest'], rank='issuer')
    with pytest.raises(ValueError):
        module._reanalyze_retained(tmp_path, prepared)
    assert not calls
    retained_version(node, run_id='version-run', candidate=BINDING['candidateDigest'], rank=0)
    assert set(module._reanalyze_retained(tmp_path, prepared)['runtimeVersions']) == {'issuer', '0'}
    assert calls == [True]
    (tmp_path/'prepare-output/runtime-version/receipt.json').unlink()
    with pytest.raises(ValueError):
        module._reanalyze_retained(tmp_path, prepared)
    assert calls == [True]
