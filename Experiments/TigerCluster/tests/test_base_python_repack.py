"""Reject changes that would invalidate native reuse during a Python repack."""
import copy
import importlib.util
from pathlib import Path

import pytest

PATH = Path(__file__).resolve().parents[1] / 'adapters/slurm-apptainer/scripts/repack-base-python.py'
SPEC = importlib.util.spec_from_file_location('base_python_repack', PATH)
repack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repack)


def seal():
    return dict(sourceSelection='base-libraries-v1', dependencies={
        'nacAbe': dict(sourceRevision='pinned', files=[], archive=dict(sha256='unchanged'))},
        files=[dict(path='NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py',
                    bytes=10, sha256='old'),
               dict(path='ndn-service-framework/ServiceProvider.cpp', bytes=20, sha256='native')])


def test_only_existing_package_python_can_change():
    old = seal()
    new = copy.deepcopy(old)
    new['files'][0].update(bytes=11, sha256='new')
    rows = repack.changes(old, new)
    assert rows == [dict(path=old['files'][0]['path'], installed='py_repoclient/orchestration.py',
                         oldSha256='old', sha256='new', bytes=11)]


@pytest.mark.parametrize('mutation,reason', [
    ('native', 'REPACK_NON_PYTHON_CHANGE'),
    ('dependency', 'REPACK_DEPENDENCY_CHANGED'),
    ('file-set', 'REPACK_FILE_SET_CHANGED'),
    ('empty', 'REPACK_NO_CHANGE'),
])
def test_repack_refuses_unproven_reuse(mutation, reason):
    old = seal()
    new = copy.deepcopy(old)
    if mutation == 'native':
        new['files'][1]['sha256'] = 'changed'
    elif mutation == 'dependency':
        new['dependencies']['nacAbe']['sourceRevision'] = 'other'
    elif mutation == 'file-set':
        new['files'].pop()
    with pytest.raises(ValueError, match=reason):
        repack.changes(old, new)
