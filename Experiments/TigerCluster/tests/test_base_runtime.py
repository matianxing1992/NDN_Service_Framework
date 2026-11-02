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
sdk = module('dependency-sdk')


def test_native_smoke_rebuild_rejects_host_before_mutation(monkeypatch, tmp_path):
    monkeypatch.setattr(runtime.os, 'geteuid', lambda: 1234)
    monkeypatch.setattr(runtime, 'MANIFEST', tmp_path)
    with pytest.raises(RuntimeError, match='BASE_CONTAINER_ROOT_REQUIRED'):
        runtime.rebuild_native_smoke()
    assert not list(tmp_path.iterdir())


def test_native_smoke_uses_same_installed_sdk_compiler_path(monkeypatch, tmp_path):
    import json
    calls = []
    monkeypatch.setattr(runtime, 'MANIFEST', tmp_path)
    monkeypatch.setattr(runtime, 'run', lambda *args: 'frozen toolchain information')
    monkeypatch.setattr(runtime.subprocess, 'run', lambda args, **kwargs: calls.append(args))
    runtime.compile_native_smoke()
    assert len(calls) == 1
    command = calls[0]
    assert command[0] == '/usr/bin/g++'
    assert '-I/opt/onnxruntime/include' in command and '-L/opt/onnxruntime/lib' in command
    assert command[-2:] == ['-o', str(tmp_path / 'base-smoke')]
    assert '-lndn-cxx' in command and '-lonnxruntime' in command
    assert 'OrtGetApiBase()->GetVersionString()' in (tmp_path / 'base-smoke.cpp').read_text()
    assert json.loads((tmp_path / 'compiler.json').read_text())['command'] == command


def test_ort_template_rebuilds_existing_abi_probe_before_image_sealing():
    template = (SCRIPTS.parent / 'templates/onnxruntime-upgrade.def.in').read_text()
    post, rest = template.split('%post\n', 1)[1].split('%test', 1)
    assert post.index('upgrade-onnxruntime') < post.index('rebuild-native-smoke')
    assert '/build-input/base-runtime.py rebuild-native-smoke' in post
    assert '/opt/ndn-base/manifest/base-runtime.py verify' in rest


def test_ort_upgrade_rejects_host_before_mutation(monkeypatch, tmp_path):
    monkeypatch.setattr(sdk.os, 'geteuid', lambda: 1234)
    with pytest.raises(ValueError, match='SDK_CONTAINER_REQUIRED'):
        sdk.upgrade_onnxruntime(tmp_path)
    assert not list(tmp_path.iterdir())


@pytest.mark.parametrize('conflict', ['repair_application_metadata', 'repair_framework_headers',
                                   'complete_nac_abe', 'complete_tokenizer_bridge',
                                   'dependency_bundle', 'wheel', 'nac_source_archive'])
def test_ort_upgrade_conflicting_modes_rejected(conflict, tmp_path):
    from types import SimpleNamespace
    args = SimpleNamespace(upgrade_onnxruntime=tmp_path, **{conflict: True})
    with pytest.raises(ValueError, match='BASE_COMPLETION_MODE_CONFLICT'):
        builder.build(args)
    assert not list(tmp_path.iterdir())


@pytest.mark.parametrize('change', ['digest', 'size', 'symlink', 'duplicate', 'path', 'bool-size'])
def test_ort_input_pair_rejects_changed_identity(tmp_path, change):
    import json
    ort = tmp_path / 'ort.tgz'
    cuda = tmp_path / 'cuda.tar.xz'
    ort.write_bytes(b'frozen ort input')
    cuda.write_bytes(b'frozen cuda input')
    rows = {'schema': 'ndnsf-ort-upgrade-v1', 'parentSha256': 'a' * 64,
            'parentSdkSha256': 'b' * 64}
    for name, path in [('onnxRuntimeCpp', ort), ('cudaRuntime', cuda)]:
        rows[name] = {'filename': path.name, 'version': '1.31.1',
                      'sha256': sdk.digest(path), 'bytes': path.stat().st_size}
    lock = tmp_path / 'onnxruntime-upgrade.lock.json'
    lock.write_text(json.dumps(rows))
    assert sdk.verify_onnxruntime_inputs(tmp_path) == rows
    if change == 'digest':
        ort.write_bytes(b'changed ort file')
    elif change == 'size':
        rows['onnxRuntimeCpp']['bytes'] += 1
    elif change == 'symlink':
        ort.rename(tmp_path / 'original')
        ort.symlink_to(tmp_path / 'original')
    elif change == 'duplicate':
        rows['cudaRuntime'] = dict(rows['onnxRuntimeCpp'])
    elif change == 'path':
        rows['onnxRuntimeCpp']['filename'] = '../ort.tgz'
    else:
        rows['onnxRuntimeCpp']['bytes'] = True
    lock.write_text(json.dumps(rows))
    with pytest.raises(ValueError, match='SDK_ORT_ARCHIVE_'):
        sdk.verify_onnxruntime_inputs(tmp_path)


