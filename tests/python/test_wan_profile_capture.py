"""Host observer contracts only; these fixtures do not attest CUDA execution."""

import hashlib
import gzip
import importlib.util
import json
import sys
from pathlib import Path

import pytest


@pytest.fixture(params=["wan-video-single", "wan-video-pp", "wan-video-pp-tp", "wan-video-tp-pp"])
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


def test_compressed_cli_preserves_raw_identity_and_defers_json_validation(observer, monkeypatch):
    module, root, source, output, _ = observer
    payload = json.dumps([{'fixture': 'closed bytes only ' * 300}]).encode()
    profile = source / 'ort-profile-compressed.json'
    profile.write_bytes(payload)
    args = ['capture-wan-profiles.py', str(root), '--timeout', '1', '--compress',
            '--max-bytes', str(len(payload)), '--max-stored-bytes', '512']
    monkeypatch.setattr(sys, 'argv', args)
    loads = module.json.loads

    def control_only(wire, *a, **kw):
        assert wire != payload, 'compressed live observer decoded the profile'
        return loads(wire, *a, **kw)

    monkeypatch.setattr(module.json, 'loads', control_only)
    module.main()
    archived = output / (profile.name + '.gz')
    assert gzip.decompress(archived.read_bytes()) == payload
    row, = map(loads, (root / 'provider-1-profiles.jsonl').read_text().splitlines())
    assert row['bytes'] == len(payload) and row['stored_bytes'] == archived.stat().st_size
    assert row['sha256'] == hashlib.sha256(payload).hexdigest()
    monkeypatch.setattr(module.json, 'loads', loads)
    monkeypatch.setattr(sys, 'argv', args + ['--verify-only'])
    module.main()
    receipt = loads((root / 'provider-1-profile-verification.json').read_text())
    assert receipt['bytes'] == len(payload) and receipt['stored_bytes'] <= 512
    assert receipt['profile_json_validated']


def test_compressed_storage_overflow_removes_only_its_partial_output(observer):
    module, root, source, output, _ = observer
    (source / 'ort-profile-capacity.json').write_bytes(b'[0]')
    output.parent.mkdir(parents=True)
    canary = output.parent / 'existing'
    canary.write_bytes(b'keep')
    with pytest.raises(ValueError, match='storage capacity exceeded'):
        module.capture(root, timeout=1, compress=True, max_stored_bytes=1)
    assert not list(output.iterdir()) and canary.read_bytes() == b'keep'
    assert not (root / 'provider-1-profiles.jsonl').read_text()


@pytest.mark.parametrize('growth_on_read', [1, 2])
def test_compressed_capture_retries_a_growing_profile(observer, monkeypatch, growth_on_read):
    module, root, source, output, _ = observer
    profile = source / 'ort-profile-growing.json'
    original = b'[0' if growth_on_read == 1 else b'[0]'
    suffix = b']' if growth_on_read == 1 else b'\n'
    payload = original + suffix
    profile.write_bytes(original)
    open_path = Path.open
    reads = 0
    retries = 0

    def grow_profile(path, mode='r', *args, **kwargs):
        nonlocal reads
        if path == profile and mode == 'rb':
            reads += 1
            if reads == growth_on_read:
                with open_path(profile, 'ab') as writer:
                    writer.write(suffix)
        return open_path(path, mode, *args, **kwargs)

    def check_retry(_):
        nonlocal retries
        retries += 1
        assert not list(output.iterdir())
        assert not (root / 'provider-1-profiles.jsonl').read_text()

    monkeypatch.setattr(Path, 'open', grow_profile)
    monkeypatch.setattr(module.time, 'sleep', check_retry)
    result = module.capture(root, timeout=1, compress=True, max_bytes=len(payload),
                            max_stored_bytes=128)
    assert retries == 1 and result['profiles'] == 1
    archived = output / (profile.name + '.gz')
    assert gzip.decompress(archived.read_bytes()) == payload
    row, = map(json.loads, (root / 'provider-1-profiles.jsonl').read_text().splitlines())
    assert row['sha256'] == hashlib.sha256(payload).hexdigest()
    assert module.verify(root, compress=True)['profile_json_validated']


def test_compressed_archive_requires_explicit_mode_and_bounded_expansion(observer):
    module, root, source, output, _ = observer
    profile = source / 'ort-profile-expansion.json'
    profile.write_bytes(b'[0,0,0]')
    module.capture(root, timeout=1, compress=True)
    with pytest.raises(ValueError, match='compression differs'):
        module.verify(root)
    index = root / 'provider-1-profiles.jsonl'
    row = json.loads(index.read_text())
    row['bytes'] = 2
    index.write_text(json.dumps(row) + '\n')
    with pytest.raises(ValueError, match='uncompressed size differs'):
        module.verify(root, compress=True)
    assert not (root / 'provider-1-profile-verification.json').exists()


def test_compressed_malformed_profile_fails_post_run_validation(observer):
    module, root, source, _, _ = observer
    (source / 'ort-profile-invalid.json').write_bytes(b'[]]')
    assert module.capture(root, timeout=1, compress=True)['profiles'] == 1
    with pytest.raises(json.JSONDecodeError):
        module.verify(root, compress=True)
    assert not (root / 'provider-1-profile-verification.json').exists()
