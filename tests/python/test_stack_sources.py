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


@pytest.fixture
def patched_source(repository, tmp_path, monkeypatch):
    root = tmp_path / 'patch-repository'
    directory = root / 'patches'
    directory.mkdir(parents=True)
    entries = []
    for index, (before, after) in enumerate((('original', 'middle'), ('middle', 'final')), 1):
        patch = directory / f'{index}.patch'
        patch.write_text('diff --git a/tracked.txt b/tracked.txt\n'
                         '--- a/tracked.txt\n+++ b/tracked.txt\n'
                         f'@@ -1 +1 @@\n-{before}\n+{after}\n')
        entries.append({'file': patch.name, 'sha256': sources.digest(patch)})
    series = directory / 'series.json'
    series.write_text(json.dumps({'schema': 1, 'upstreamCommit': git(repository, 'rev-parse', 'HEAD'),
                                  'patches': entries}))
    source = {'url': str(repository), 'commit': git(repository, 'rev-parse', 'HEAD'),
              'patch_series': {'path': 'patches/series.json', 'sha256': sources.digest(series)},
              'requires': ['ndn-cxx', 'openabe'], 'build_recipe_revision': 2}
    monkeypatch.setattr(sources, 'REPOSITORY', root)
    return source, root


@pytest.mark.parametrize('name', ['NAC-ABE', 'openabe'])
def test_materialize_applies_ordered_patches_without_changing_checkout(patched_source, tmp_path, name):
    source, _ = patched_source
    checkout = sources.prepare(source, name, tmp_path / 'deps')
    application = tmp_path / 'application'
    application.mkdir()
    git(application, 'init')
    (application / 'user.txt').write_text('untouched\n')
    # The materialization is deliberately inside an unrelated application Git
    # tree, where unbounded Git discovery previously selected the wrong root.
    target = sources.materialize(source, name, checkout, application / 'build')
    assert (target / 'tracked.txt').read_text() == 'final\n'
    assert (checkout / 'tracked.txt').read_text() == 'original\n'
    assert git(checkout, 'status', '--porcelain') == ''
    assert (application / 'user.txt').read_text() == 'untouched\n'
    assert not (application / 'tracked.txt').exists()


def test_materialize_refuses_dirty_checkout_without_cleanup(patched_source, tmp_path):
    source, _ = patched_source
    checkout = sources.prepare(source, 'NAC-ABE', tmp_path / 'deps')
    (checkout / 'tracked.txt').write_text('user edits\n')
    with pytest.raises(ValueError, match='dirty source'):
        sources.materialize(source, 'NAC-ABE', checkout, tmp_path / 'build')
    assert (checkout / 'tracked.txt').read_text() == 'user edits\n'
    assert not (tmp_path / 'build').exists()


@pytest.mark.parametrize('corruption', ['series', 'patch', 'upstream', 'duplicate', 'escape'])
def test_patch_identity_corruption_is_rejected(patched_source, corruption):
    source, root = patched_source
    series = root / 'patches/series.json'
    data = json.loads(series.read_text())
    if corruption == 'series':
        series.write_text(series.read_text() + '\n')
    elif corruption == 'patch':
        (root / 'patches/1.patch').write_text('tampered\n')
    else:
        if corruption == 'upstream':
            data['upstreamCommit'] = 'a' * 40
        elif corruption == 'duplicate':
            data['patches'].append(data['patches'][0])
        else:
            data['patches'][0]['file'] = '../outside.patch'
        series.write_text(json.dumps(data))
        source['patch_series']['sha256'] = sources.digest(series)
    with pytest.raises(ValueError):
        sources.patch_series(source)


def test_wrong_patch_order_preserves_failed_diagnostic_tree(patched_source, tmp_path):
    source, root = patched_source
    series = root / 'patches/series.json'
    data = json.loads(series.read_text())
    data['patches'].reverse()
    series.write_text(json.dumps(data))
    source['patch_series']['sha256'] = sources.digest(series)
    checkout = sources.prepare(source, 'NAC-ABE', tmp_path / 'deps')
    with pytest.raises(ValueError, match='diagnostic tree preserved'):
        sources.materialize(source, 'NAC-ABE', checkout, tmp_path / 'build')
    assert len(list((tmp_path / 'build').glob('NAC-ABE-patched-*'))) == 1
    assert git(checkout, 'status', '--porcelain') == ''
    assert (checkout / 'tracked.txt').read_text() == 'original\n'