@pytest.mark.parametrize('bad', ['traversal', 'external-link', 'symlink-parent', 'hard-link'])
def test_ort_archive_rejected_before_extraction(tmp_path, bad):
    import io
    import tarfile
    archive = tmp_path / 'vendor.tar'
    with tarfile.open(archive, 'w') as output:
        if bad == 'traversal':
            member = tarfile.TarInfo('../outside')
        elif bad == 'external-link':
            member = tarfile.TarInfo('sdk/lib/libbad.so')
            member.type = tarfile.SYMTYPE
            member.linkname = '../../../outside'
        elif bad == 'hard-link':
            member = tarfile.TarInfo('sdk/lib/hard')
            member.type = tarfile.LNKTYPE
            member.linkname = 'sdk/lib/target'
        else:
            member = tarfile.TarInfo('sdk/lib')
            member.type = tarfile.SYMTYPE
            member.linkname = 'target'
            output.addfile(member)
            target = tarfile.TarInfo('sdk/target')
            target.type = tarfile.DIRTYPE
            output.addfile(target)
            member = tarfile.TarInfo('sdk/lib/child')
        output.addfile(member, io.BytesIO(b''))
    destination = tmp_path / 'extracted'
    with pytest.raises(ValueError, match='SDK_ORT_ARCHIVE_'):
        sdk.extract_runtime_archive(archive, destination)
    assert not destination.exists()


def test_ort_archive_preserves_internal_soname_links(tmp_path):
    import io
    import tarfile
    archive = tmp_path / 'vendor.tar'
    with tarfile.open(archive, 'w') as output:
        member = tarfile.TarInfo('sdk/lib/runtime.so.1')
        member.size = 3
        output.addfile(member, io.BytesIO(b'elf'))
        link = tarfile.TarInfo('sdk/lib/runtime.so')
        link.type = tarfile.SYMTYPE
        link.linkname = 'runtime.so.1'
        output.addfile(link)
    root = sdk.extract_runtime_archive(archive, tmp_path / 'extracted')
    assert root == tmp_path / 'extracted/sdk'
    assert (root / 'lib/runtime.so').is_symlink()
    assert (root / 'lib/runtime.so').read_bytes() == b'elf'


@pytest.mark.parametrize('package', runtime.APP_DISTRIBUTIONS + (
    'ndnsf_di_adapter_llama', 'ndnsf_di_adapter_onnx', 'ndnsf_di_adapter_qwen',
    'NDNSF.DI.Adapter.LLAMA'))
@pytest.mark.parametrize('kind', ['dist-info', 'egg-info-dir', 'egg-info-file', 'egg-info-link'])
def test_application_metadata_rejected_without_importable_module(tmp_path, package, kind):
    site = tmp_path / 'venv/lib/python3.10/site-packages'
    site.mkdir(parents=True)
    metadata = site / (package + ('-0.111.0.dist-info' if kind == 'dist-info' else '.egg-info'))
    if kind == 'egg-info-link':
        metadata.symlink_to(tmp_path / 'missing')
    elif kind == 'egg-info-file':
        metadata.write_text('Name: ' + package.replace('_', '-') + '\n')
    else:
        metadata.mkdir()
        (metadata / ('METADATA' if kind == 'dist-info' else 'PKG-INFO')).write_text(
            'Name: ' + package.replace('_', '-') + '\n')
    with pytest.raises(RuntimeError, match='APPLICATION_METADATA_IN_BASE'):
        runtime.verify_application_metadata(site)
    runtime.clean_application_metadata({'artifacts': {}}, site)
    assert not metadata.exists() and not metadata.is_symlink()


