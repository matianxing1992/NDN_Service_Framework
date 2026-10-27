"""Dependency-base failures must occur before compile/run probes."""
import importlib.util
import json
import sys
import subprocess
from types import SimpleNamespace
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
PATH = ROOT / 'Experiments/TigerCluster/adapters/slurm-apptainer/scripts/dependency-sdk.py'


@pytest.fixture
def sdk(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location('dependency_sdk', PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    manifest = tmp_path / 'dependency-sdk.json'
    monkeypatch.setattr(module, 'MANIFEST', manifest)
    monkeypatch.setattr(module, 'run', lambda *args, **kwargs: 'packages')
    def unexpected_probe(*args, **kwargs):
        pytest.fail('compiler probe started before invalid SDK was rejected')
    monkeypatch.setattr(module, 'check', unexpected_probe)
    return module, manifest


@pytest.mark.parametrize('mutation', ['changed', 'missing', 'package-drift'])
def test_dependency_identity_rejected_before_probe(sdk, tmp_path, mutation):
    module, manifest = sdk
    artifact = tmp_path / 'header.hpp'
    artifact.write_text('qualified header')
    body = {'schema': 'ndnsf-base-sdk-v1', 'artifacts': {str(artifact): module.digest(artifact)},
            'packages': 'packages'}
    if mutation == 'changed':
        artifact.write_text('different header')
    elif mutation == 'missing':
        artifact.unlink()
    else:
        body['packages'] = 'different packages'
    manifest.write_text(json.dumps(body))
    with pytest.raises((AssertionError, FileNotFoundError)):
        module.verify()


@pytest.mark.parametrize('loader_error', ['libdependency.so => not found',
                                        'undefined symbol: missing_symbol',
                                        'libdependency.so => /usr/local/lib/libdependency.so',
                                        'libdependency.so => /opt/ndnsf-di/current/lib/libdependency.so'])
def test_bad_dynamic_closure_rejected_before_probe(sdk, monkeypatch, loader_error):
    module, manifest = sdk
    manifest.write_text(json.dumps({'schema': 'ndnsf-base-sdk-v1', 'artifacts': {}, 'packages': 'packages'}))
    monkeypatch.setattr(module, 'run', lambda *args, **kwargs:
                        'packages' if args[0] == '/usr/bin/dpkg-query' else loader_error)
    with pytest.raises(AssertionError, match='SDK_(LINK_CLOSURE|WRONG_ORIGIN)'):
        module.verify()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


@pytest.mark.parametrize('change_parent', [False, True])
def test_sdk_dispatch_binds_parent_and_excludes_ndnsf_source(tmp_path, monkeypatch, change_parent):
    fixtures = load('sdk_handoff_fixtures', ROOT / 'tests/python/test_development_handoff.py')
    lock, workspaces, wheels, base = fixtures.inputs.__wrapped__(tmp_path)
    bundle = tmp_path / 'handoff'
    fixtures.handoff.prepare(lock, workspaces, wheels, bundle)
    builder = load('sdk_builder', PATH.with_name('build-base-sif.py'))
    monkeypatch.setattr(builder.shutil, 'disk_usage', lambda _: SimpleNamespace(free=100 * 1024**3))
    apptainer = tmp_path / 'apptainer'
    apptainer.write_text('''#!/usr/bin/python3
import json, pathlib, sys
if sys.argv[1] == 'version': print('1.5.3')
elif sys.argv[1] == 'build':
    definition = pathlib.Path(sys.argv[-1]).read_text()
    assert '/build-input/sdk' in definition and 'dependency-sdk.py prepare' in definition
    pathlib.Path(sys.argv[-2]).write_text('mock SIF, not a runtime acceptance')
elif sys.argv[1] == 'exec': print(json.dumps({'status':'PASS', 'scope':'BASE_DEPENDENCY_SDK'}))
else: sys.exit(2)
''')
    apptainer.chmod(0o755)
    args = SimpleNamespace(base=base, output=tmp_path / 'output', dependency_bundle=bundle,
                           apptainer=str(apptainer), expected_apptainer='1.5.3')
    if change_parent:
        base.write_text('changed parent')
        with pytest.raises(ValueError, match='SDK_PARENT_IDENTITY'):
            builder.build(args)
        assert not args.output.exists()
    else:
        builder.build(args)
        inputs = args.output / 'inputs/sdk'
        assert not (inputs / 'source/workspace.tar').exists()
        manifest = json.loads((inputs / 'inputs.json').read_text())
        assert set(manifest['repositories']) == {'nacAbe', 'ndnSvs', 'ndnSd'}
        assert (inputs / 'source/nacAbe.tar').is_file()
        assert json.loads((args.output / 'build-record.json').read_text())['parentSha256'] == builder.digest(base)
        template = (args.output / 'base.def').read_text()
        assert f'From: {base}' in template
        for heading in ['%post', '%test', '%runscript']:
            shell = template.split(heading + '\n', 1)[1].split('\n%', 1)[0]
            subprocess.run(['/bin/sh', '-n'], input=shell, text=True, check=True)


def test_tokenizer_completion_extracts_only_sealed_bridge(tmp_path):
    fixtures = load('tokenizer_handoff_fixtures', ROOT / 'tests/python/test_development_handoff.py')
    initial = fixtures.inputs.__wrapped__(tmp_path)
    workspace, base, sdk, native = fixtures.runtime_inputs(initial, tmp_path)
    bundle = tmp_path / 'bundle'
    fixtures.handoff.prepare_runtime(workspace, base, sdk, native, bundle)
    builder = load('tokenizer_builder', PATH.with_name('build-base-sif.py'))
    output = tmp_path / 'inputs'
    output.mkdir()
    builder.tokenizer_inputs(bundle, output, builder.digest(base))
    crate = output / 'tokenizer/crate'
    assert {str(p.relative_to(crate)) for p in crate.rglob('*') if p.is_file()} == {
        'Cargo.toml', 'Cargo.lock', 'src/lib.rs', 'tokenizer-abi.h'}
    assert not (output / 'tokenizer/source').exists()
    body = json.loads((output / 'tokenizer/inputs.json').read_text())
    assert len(body['files']) == 9
    for name, expected in body['files'].items():
        assert builder.digest(output / 'tokenizer' / name) == expected
    with pytest.raises(ValueError, match='TOKENIZER_PARENT_IDENTITY'):
        builder.tokenizer_inputs(bundle, tmp_path / 'wrong-parent', '0' * 64)
    assert not (tmp_path / 'wrong-parent').exists()


@pytest.fixture
def tokenizer_completion(tmp_path, monkeypatch):
    module = load('tokenizer_sdk_completion', PATH)
    inputs = tmp_path / 'inputs'
    names = {'crate/Cargo.toml', 'crate/Cargo.lock', 'crate/src/lib.rs',
             'crate/tokenizer-abi.h', 'probes/probe.cpp', 'probes/stable-probe.cpp',
             'probes/vectors.json', 'probes/stable-vectors.json'}
    for name in names:
        path = inputs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('frozen ' + name)
    native = {'rustVersion': '1.90.0', 'crate': {
        name: 'sha256:' + module.digest(inputs / 'crate' / name)
        for name in ('Cargo.toml', 'Cargo.lock')}}
    (inputs / 'native-inputs.json').write_text(json.dumps(native))
    files = {str(p.relative_to(inputs)): module.digest(p)
             for p in inputs.rglob('*') if p.is_file()}
    manifest = tmp_path / 'dependency-sdk.json'
    manifest.write_text(json.dumps({'schema': 'ndnsf-base-sdk-v1', 'artifacts': {},
                                   'inputs': {'files': {'native/native-inputs.json':
                                                       files['native-inputs.json']}},
                                   'packages': 'packages'}))
    (inputs / 'inputs.json').write_text(json.dumps({
        'files': files, 'sourceSealDigest': 'sha256:' + 'a' * 64,
        'baseSdkSha256': 'sha256:' + module.digest(manifest)}))
    monkeypatch.setattr(module, 'MANIFEST', manifest)
    monkeypatch.setattr(module, 'TOKENIZER_ARCHIVE', tmp_path / 'bridge.a')
    monkeypatch.setattr(module, 'TOKENIZER_PROVENANCE', tmp_path / 'provenance')
    monkeypatch.setattr(module.os, 'geteuid', lambda: 0)
    original_is_dir = Path.is_dir
    monkeypatch.setattr(Path, 'is_dir', lambda path:
                        True if str(path) == '/.singularity.d' else original_is_dir(path))
    monkeypatch.setattr(module, 'verify', lambda: None)
    monkeypatch.setattr(module, 'run', lambda *args, **kwargs:
                        ('rustc' if 'rustc' in args[0] else 'cargo') + ' 1.90.0 (pinned)')
    return module, inputs, manifest


@pytest.mark.parametrize('name', ['Cargo.toml', 'Cargo.lock', 'src/lib.rs', 'tokenizer-abi.h'])
@pytest.mark.parametrize('mutation', ['missing', 'changed'])
def test_tokenizer_source_failure_precedes_compile(tokenizer_completion, monkeypatch, name, mutation):
    module, inputs, manifest = tokenizer_completion
    original = manifest.read_bytes()
    path = inputs / 'crate' / name
    if mutation == 'missing':
        path.unlink()
    else:
        path.write_text('changed')
    monkeypatch.setattr(module, 'check', lambda *args, **kwargs:
                        pytest.fail('compiler started before source admission'))
    with pytest.raises(ValueError, match='SDK_TOKENIZER_INPUT_CHANGED'):
        module.complete_tokenizer(inputs)
    assert manifest.read_bytes() == original and not module.TOKENIZER_ARCHIVE.exists()


def test_tokenizer_existing_archive_is_preserved(tokenizer_completion, monkeypatch):
    module, inputs, manifest = tokenizer_completion
    original = manifest.read_bytes()
    module.TOKENIZER_ARCHIVE.write_bytes(b'incompatible existing archive')
    monkeypatch.setattr(module, 'check', lambda *args, **kwargs:
                        pytest.fail('compiler started before output conflict admission'))
    with pytest.raises(ValueError, match='SDK_TOKENIZER_OUTPUT_EXISTS'):
        module.complete_tokenizer(inputs)
    assert module.TOKENIZER_ARCHIVE.read_bytes() == b'incompatible existing archive'
    assert manifest.read_bytes() == original


def test_tokenizer_offline_compile_failure_leaves_sdk_unchanged(tokenizer_completion, monkeypatch):
    module, inputs, manifest = tokenizer_completion
    original = manifest.read_bytes()
    def cargo_failure(*args, **kwargs):
        assert args[0] == '/opt/rust/bin/cargo'
        assert '--offline' in args and '--locked' in args
        assert args[args.index('--jobs') + 1] == '2'
        assert kwargs['env']['CARGO_HOME'] == '/opt/cargo-home'
        raise subprocess.CalledProcessError(101, args)
    monkeypatch.setattr(module, 'check', cargo_failure)
    with pytest.raises(subprocess.CalledProcessError):
        module.complete_tokenizer(inputs)
    assert manifest.read_bytes() == original and not module.TOKENIZER_ARCHIVE.exists()


@pytest.fixture
def nac_inputs(tmp_path):
    fixtures = load('nac_handoff_fixtures', ROOT / 'tests/python/test_development_handoff.py')
    lock, workspaces, wheels, base = fixtures.inputs.__wrapped__(tmp_path)
    bundle = tmp_path / 'handoff'
    fixtures.handoff.prepare(lock, workspaces, wheels, bundle)
    builder = load('nac_builder', PATH.with_name('build-base-sif.py'))
    output = tmp_path / 'inputs'
    builder.nac_inputs(bundle, output, builder.digest(base))
    repository = json.loads(lock.read_text())['repositories']['nacAbe']
    module = load('nac_sdk', PATH)
    archive_sha = module.digest(bundle / 'source/nacAbe.tar')
    return module, output / 'nac', repository, archive_sha


def test_nac_closure_uses_existing_patch_validator_and_only_locked_inputs(nac_inputs):
    module, inputs, repository, archive_sha = nac_inputs
    frozen, helper, source, patches = module.nac_contract(inputs, repository, archive_sha)
    assert len(patches) == 5 and source['commit'] == repository['revision']
    assert len(helper.load_lock(inputs / 'project/packaging/host-dependencies.lock.json')['sources']) == 5
    assert any(name.startswith('project/patches/openabe/') for name in frozen['files'])
    assert not (inputs / 'project/ndn-service-framework').exists()
    assert not list(inputs.rglob('*.pyc'))


@pytest.mark.parametrize('mutation', ['patch', 'missing', 'symlink', 'archive', 'revision', 'extra'])
def test_nac_rejects_changed_or_unbound_inputs_before_build(nac_inputs, tmp_path, mutation):
    module, inputs, repository, archive_sha = nac_inputs
    patch = next((inputs / 'project/patches/nac-abe').glob('*.patch'))
    if mutation == 'patch':
        patch.write_bytes(b'changed patch')
    elif mutation == 'missing':
        patch.unlink()
    elif mutation == 'symlink':
        outside = tmp_path / 'patch'
        patch.rename(outside)
        patch.symlink_to(outside)
    elif mutation == 'archive':
        archive_sha = '0' * 64
    elif mutation == 'revision':
        repository['revision'] = '0' * 40
    else:
        frozen = json.loads((inputs / 'inputs.json').read_text())
        frozen['files']['unexpected.cpp'] = '0' * 64
        (inputs / 'inputs.json').write_text(json.dumps(frozen))
    with pytest.raises(ValueError, match='SDK_NAC_'):
        module.nac_contract(inputs, repository, archive_sha)


def test_nac_native_suites_receive_isolated_schema_and_selected_library(tmp_path, monkeypatch):
    module = load('nac_native_suite_runner', PATH)
    prefix = tmp_path / 'prefix'
    schema = tmp_path / 'trust-schema.conf'
    schema.write_text('frozen schema')
    executable = tmp_path / 'unit-tests'
    monkeypatch.setattr(module, 'run', lambda *a, **k:
                        'libnac-abe.so => ' + str(prefix / 'lib/libnac-abe.so'))
    seen = []
    def check_suite(*args, **kwargs):
        cwd = Path(kwargs['cwd'])
        assert cwd != schema.parent and (cwd / schema.name).read_bytes() == schema.read_bytes()
        assert kwargs['env']['HOME'] == str(cwd)
        assert kwargs['env']['LD_LIBRARY_PATH'].split(':')[0] == str(prefix / 'lib')
        seen.append(args[1])
    monkeypatch.setattr(module, 'check', check_suite)
    module.nac_native_tests(executable, prefix, schema)
    assert seen == ['--run_test=' + suite for suite in module.NAC_SUITES]
    monkeypatch.setattr(module, 'run', lambda *a, **k:
                        'libnac-abe.so => /wrong/libnac-abe.so')
    with pytest.raises(ValueError, match='SDK_NAC_TEST_LIBRARY_ORIGIN'):
        module.nac_native_tests(executable, prefix, schema)