def test_parallel_sdk_receipt_binds_distinct_upstream_and_patch_identity(patched_source, tmp_path):
    source, root = patched_source
    data = {'sources': {'NAC-ABE': source}}
    upstream = tmp_path / 'global'
    (upstream / 'lib').mkdir(parents=True)
    for name in source['requires']:
        (upstream / 'lib' / sources.LIBRARIES[name]).write_bytes(name.encode())
    sdk = sources.sdk_prefix(data, 'NAC-ABE', upstream)
    assert sdk != upstream
    (sdk / 'lib').mkdir(parents=True)
    library = sdk / 'lib/libnac-abe.so'
    library.write_bytes(b'new-nac-api')
    assert sources.receipt(data, 'NAC-ABE', sdk, record=True, upstream_prefix=upstream)
    assert sources.receipt(data, 'NAC-ABE', sdk, upstream_prefix=upstream)
    (upstream / 'lib/libopenabe.so').write_bytes(b'changed upstream')
    assert not sources.receipt(data, 'NAC-ABE', sdk, upstream_prefix=upstream)
    sources.receipt(data, 'NAC-ABE', sdk, record=True, upstream_prefix=upstream)
    library.write_bytes(b'changed target')
    assert not sources.receipt(data, 'NAC-ABE', sdk, upstream_prefix=upstream)
    series = root / 'patches/series.json'
    series.write_text(series.read_text() + '\n')
    source['patch_series']['sha256'] = sources.digest(series)
    assert sources.sdk_prefix(data, 'NAC-ABE', upstream) != sdk
    assert not sources.receipt(data, 'NAC-ABE', sdk, upstream_prefix=upstream)


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
    cxx_receipt = json.loads(
        (tmp_path / 'share/ndnsf/source-receipts/ndn-cxx.json').read_text())
    assert cxx_receipt['source']['commit'] == data['sources']['ndn-cxx']['commit']
    assert 'patches' not in cxx_receipt  # IMS API is included in the pinned fork commit.
    svs_receipt = json.loads(
        (tmp_path / 'share/ndnsf/source-receipts/ndn-svs.json').read_text())
    assert svs_receipt['patches'] == {
        'publication-latency.patch': sources.digest(ROOT / 'tools/ndn-svs/publication-latency.patch')}
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


@pytest.mark.parametrize('name,patch_name', [
    ('ndn-svs', 'publication-latency.patch'),
])
def test_dependency_patch_change_invalidates_receipt(tmp_path, monkeypatch, name, patch_name):
    data = sources.load_lock(LOCK)
    project = tmp_path / 'project'
    helper = project / 'scripts/stack_sources.py'
    helper.parent.mkdir(parents=True)
    helper.write_text('# fixture helper identity\n')
    monkeypatch.setattr(sources, '__file__', str(helper))
    patch = project / 'tools' / name / patch_name
    patch.parent.mkdir(parents=True)
    patch.write_bytes(b'first patch\n')
    library_dir = tmp_path / 'lib'
    library_dir.mkdir()
    for dependency in [name] + data['sources'][name]['requires']:
        (library_dir / sources.LIBRARIES[dependency]).write_bytes(dependency.encode())

    assert sources.receipt(data, name, tmp_path, record=True)
    assert sources.receipt(data, name, tmp_path)
    patch.write_bytes(b'changed patch\n')
    assert not sources.receipt(data, name, tmp_path)


def test_different_pin_invalidates_receipt(tmp_path):
    data = sources.load_lock(LOCK)
    # This case isolates ordinary source-pin receipt invalidation. The locked
    # patched-source mismatch is a separate, earlier fail-closed boundary.
    data['sources']['openabe'].pop('patch_series')
    (tmp_path / 'lib').mkdir()
    (tmp_path / 'lib/libopenabe.so').write_bytes(b'library')
    sources.receipt(data, 'openabe', tmp_path, record=True)
    changed = copy.deepcopy(data)
    changed['sources']['openabe']['commit'] = 'a' * 40
    assert not sources.receipt(changed, 'openabe', tmp_path)


def test_patched_pin_mismatch_rejected_before_receipt_comparison(tmp_path):
    data = sources.load_lock(LOCK)
    (tmp_path / 'lib').mkdir()
    (tmp_path / 'lib/libopenabe.so').write_bytes(b'library')
    assert sources.receipt(data, 'openabe', tmp_path, record=True)
    changed = copy.deepcopy(data)
    changed['sources']['openabe']['commit'] = 'a' * 40
    with pytest.raises(ValueError, match='schema/upstream commit differs'):
        sources.receipt(changed, 'openabe', tmp_path)


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


def test_host_lock_binds_openabe_final_reference_patch():
    data = sources.load_lock(LOCK)
    source = data['sources']['openabe']
    assert source['commit'] == 'cb2a18c2d2845a03ed502df4b77f596b55e48ebd'
    assert source['build_recipe_revision'] == 2
    assert source['patch_series']['path'] == 'patches/openabe/series.json'
    patches = sources.patch_series(source)
    assert [name for name, _ in patches] == [
        '0001-final-reference-release.patch',
        '0002-aes-block-output-and-nist-vector.patch',
    ]
    assert b'src/tools/zprng.cpp' in patches[1][1]
    assert b'src/test_libopenabe.cpp' in patches[1][1]
