"""Repackaging cannot reuse binaries from different source or runtime inputs."""
import importlib.util
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
