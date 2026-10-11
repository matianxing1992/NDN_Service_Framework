"""Repeated experiments reuse their probe evidence directory without purging it."""
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest


@pytest.fixture
def probe_module(monkeypatch):
    path = Path(__file__).resolve().parents[2] / 'Experiments/native_probe_validation_links.py'
    spec = importlib.util.spec_from_file_location('probe_workspace_fixture', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.setattr(module.shutil, 'which', lambda name: '/bin/' + name)
    monkeypatch.setitem(sys.modules, 'mininet.link', SimpleNamespace(TCLink=object))

    def stop_at_network(*args, **kwargs):
        raise RuntimeError('fixture network boundary')

    monkeypatch.setattr(module.subprocess, 'run', stop_at_network)
    return module


def configure(module, output):
    hosts = [SimpleNamespace(name=name, pid=1,
        params={'params': {'homeDir': str(output.parent)}}) for name in ('one', 'two')]
    module.configure_probe_validation_links(SimpleNamespace(net=SimpleNamespace(hosts=hosts)),
        output, 4, {'one': '/producer/one', 'two': '/producer/two'})


@pytest.mark.parametrize('existing', [False, True])
def test_fresh_and_reused_directories_reach_network_without_purging(probe_module, tmp_path, existing):
    output = tmp_path / 'probe-validation-links'
    if existing:
        output.mkdir()
        (output / 'keep.log').write_text('previous evidence')
    with pytest.raises(RuntimeError, match='fixture network boundary'):
        configure(probe_module, output)
    assert output.is_dir()
    if existing:
        assert (output / 'keep.log').read_text() == 'previous evidence'


def test_symlink_output_is_rejected(probe_module, tmp_path):
    target = tmp_path / 'target'
    target.mkdir()
    output = tmp_path / 'probe-validation-links'
    output.symlink_to(target, target_is_directory=True)
    with pytest.raises(ValueError, match='symlink'):
        configure(probe_module, output)
    assert output.is_symlink()


def test_existing_file_is_preserved(probe_module, tmp_path):
    output = tmp_path / 'probe-validation-links'
    output.write_text('keep')
    with pytest.raises(FileExistsError):
        configure(probe_module, output)
    assert output.read_text() == 'keep'
