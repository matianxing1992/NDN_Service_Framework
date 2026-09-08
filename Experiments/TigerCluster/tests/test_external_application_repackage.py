"""Repackaging cannot reuse binaries from different source or runtime inputs."""
import importlib.util
import json
from pathlib import Path

import pytest

from test_external_application import bundle, BASE


@pytest.fixture
def builder(monkeypatch):
    path = Path(__file__).resolve().parents[1]/'adapters/slurm-apptainer/scripts/build-external-yolo.py'
    spec = importlib.util.spec_from_file_location('external_builder', path)
    owner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owner)
    monkeypatch.setattr(owner, 'FLAGS', '-O1')
    return owner


def test_identical_source_preserves_original_compiler_identity(tmp_path, builder):
    bundle(tmp_path)
    body = builder.reusable_application(tmp_path,
        dict(sealDigest='sha256:'+'c'*64, sourceRevision='d'*40), BASE)
    assert body['buildIdentity']['builderSha256'] == 'sha256:'+'b'*64


@pytest.mark.parametrize('fault', [None, 'source', 'native', 'parent', 'extra'])
def test_python_repack_parent_is_explicit_and_source_bound(builder, fault):
    seal = 'sha256:' + 'c' * 64
    record = dict(schema='spec183-base-python-repack-v1', parentSifSha256=BASE,
                  previousSealSha256='sha256:'+'d'*64, sourceSealSha256=seal,
                  files=[dict(path='NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py',
                              installed='py_repoclient/orchestration.py', bytes=11,
                              oldSha256='sha256:'+'a'*64, sha256='sha256:'+'b'*64)])
    if fault == 'source':
        record['sourceSealSha256'] = 'sha256:' + 'e' * 64
    elif fault == 'native':
        record['files'][0]['path'] = 'pythonWrapper/src/ndnsf/_ndnsf.cpp'
    elif fault == 'parent':
        record['parentSifSha256'] = 'unbound'
    elif fault == 'extra':
        record['unchecked'] = True
    if fault is None:
        assert builder.python_repack_parent(record, seal) == BASE
    else:
        with pytest.raises(ValueError, match='APP_BASE_REPACK'):
            builder.python_repack_parent(record, seal)


@pytest.mark.parametrize('fault', ['source', 'revision', 'base', 'binary', 'flags'])
def test_rejects_reuse_across_changed_inputs(tmp_path, builder, monkeypatch, fault):
    bundle(tmp_path)
    seal = dict(sealDigest='sha256:'+'c'*64, sourceRevision='d'*40)
    base = BASE
    if fault == 'source':
        seal['sealDigest'] = 'sha256:'+'e'*64
    elif fault == 'revision':
        seal['sourceRevision'] = 'e'*40
    elif fault == 'base':
        base = 'sha256:'+'e'*64
    elif fault == 'binary':
        (tmp_path/'bin/di-native-provider').write_bytes(b'changed')
    else:
        monkeypatch.setattr(builder, 'FLAGS', '-O3')
    with pytest.raises(ValueError):
        builder.reusable_application(tmp_path, seal, base)


@pytest.mark.parametrize('fault', [None, 'marker', 'symlink', 'base', 'flags'])
def test_incremental_cache_requires_owned_matching_runtime(tmp_path, builder, monkeypatch, fault):
    app = tmp_path/'application'
    bundle(app)
    body = json.loads((app/'application-manifest.json').read_text())
    cache = tmp_path/'cache'
    work = cache/body['buildKey']
    work.mkdir(parents=True)
    marker = work/'cache-identity.json'
    marker.write_text(json.dumps(body['buildIdentity']))
    base = BASE
    if fault == 'marker':
        marker.write_text('{}')
    elif fault == 'symlink':
        actual = work.with_name('actual')
        work.rename(actual)
        work.symlink_to(actual, target_is_directory=True)
    elif fault == 'base':
        base = 'sha256:'+'e'*64
    elif fault == 'flags':
        monkeypatch.setattr(builder, 'FLAGS', '-O3')
    if fault is not None:
        with pytest.raises(ValueError):
            builder.compatible_build_cache(cache, app, base)
    else:
        selected, identity = builder.compatible_build_cache(cache, app, base)
        assert selected == work and identity == body['buildIdentity']


def test_incremental_cache_can_be_selected_by_the_new_application(tmp_path, builder):
    cache = tmp_path/'cache'
    work = cache/'previous'
    work.mkdir(parents=True)
    (work/'object.o').write_bytes(b'preserved build object')
    identity = {'fixture': 'new compiler provenance'}
    builder.publish_cache(cache, work, identity, 'next')
    assert not work.exists()
    assert (cache/'next/object.o').read_bytes() == b'preserved build object'
    assert json.loads((cache/'next/cache-identity.json').read_text()) == identity
    work.mkdir()
    with pytest.raises(ValueError, match='APP_CACHE_DESTINATION_EXISTS'):
        builder.publish_cache(cache, work, identity, 'next')
