"""Effective behavior must agree, even when every manifest hash is valid."""
import copy
import json
from pathlib import Path

import pytest

from test_yolo_submit import dispatch_profile, file_ref
from runtime.yolo_profile import ClosureError, check_operator_profile, effective_profile_document


@pytest.mark.parametrize('fault', ['timing', 'oracle', 'legacy-profile-id', 'extra'])
def test_rehashed_wrong_snapshot_is_rejected(tmp_path, fault):
    path, profile, _ = dispatch_profile(tmp_path)
    assert check_operator_profile(path, stage='dispatch')['integrity'] == 'VERIFIED'
    plane = Path(profile['release']['dispatch']['path'])
    doc = json.loads(plane.read_text())
    snapshot = plane.parent / doc['files']['effectiveProfile']['path']
    value = json.loads(snapshot.read_text())
    if fault == 'timing':
        value['effectiveBehavior']['timing']['startupSeconds'] += 1
    elif fault == 'oracle':
        value['effectiveBehavior']['oracle']['reference']['sha256'] = 'sha256:'+'a'*64
    elif fault == 'legacy-profile-id':
        value['effectiveBehavior']['profileId'] = profile['profileId']
    else:
        value['ignored'] = True
    snapshot.write_text(json.dumps(value))
    doc['files']['effectiveProfile'] = dict(file_ref(snapshot), path=snapshot.name)
    plane.write_text(json.dumps(doc))
    profile['release']['dispatch'] = file_ref(plane)
    path.write_text(json.dumps(profile))
    with pytest.raises(ClosureError, match='EFFECTIVE_PROFILE_BINDING'):
        check_operator_profile(path, stage='dispatch')


def test_physical_paths_and_gate_updates_do_not_change_behavior(tmp_path):
    _, profile, _ = dispatch_profile(tmp_path)
    changed = copy.deepcopy(profile)
    changed['release']['gates']['localSif'] = file_ref(tmp_path / 'profile.json')
    changed['oracle']['reference']['path'] = '/different/location'
    changed['runtime']['apptainer'] = '/another/apptainer'
    changed['security']['authorityPrivateKey'] = '/private/elsewhere'
    changed['storage']['sharedRunRoot'] = '/project/another/runs'
    assert effective_profile_document(changed) == effective_profile_document(profile)


def test_renderer_refreshes_nonrelease_rows_before_snapshot_once(tmp_path, monkeypatch):
    from tools import spec183_dispatch_plane as renderer
    from runtime import yolo_profile
    path, profile, _ = dispatch_profile(tmp_path)
    planes = tmp_path
    # Real small content planes and real frozen harness. Only source locations
    # are redirected to fixtures; no renderer, hash or validator is doubled.
    runtime_sources = {name: Path(tmp_path / 'runtime' / name)
                       for name in ('sif', 'nativeManifest', 'libraryLock')}
    dispatch_sources = {name: Path(tmp_path / 'dispatch' / name)
                        for name in ('modelManifest', 'oracle', 'fixture', 'trustPolicy', 'validationContract')}
    source = tmp_path / 'saved-sources'
    source.mkdir()
    for rows in (runtime_sources, dispatch_sources):
        for name, old in list(rows.items()):
            target = source / name
            target.write_bytes(old.read_bytes())
            rows[name] = target
    monkeypatch.setattr(renderer, 'RUNTIME_SOURCES', runtime_sources)
    monkeypatch.setattr(renderer, 'DISPATCH_SOURCES', dispatch_sources)
    profile['evidence']['harnessManifest']['path'] = str(tmp_path / 'dispatch/harness/harness-manifest.json')
    contract = tmp_path / 'fixture-only.txt'
    contract.write_text('updated contract; stale file rows must be refreshed')
    path.write_text(json.dumps(profile))
    first = renderer.render(planes, path)
    result = yolo_profile.check_operator_profile(path, stage='dispatch')
    assert result['integrity'] == 'VERIFIED'
    assert result['qualification'] == 'NOT_EVALUATED'
    encoded = path.read_bytes()
    second = renderer.render(planes, path)
    assert first['dispatch'] == second['dispatch']
    assert path.read_bytes() == encoded
    assert second['profileRowsUpdated'] == []
