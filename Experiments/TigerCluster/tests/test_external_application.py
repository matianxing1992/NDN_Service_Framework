import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.application import verify_application


BASE = 'sha256:' + 'a' * 64


def bundle(root, extra=None):
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
    identity = {'baseSifSha256': BASE, 'flags': '-O1',
                'builderSha256': 'sha256:' + 'b' * 64,
                'targets': ['App_ServiceController', 'di-native-provider', 'di-native-fault-provider']}
    body = {'schemaVersion': 'spec183-external-application-v1', 'layout': 'layered-v1',
            'scope': 'BUILT_APPLICATION_CANDIDATE', 'baseSifSha256': BASE,
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
