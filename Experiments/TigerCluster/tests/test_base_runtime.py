"""Base packaging regression checks; these do not qualify native DI behavior."""
import importlib.util
from pathlib import Path
import zipfile

import pytest

SCRIPTS = Path(__file__).resolve().parents[1] / 'adapters/slurm-apptainer/scripts'


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), SCRIPTS / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


runtime = module('base-runtime')
builder = module('build-base-sif')


def wheel_fixture(tmp_path, omit=None):
    wheel = tmp_path / 'numpy.whl'
    with zipfile.ZipFile(wheel, 'w') as archive:
        for name in runtime.NUMPY_LIBS - {omit}:
            archive.writestr('numpy.libs/' + name, name.encode())
    return wheel


def test_wheel_digest_rejected_before_repair(tmp_path):
    wheel = wheel_fixture(tmp_path)
    with pytest.raises(RuntimeError, match='NUMPY_WHEEL_DIGEST'):
        runtime.numpy_wheel_files(wheel, '0' * 64)


@pytest.mark.parametrize('missing', sorted(runtime.NUMPY_LIBS))
def test_each_missing_private_library_is_rejected(tmp_path, missing):
    wheel = wheel_fixture(tmp_path, missing)
    with pytest.raises(RuntimeError, match='NUMPY_PRIVATE_LIBRARY_SET'):
        runtime.numpy_wheel_files(wheel, runtime.digest(wheel))


def test_final_bytes_checked_even_when_numpy_would_import(tmp_path):
    wheel = wheel_fixture(tmp_path)
    files = runtime.numpy_wheel_files(wheel, runtime.digest(wheel))
    with zipfile.ZipFile(wheel) as archive:
        archive.extractall(tmp_path / 'site')
    runtime.verify_numpy_files(tmp_path / 'site', files)
    (tmp_path / 'site/numpy.libs' / sorted(runtime.NUMPY_LIBS)[0]).write_bytes(b'corrupt')
    with pytest.raises(RuntimeError, match='NUMPY_PRIVATE_LIBRARY_CHANGED'):
        runtime.verify_numpy_files(tmp_path / 'site', files)


def test_untracked_private_library_is_rejected(tmp_path):
    (tmp_path / 'numpy.libs').mkdir()
    (tmp_path / 'numpy.libs/unexpected.so').write_bytes(b'old ABI')
    with pytest.raises(RuntimeError, match='NUMPY_INSTALLED_LIBRARY_SET'):
        runtime.verify_numpy_files(tmp_path, {})


def test_parent_symlink_rejected(tmp_path):
    (tmp_path / 'real.sif').write_bytes(b'parent')
    (tmp_path / 'base.sif').symlink_to(tmp_path / 'real.sif')
    with pytest.raises(ValueError, match='BASE_PATH_SYMLINK'):
        builder.regular_path(tmp_path / 'base.sif')


def test_parent_directory_symlink_rejected(tmp_path):
    (tmp_path / 'real').mkdir()
    (tmp_path / 'alias').symlink_to(tmp_path / 'real', target_is_directory=True)
    with pytest.raises(ValueError, match='BASE_PATH_SYMLINK'):
        builder.regular_path(tmp_path / 'alias/new', exists=False)


def test_host_prepare_rejected_before_writes(monkeypatch, tmp_path):
    monkeypatch.setattr(runtime.os, 'geteuid', lambda: 1234)
    with pytest.raises(RuntimeError, match='BASE_CONTAINER_ROOT_REQUIRED'):
        runtime.prepare(tmp_path / 'missing-lock', tmp_path)
    assert not list(tmp_path.iterdir())


def test_resolved_unapproved_library_is_rejected(tmp_path):
    library = tmp_path / 'libndn-cxx.so'
    library.write_bytes(b'wrong ABI')
    with pytest.raises(RuntimeError, match='BASE_LINK_ORIGIN'):
        runtime.verify_link_origins('libndn-cxx.so => ' + str(library) + ' (0x1)')


def test_unresolved_library_is_rejected():
    with pytest.raises(RuntimeError, match='BASE_LINK_UNRESOLVED'):
        runtime.verify_link_origins('libopenblas.so => not found')


def test_editable_python_route_is_rejected(tmp_path):
    (tmp_path / 'old-app.egg-link').write_text('/src/ndnsf')
    with pytest.raises(RuntimeError, match='BASE_PYTHON_EDITABLE_INSTALL'):
        runtime.python_routes(tmp_path)


def test_extra_python_hook_is_rejected(tmp_path):
    (tmp_path / 'app.pth').write_text('import ndnsf')
    with pytest.raises(RuntimeError, match='BASE_PYTHON_UNKNOWN_PTH'):
        runtime.python_routes(tmp_path)


def test_known_hook_with_changed_content_is_rejected(tmp_path):
    hook = tmp_path / 'coloredlogs.pth'
    hook.write_text('original')
    expected = {hook.name: runtime.digest(hook)}
    hook.write_text('import old_app')
    with pytest.raises(RuntimeError, match='BASE_PYTHON_HOOK_CONTENT'):
        runtime.python_routes(tmp_path, expected)


def test_system_loader_origin_is_accepted():
    loader = Path('/lib64/ld-linux-x86-64.so.2')
    if not loader.exists():
        pytest.skip('x86_64 loader not installed')
    runtime.verify_link_origins(str(loader) + ' (0x1)')
