"""Dependency source identity must gate skipping native dependency builds."""
import copy
import importlib.util
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / 'adapters/slurm-apptainer/scripts/render-library-runtime.py'
spec = importlib.util.spec_from_file_location('base_renderer', SCRIPT)
renderer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(renderer)


def seal():
    return dict(sourceSelection='base-libraries-v1', files=[{'path': 'Core.cpp', 'sha256': 'old'}],
                dependencies={name: dict(sourceRevision='abc', files=[{'path': 'lib.cpp', 'sha256': 'a'}],
                                         archive={'sha256': 'sha256:archive'})
                              for name in ('nacAbe', 'ndnSvs', 'ndnSd')})


def test_core_change_allows_dependency_reuse():
    old = seal()
    new = copy.deepcopy(old)
    new['files'][0]['sha256'] = 'new-core'
    renderer.validate_dependency_reuse(old, new)


@pytest.mark.parametrize('field', ['sourceRevision', 'files', 'archive'])
def test_changed_dependency_is_rejected(field):
    old = seal()
    new = copy.deepcopy(old)
    new['dependencies']['ndnSvs'][field] = {
        'sourceRevision': 'new', 'files': [], 'archive': {'sha256': 'new'}
    }[field]
    with pytest.raises(ValueError, match='BASE_REUSE_DEPENDENCY_CHANGED:ndnSvs'):
        renderer.validate_dependency_reuse(old, new)


def test_missing_dependency_and_wrong_source_selection_are_rejected():
    old = seal()
    new = copy.deepcopy(old)
    del new['dependencies']['nacAbe']
    with pytest.raises(ValueError, match='BASE_REUSE_DEPENDENCY_SET'):
        renderer.validate_dependency_reuse(old, new)
    new = seal()
    new['sourceSelection'] = 'legacy-complete'
    with pytest.raises(ValueError, match='BASE_REUSE_SOURCE_SELECTION'):
        renderer.validate_dependency_reuse(old, new)
