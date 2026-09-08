import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.application import verify_application


BASE = 'sha256:' + 'a' * 64


def bundle(root, extra=None, *, base_sha256=BASE):
    files = {f'bin/{name}': b'component fixture, not ELF' for name in (
        'di-native-provider', 'di-native-fault-provider', 'App_ServiceController')}
    files.update(extra or {})
    rows = []
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        rows.append({'path': name, 'bytes': len(data),
                     'sha256': 'sha256:' + hashlib.sha256(data).hexdigest()})
    identity = {'baseSifSha256': base_sha256, 'flags': '-O1',
                'builderSha256': 'sha256:' + 'b' * 64,
                'targets': ['App_ServiceController', 'di-native-provider', 'di-native-fault-provider']}
    body = {'schemaVersion': 'spec183-external-application-v1', 'layout': 'layered-v1',
            'scope': 'BUILT_APPLICATION_CANDIDATE', 'baseSifSha256': base_sha256,
            'sourceSealDigest': 'sha256:' + 'c' * 64, 'sourceRevision': 'd' * 40,
            'buildIdentity': identity,
            'buildKey': hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest(),
            'files': rows}
    data = json.dumps(body).encode()
    (root / 'application-manifest.json').write_bytes(data)
    return 'sha256:' + hashlib.sha256(data).hexdigest()


def test_content_pass_is_only_an_application_candidate(tmp_path):
    sha = bundle(tmp_path)
    body = verify_application(tmp_path, manifest_sha256=sha, base_sif_sha256=BASE)
    assert body['scope'] == 'BUILT_APPLICATION_CANDIDATE'


def test_wrong_base_rejected(tmp_path):
    sha = bundle(tmp_path)
    with pytest.raises(ValueError, match='APP_BASE_MISMATCH'):
        verify_application(tmp_path, manifest_sha256=sha, base_sif_sha256='sha256:' + 'e' * 64)


def test_changed_binary_rejected(tmp_path):
    sha = bundle(tmp_path)
    (tmp_path / 'bin/di-native-provider').write_bytes(b'changed')
    with pytest.raises(ValueError, match='APP_FILE_CHANGED'):
        verify_application(tmp_path, manifest_sha256=sha, base_sif_sha256=BASE)


def test_even_declared_base_library_shadow_is_rejected(tmp_path):
    sha = bundle(tmp_path, {'lib/libnac-abe.so': b'foreign library'})
    with pytest.raises(ValueError, match='APP_BASE_LIBRARY_SHADOW'):
        verify_application(tmp_path, manifest_sha256=sha, base_sif_sha256=BASE)


def layered_profile(root):
    sha = bundle(root)
    path = root / 'application-manifest.json'
    return {'layout': 'layered-v1', 'sifSha256': BASE[7:],
            'applicationManifest': {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha}}


def test_container_mounts_verified_app_read_only(tmp_path):
    from runtime.baseline import container_command
    profile = dict(layered_profile(tmp_path / 'app'), apptainer='apptainer', sif='/base.sif')
    argv = container_command(profile, tmp_path / 'bundle', tmp_path / 'role',
                             tmp_path / 'public', tmp_path / 'output', ['/app/bin/di-native-provider'])
    assert str(tmp_path / 'app') + ':/app:ro' in argv
    assert 'NDNSF_APP_LAYOUT=layered-v1' in argv
    assert 'PYTHONPATH=/bundle:/app/repo/NDNSF-DistributedInference' in argv


def test_layered_provider_reaches_process_boundary(tmp_path, monkeypatch):
    import subprocess
    from test_yolo_application import application_inputs
    from runtime.yolo_worker import NodeRuntime
    inputs = application_inputs(tmp_path / 'node', 0, mode='local-cpu')
    inputs['profile'].update(layered_profile(tmp_path / 'app'))
    worker = NodeRuntime(**inputs)
    original, calls = subprocess.Popen, []
    def boundary(argv, **kwargs):
        calls.append(argv)
        return original([sys.executable, '-c', 'import time; time.sleep(60)'], **kwargs)
    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        worker.start_provider('BackboneNeck', identity='/run/actual/BackboneNeck',
                              service='/Detection/YOLO', group='/run/sync',
                              controller='/run/controller', permission_wait_ms=12000)
        assert len(calls) == 1
        assert '/app/bin/di-native-provider' in calls[0]
        assert str(tmp_path / 'app') + ':/app:ro' in calls[0]
    finally:
        assert all(row['reaped'] for row in worker.close())
