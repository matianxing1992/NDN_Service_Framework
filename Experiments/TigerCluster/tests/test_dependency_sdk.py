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
