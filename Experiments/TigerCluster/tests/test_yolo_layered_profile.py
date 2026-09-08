"""Layered content/profile binding; all payloads remain non-runtime fixtures."""
import json
from pathlib import Path

import pytest

from test_external_application import bundle
from test_yolo_submit import dispatch_profile, file_ref, refresh_effective_profile
from runtime.yolo_profile import check_operator_profile, check_plane, load_operator_profile, ClosureError


def layered_dispatch(tmp_path):
    path, profile, _ = dispatch_profile(tmp_path)
    parent = None
    for stage in ('inputs', 'runtime'):
        plane = Path(profile['release'][stage]['path'])
        body = json.loads(plane.read_text())
        body['parameters']['layout'] = 'layered-v1'
        body['parentId'] = parent
        plane.write_text(json.dumps(body))
        parent = check_plane(plane, expected_stage=stage, parent_id=parent)['id']
        profile['release'][stage] = file_ref(plane)
    image_sha = body['files']['sif']['sha256']
    plane = Path(profile['release']['dispatch']['path'])
    app = plane.parent / 'application'
    bundle(app, base_sha256=image_sha)
    reference = file_ref(app / 'application-manifest.json')
    profile['runtime'].update(layout='layered-v1', applicationManifest=reference,
                             nativeProvider='/app/bin/di-native-provider')
    body = json.loads(plane.read_text())
    body['parameters']['layout'] = 'layered-v1'
    body['parentId'] = parent
    body['files']['applicationManifest'] = dict(reference, path='application/application-manifest.json')
    plane.write_text(json.dumps(body))
    profile['release']['dispatch'] = file_ref(plane)
    path.write_text(json.dumps(profile))
    refresh_effective_profile(path, profile)
    return path, profile, app


def test_layered_dispatch_verifies_content_without_qualification(tmp_path):
    path, _, _ = layered_dispatch(tmp_path)
    result = check_operator_profile(path, stage='dispatch')
    assert result['application']['integrity'] == 'VERIFIED'
    assert result['qualification'] == 'NOT_EVALUATED'
    assert result['status'] == 'INCOMPLETE'


def test_layered_dispatch_rejects_binary_tamper(tmp_path):
    path, _, app = layered_dispatch(tmp_path)
    (app / 'bin/di-native-provider').write_bytes(b'tampered')
    with pytest.raises(ClosureError, match='APP_CONTENT_BINDING'):
        check_operator_profile(path, stage='dispatch')


@pytest.mark.parametrize('field', ['layout', 'applicationManifest', 'nativeProvider'])
def test_profile_rejects_mixed_legacy_and_layered_fields(tmp_path, field):
    path, profile, _ = layered_dispatch(tmp_path)
    if field == 'nativeProvider':
        profile['runtime'][field] = '/opt/ndnsf-di/current/bin/di-native-provider'
    else:
        del profile['runtime'][field]
    path.write_text(json.dumps(profile))
    with pytest.raises(ClosureError, match='PROFILE_SCHEMA'):
        load_operator_profile(path, stage='dispatch')
