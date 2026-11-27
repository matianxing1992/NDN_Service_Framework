"""Host observer contracts only; these fixtures do not attest CUDA execution."""

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import pytest


@pytest.fixture(params=["wan-video-pp", "wan-video-pp-tp", "wan-video-tp-pp"])
def observer(tmp_path, monkeypatch, request):
    path = (Path(__file__).resolve().parents[2] / 'Experiments/TigerCluster/scripts' /
            'capture-wan-profiles.py')
    spec = importlib.util.spec_from_file_location('wan_profile_capture', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.setenv('SLURM_JOB_ID', '123')
    monkeypatch.setattr(module.socket, 'gethostname', lambda: 'test-back')
    root = tmp_path / 'operation'
    source = tmp_path / 'artifacts'
    source.mkdir()
    clip = tmp_path / 'output/video/clip'
    deployment = request.param
    boot = 'f' * 32

    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    write(root / 'd/deployment-real.json', dict(
        deployment_id=deployment, nodes=[dict(id='gpu1', host='test-back')],
        actors=[dict(id='provider-1', kind='provider', node='gpu1',
                     parameters={'artifact-cache-dir': str(source)}),
                dict(id='requester', kind='requester', node='gpu0')],
        requester=dict(wan_generation=dict(output_directory=str(clip)))))
    write(root / 'd/state' / deployment / 'state.json',
          dict(boot_id=boot, job_reference=dict(job_id='123')))
    evidence = root / 'd/evidence' / deployment / boot
    write(evidence / 'nodes/gpu1/gpu-allocation.json', dict(
        job_id='123', host='test-back', devices=[dict(uuid='GPU-test-fixture-only')]))
    state = evidence / 'actors/requester/runtime' / deployment / 'state.json'
    write(state, dict(status='STOPPED'))
    return module, root, source, clip.parents[1] / 'profiles/provider-1', state


def test_back_observer_drains_without_peer_or_live_profile_json_decode(observer, monkeypatch):
    module, root, source, output, _ = observer
    payload = b'[{"fixture":"opaque observer bytes; not CUDA evidence"}]'
    profile = source / 'ort-profile-complete.json'
    profile.write_bytes(payload)
    loads = module.json.loads

    def control_json_only(wire, *args, **kwargs):
        assert wire != payload, 'live observer decoded the profile instead of copying it'
        return loads(wire, *args, **kwargs)

    monkeypatch.setattr(module.json, 'loads', control_json_only)
    monkeypatch.setattr(module.subprocess, 'check_output',
                        lambda *a, **k: pytest.fail('terminal observer queried NVML'))
    result = module.capture(root, timeout=1)
    assert result['profiles'] == 1 and not result['cancelled']
    assert not (root / 'capture-drain').exists()
    assert (output / profile.name).read_bytes() == payload
    row, = [loads(line) for line in (root / 'provider-1-profiles.jsonl').read_text().splitlines()]
    assert row['sha256'] == hashlib.sha256(payload).hexdigest()
    assert row['bytes'] == len(payload)
    monkeypatch.setattr(module.json, 'loads', loads)
    assert module.verify(root)['profile_json_validated']


def test_observer_waits_for_closed_file_then_retires_on_requester_state(observer, monkeypatch):
    module, root, source, output, state = observer
    profile = source / 'ort-profile-closing.json'
    profile.write_bytes(b'[')
    state.write_text(json.dumps(dict(status='REQUEST_RUNNING')))
    monkeypatch.setattr(module.subprocess, 'check_output', lambda *a, **k: '32760, 32212')

    def close_profile(_):
        profile.write_bytes(b'[]')
        state.write_text(json.dumps(dict(status='STOPPED')))

    monkeypatch.setattr(module.time, 'sleep', close_profile)
    result = module.capture(root, timeout=1)
    assert result['profiles'] == 1
    assert (output / profile.name).read_bytes() == b'[]'


def test_observer_rejects_capacity_before_copying(observer):
    module, root, source, output, _ = observer
    (source / 'ort-profile-large.json').write_bytes(b'[0]')
    with pytest.raises(ValueError, match='capacity exceeded'):
        module.capture(root, max_bytes=2)
    assert not list(output.iterdir())


def test_observer_rejects_profile_symlink(observer):
    module, root, source, output, _ = observer
    target = source / 'other.json'
    target.write_bytes(b'[]')
    (source / 'ort-profile-link.json').symlink_to(target)
    with pytest.raises(ValueError, match='owned regular file'):
        module.capture(root)
    assert not list(output.iterdir())


def test_profile_json_validation_is_deferred_and_still_rejects_malformed_archive(observer):
    module, root, source, _, _ = observer
    (source / 'ort-profile-invalid.json').write_bytes(b'[]]')
    assert module.capture(root, timeout=1)['profiles'] == 1
    with pytest.raises(json.JSONDecodeError):
        module.verify(root)
    assert not (root / 'provider-1-profile-verification.json').exists()


def test_host_cli_arguments_capture_and_verify_the_same_archive(observer, monkeypatch):
    module, root, source, output, _ = observer
    (source / 'ort-profile-cli.json').write_bytes(b'[]')
    monkeypatch.setattr(module.subprocess, 'check_output',
                        lambda *a, **k: pytest.fail('terminal observer queried NVML'))
    args = ['capture-wan-profiles.py', str(root), '--output-root', str(output.parents[1]),
            '--timeout', '1', '--max-bytes', '1048576', '--max-profiles', '16']
    monkeypatch.setattr(sys, 'argv', args)
    module.main()
    assert (output / 'ort-profile-cli.json').read_bytes() == b'[]'
    monkeypatch.setattr(sys, 'argv', args + ['--verify-only'])
    module.main()
    receipt = json.loads((root / 'provider-1-profile-verification.json').read_bytes())
    assert receipt['profiles'] == 1 and receipt['profile_json_validated']
