"""Real temporary Git trees and offline receipt checks; no install or network."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('stack_sources', ROOT / 'scripts/stack_sources.py')
sources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sources)
LOCK = ROOT / 'packaging/host-dependencies.lock.json'


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


@pytest.fixture
def repository(tmp_path):
    path = tmp_path / 'upstream'
    path.mkdir()
    git(path, 'init')
    git(path, 'config', 'user.name', 'Fixture')
    git(path, 'config', 'user.email', 'fixture@example.invalid')
    (path / 'tracked.txt').write_text('original\n')
    git(path, 'add', 'tracked.txt')
    git(path, 'commit', '-m', 'fixture')
    return path


def test_prepare_pins_commit_and_preserves_dirty_tree(repository, tmp_path):
    sha = git(repository, 'rev-parse', 'HEAD')
    source = {'url': str(repository), 'commit': sha}
    directory = sources.prepare(source, 'ndn-svs', tmp_path / 'deps with space')
    assert git(directory, 'rev-parse', 'HEAD') == sha
    assert sources.prepare(source, 'ndn-svs', directory.parent) == directory
    (directory / 'tracked.txt').write_text('user edit\n')
    with pytest.raises(ValueError, match='dirty source'):
        sources.prepare(source, 'ndn-svs', directory.parent)
    assert (directory / 'tracked.txt').read_text() == 'user edit\n'


def test_wrong_remote_and_symlink_rejected(repository, tmp_path):
    source = {'url': str(repository), 'commit': git(repository, 'rev-parse', 'HEAD')}
    directory = sources.prepare(source, 'ndn-svs', tmp_path / 'deps')
    with pytest.raises(ValueError, match='wrong remote'):
        sources.prepare(dict(source, url='/unrelated'), 'ndn-svs', directory.parent)
    alias = tmp_path / 'aliases'
    alias.mkdir()
    (alias / directory.name).symlink_to(directory)
    with pytest.raises(ValueError, match='symlink'):
        sources.prepare(source, 'ndn-svs', alias)


def test_lock_order_and_invalid_commit(tmp_path):
    data = sources.load_lock(LOCK)
    ordered = sources.order(data)
    assert ordered.index('ndn-svs') < ordered.index('NDNSD')
    data['sources']['ndn-svs']['commit'] = 'Experimental'
    invalid = tmp_path / 'lock.json'
    invalid.write_text(json.dumps(data))
    with pytest.raises(ValueError, match='40-character'):
        sources.load_lock(invalid)


def test_receipts_invalidate_changed_upstream_and_library(tmp_path):
    data = sources.load_lock(LOCK)
    lib = tmp_path / 'lib'
    lib.mkdir()
    for name, filename in sources.LIBRARIES.items():
        (lib / filename).write_bytes(name.encode())
    for name in sources.order(data):
        assert sources.receipt(data, name, tmp_path, record=True)
        assert sources.receipt(data, name, tmp_path)
    (lib / 'libndn-cxx.so').write_bytes(b'new ABI')
    assert not sources.receipt(data, 'ndn-cxx', tmp_path)
    assert not sources.receipt(data, 'ndn-svs', tmp_path)
    assert sources.receipt(data, 'openabe', tmp_path)


def test_downstream_receipt_accepts_compatible_upstream_without_source_receipt(tmp_path):
    data = sources.load_lock(LOCK)
    name = 'NAC-ABE'
    library_dir = tmp_path / 'lib'
    library_dir.mkdir()
    dependencies = data['sources'][name]['requires']
    assert dependencies
    for dependency in dependencies:
        (library_dir / sources.LIBRARIES[dependency]).write_bytes(
            ('reused-' + dependency).encode())
    (library_dir / sources.LIBRARIES[name]).write_bytes(b'built-downstream')

    receipt_dir = tmp_path / 'share/ndnsf/source-receipts'
    assert not any(receipt_dir.glob('*.json'))
    assert sources.receipt(data, name, tmp_path, record=True)
    assert sources.receipt(data, name, tmp_path)


def test_different_pin_invalidates_receipt(tmp_path):
    data = sources.load_lock(LOCK)
    (tmp_path / 'lib').mkdir()
    (tmp_path / 'lib/libopenabe.so').write_bytes(b'library')
    sources.receipt(data, 'openabe', tmp_path, record=True)
    changed = copy.deepcopy(data)
    changed['sources']['openabe']['commit'] = 'a' * 40
    assert not sources.receipt(changed, 'openabe', tmp_path)


def test_build_recipe_revision_invalidates_receipt(tmp_path):
    data = sources.load_lock(LOCK)
    (tmp_path / 'lib').mkdir()
    (tmp_path / 'lib/libopenabe.so').write_bytes(b'library')
    assert sources.receipt(data, 'openabe', tmp_path, record=True)

    changed = copy.deepcopy(data)
    changed['sources']['openabe']['build_recipe_revision'] += 1
    assert not sources.receipt(changed, 'openabe', tmp_path)


def test_unrelated_installer_edit_does_not_invalidate_receipt(tmp_path, monkeypatch):
    data = sources.load_lock(LOCK)
    (tmp_path / 'lib').mkdir()
    (tmp_path / 'lib/libopenabe.so').write_bytes(b'library')
    project = tmp_path / 'project'
    project.mkdir()
    helper = project / 'stack_sources.py'
    helper.write_text('# original helper implementation\n')
    installer = project / 'install_ndnsf_stack.sh'
    installer.write_text('# original profile flags\n')
    monkeypatch.setattr(sources, '__file__', str(helper))

    assert sources.receipt(data, 'openabe', tmp_path, record=True)
    installer.write_text('# unrelated Qwen profile change\n')

    assert sources.receipt(data, 'openabe', tmp_path)
    helper.write_text('# changed helper implementation\n')
    assert not sources.receipt(data, 'openabe', tmp_path)


def test_legacy_lock_defaults_build_recipe_revision_to_one(tmp_path):
    data = sources.load_lock(LOCK)
    for source in data['sources'].values():
        source.pop('build_recipe_revision')
    path = tmp_path / 'legacy-lock.json'
    path.write_text(json.dumps(data))

    loaded = sources.load_lock(path)

    assert all(source['build_recipe_revision'] == 1 for source in loaded['sources'].values())


@pytest.mark.parametrize('revision', [0, -1, True, '1'])
def test_invalid_build_recipe_revision_is_rejected(tmp_path, revision):
    data = sources.load_lock(LOCK)
    data['sources']['openabe']['build_recipe_revision'] = revision
    path = tmp_path / 'invalid-recipe-revision.json'
    path.write_text(json.dumps(data))

    with pytest.raises(ValueError, match='build_recipe_revision'):
        sources.load_lock(path)


def test_unsupported_prerequisite_graph_rejected(tmp_path):
    data = sources.load_lock(LOCK)
    data['sources']['ndn-cxx']['requires'] = ['openabe']
    path = tmp_path / 'lock.json'
    path.write_text(json.dumps(data))
    with pytest.raises(ValueError, match='unsupported native prerequisite graph'):
        sources.load_lock(path)
