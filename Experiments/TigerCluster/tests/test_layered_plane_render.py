"""Real renderer to content validator; fixture bytes never qualify runtime."""
import json
from pathlib import Path
import shutil

import pytest

from test_yolo_layered_profile import layered_dispatch
from runtime.yolo_profile import check_operator_profile
from tools import spec183_dispatch_plane as renderer
from tools.spec183_inputs_plane import source_files, render as render_inputs, check as check_inputs


def test_layered_render_binds_the_selected_base_and_app(tmp_path):
    (tmp_path/'seed').mkdir()
    profile_path, profile, app = layered_dispatch(tmp_path/'seed')
    root = tmp_path/'planes'
    root.mkdir()
    inputs = Path(profile['release']['inputs']['path']).parent
    shutil.copytree(inputs, root/'inputs')
    runtime = Path(profile['release']['runtime']['path'])
    rows = json.loads(runtime.read_text())['files']
    sources = {name: runtime.parent/row['path'] for name, row in rows.items()}
    result = renderer.render(root, profile_path, runtime_sources=sources, application=app)
    assert result['status'] == 'RENDERED'
    verified = check_operator_profile(profile_path, stage='dispatch')
    assert verified['application']['integrity'] == 'VERIFIED'
    assert verified['qualification'] == 'NOT_EVALUATED'
    with pytest.raises(ValueError, match='LAYERED_PLANE_OUTPUT_EXISTS'):
        renderer.render(root, profile_path, runtime_sources=sources, application=app)


def test_source_descriptor_resolves_relative_to_itself_and_rejects_missing_keys(tmp_path):
    descriptor = tmp_path/'sources.json'
    descriptor.write_text(json.dumps({'sif': 'runtime.sif'}))
    assert source_files(descriptor, {'sif'}) == {'sif': tmp_path/'runtime.sif'}
    with pytest.raises(ValueError, match='PLANE_SOURCE_FILES'):
        source_files(descriptor, {'sif', 'nativeManifest'})


def test_input_renderer_records_explicit_layered_sources(tmp_path):
    sources = {}
    for name in ('sourceLock', 'sourceSeal', 'buildDefinition', 'baseSif'):
        path = tmp_path/name
        path.write_bytes(('fixture:'+name).encode())
        sources[name] = path
    output = tmp_path/'inputs'
    body = render_inputs(output, sources=sources, layout='layered-v1')
    assert body['parameters'] == {'maxBuildJobs': 2, 'layout': 'layered-v1'}
    assert check_inputs(output)['integrity'] == 'VERIFIED'