def test_metadata_cleanup_preserves_sdk_dependencies_modules_and_hooks(tmp_path):
    site = tmp_path / 'venv/lib/python3.10/site-packages'
    owned = site / 'ndnsf_di_ops-0.111.0.dist-info'
    owned.mkdir(parents=True)
    (owned / 'entry_points.txt').write_text('[console_scripts]\nndnsf-di=old:main\n')
    launcher = tmp_path / 'venv/bin/ndnsf-di'
    launcher.parent.mkdir()
    launcher.write_text('old NDNSF entry point')
    retained = [site / 'onnxruntime_gpu-1.20.dist-info/METADATA',
                site / 'ndnsf/__init__.py', site / 'distutils-precedence.pth',
                tmp_path / 'ndn-sdk/include/ndn-cxx/name.hpp']
    for path in retained:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('preserved ' + path.name)
    before = {path: path.read_bytes() for path in retained}
    rows = runtime.clean_application_metadata(
        {'artifacts': {str(retained[-1]): runtime.digest(retained[-1])}}, site)
    assert not owned.exists() and not launcher.exists()
    assert all(path.read_bytes() == data for path, data in before.items())
    assert {row['path'] for row in rows} == {str(owned), str(owned / 'entry_points.txt'),
                                            str(launcher)}
    runtime.verify_application_metadata(site)


@pytest.mark.parametrize('package', ['ndnsf_di_ops', 'ndnsf_di_adapter_llama'])
def test_metadata_sdk_conflict_rejected_before_removing_launcher(tmp_path, package):
    site = tmp_path / 'venv/lib/python3.10/site-packages'
    owned = site / (package + '-0.111.0.dist-info')
    owned.mkdir(parents=True)
    protected = owned / 'METADATA'
    protected.write_text('protected receipt')
    launcher = tmp_path / 'venv/bin/ndnsf-di'
    launcher.parent.mkdir()
    launcher.write_text('retain on conflict')
    with pytest.raises(RuntimeError, match='BASE_METADATA_OWNED_BY_SDK'):
        runtime.clean_application_metadata({'artifacts': {str(protected): 'frozen'}}, site)
    assert protected.exists() and launcher.read_text() == 'retain on conflict'


@pytest.mark.parametrize('name', runtime.FRAMEWORK_HEADERS)
@pytest.mark.parametrize('dangling', [False, True])
def test_framework_headers_rejected_including_dangling_links(tmp_path, name, dangling):
    path = tmp_path / 'include' / name
    path.parent.mkdir()
    if dangling:
        path.symlink_to(tmp_path / 'missing')
    else:
        path.mkdir()
        (path / 'old.hpp').write_text('old framework')
    with pytest.raises(RuntimeError, match='APPLICATION_HEADERS_IN_BASE'):
        runtime.verify_framework_headers([tmp_path])


def test_header_cleanup_preserves_external_sdk_and_receipt(tmp_path):
    import json
    owned = tmp_path / 'include/NDNSF-DistributedInference/cpp'
    owned.mkdir(parents=True)
    old = owned / 'NativeModelRunner.hpp'
    old.write_text('old framework')
    external = tmp_path / 'include/ndn-cxx/name.hpp'
    external.parent.mkdir()
    external.write_text('external sdk')
    receipt = tmp_path / 'dependency-sdk.json'
    receipt.write_text(json.dumps({'artifacts': {str(external): runtime.digest(external)}}))
    original = receipt.read_bytes()
    rows = runtime.clean_framework_headers(json.loads(original), [tmp_path])
    assert not owned.exists()
    assert any(row['path'] == str(old) and row['sha256'] for row in rows)
    assert receipt.read_bytes() == original and external.read_text() == 'external sdk'
    runtime.verify_framework_headers([tmp_path])


def test_header_sdk_conflict_rejected_before_any_removal(tmp_path):
    first = tmp_path / 'include/ndn-service-framework'
    second = tmp_path / 'include/NDNSF-DistributedInference'
    first.mkdir(parents=True)
    second.mkdir()
    (first / 'old.hpp').write_text('framework')
    protected = second / 'sdk.hpp'
    protected.write_text('sdk owned')
    with pytest.raises(RuntimeError, match='BASE_HEADER_OWNED_BY_SDK'):
        runtime.clean_framework_headers({'artifacts': {str(protected): 'frozen'}}, [tmp_path])
    assert (first / 'old.hpp').read_text() == 'framework'
    assert protected.read_text() == 'sdk owned'


def test_header_inventory_over_bound_removes_nothing(tmp_path):
    root = tmp_path / 'include/ndn-service-framework'
    root.mkdir(parents=True)
    for index in range(10000):
        (root / str(index)).mkdir()
    with pytest.raises(RuntimeError, match='BASE_HEADER_INVENTORY_OVER_BOUND'):
        runtime.clean_framework_headers({'artifacts': {}}, [tmp_path])
    assert root.is_dir() and len(list(root.iterdir())) == 10000


def test_header_repair_rejects_host_before_mutation(monkeypatch, tmp_path):
    monkeypatch.setattr(runtime.os, 'geteuid', lambda: 1234)
    with pytest.raises(RuntimeError, match='BASE_CONTAINER_ROOT_REQUIRED'):
        runtime.repair_framework_headers()
    assert not list(tmp_path.iterdir())


@pytest.mark.parametrize('wrong_parent', [False, True])
@pytest.mark.parametrize('filesystem', ['xfs', 'rootfs'])
@pytest.mark.parametrize('metadata_only', [False, True])
def test_header_repair_builder_binds_parent_and_bounded_calls(monkeypatch, tmp_path,
                                                            wrong_parent, filesystem, metadata_only):
    import json
    from types import SimpleNamespace

    parent = tmp_path / 'parent.sif'
    parent.write_bytes(b'original dependency sdk')
    lock = tmp_path / 'parent.lock.json'
    lock.write_text(json.dumps({'parentSha256': '0' * 64 if wrong_parent else builder.digest(parent),
                                'parentBytes': parent.stat().st_size}))
    work = tmp_path / 'work'
    work.mkdir()
    monkeypatch.setenv('NDNSF_DI_WORKSPACE', str(work))
    monkeypatch.setattr(builder.os, 'geteuid', lambda: 0)
    monkeypatch.setattr(builder.shutil, 'disk_usage', lambda p: SimpleNamespace(free=32 * 1024**3))
    apptainer = tmp_path / 'apptainer'
    apptainer.write_text('fake runtime')
    output = tmp_path / 'result'
    calls = []

    def check_output(argv, **kwargs):
        if argv[0] == 'findmnt':
            return filesystem + '\n'
        if argv[1:] == ['version']:
            return '1.5.3-3.el8\n'
        receipt = 'application-metadata-repair' if metadata_only else 'framework-header-repair'
        assert argv[-2:] == ['/bin/cat', '/opt/ndn-base/manifest/' + receipt + '.json']
        return json.dumps({'status': 'VERIFIED', 'scope': 'FRAMEWORK_HEADERS_ONLY',
                           'removed': [], 'dependencySdkSha256': 'a' * 64})

    def run(argv, **kwargs):
        calls.append(argv)
        if argv[1] == 'build':
            assert argv[argv.index('--mksquashfs-args') + 1] == '-processors 2 -mem 2G'
            assert '--fakeroot' not in argv and '--no-cleanup' in argv
            definition = Path(argv[-1]).read_text()
            action = 'repair-application-metadata' if metadata_only else 'repair-framework-headers'
            assert action in definition and '@REPAIR_ACTION@' not in definition
            assert 'dependency-sdk.py prepare' not in definition
            Path(argv[-2]).write_bytes(b'derived sdk')
        else:
            assert argv[-1] == 'verify' and '--cleanenv' in argv

    monkeypatch.setattr(builder.subprocess, 'check_output', check_output)
    monkeypatch.setattr(builder.subprocess, 'run', run)
    args = SimpleNamespace(base=parent, lock=lock, output=output, wheel=None,
                           dependency_bundle=None, apptainer=str(apptainer),
                           expected_apptainer='1.5.3', repair_framework_headers=not metadata_only,
                           repair_application_metadata=metadata_only)
    if wrong_parent:
        with pytest.raises(ValueError, match='BASE_PARENT_IDENTITY'):
            builder.build(args)
        assert not output.exists() and calls == []
        return
    if filesystem == 'rootfs':
        with pytest.raises(ValueError, match='BASE_REPAIR_REQUIRES_DISK'):
            builder.build(args)
        assert not output.exists() and calls == []
        return
    builder.build(args)
    record = json.loads((output / 'build-record.json').read_text())
    scope = 'BASE_APPLICATION_METADATA_REPAIR' if metadata_only else 'BASE_FRAMEWORK_HEADER_REPAIR'
    assert record['status'] == 'PASS' and record['scope'] == scope
    assert record['apptainerVersion'] == '1.5.3-3.el8'
    assert record['cleanup']['status'] == 'VERIFIED' and len(calls) == 3
    assert parent.read_bytes() == b'original dependency sdk'


def test_header_repair_builder_requires_root_namespace(monkeypatch):
    from types import SimpleNamespace
    monkeypatch.setattr(builder.os, 'geteuid', lambda: 1000)
    with pytest.raises(ValueError, match='BASE_REPAIR_ROOT_NAMESPACE_REQUIRED'):
        builder.repair_framework_headers(SimpleNamespace(dependency_bundle=None, wheel=None))


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
