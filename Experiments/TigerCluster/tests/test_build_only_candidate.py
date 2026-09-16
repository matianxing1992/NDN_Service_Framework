"""Exercise the real build driver with a fake Apptainer, never a runtime PASS."""
import importlib.util
import json
from pathlib import Path
import subprocess
import shutil
import sys

import pytest

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / 'Experiments/TigerCluster/adapters/slurm-apptainer/scripts'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    sys.modules[name] = obj
    spec.loader.exec_module(obj)
    return obj


@pytest.mark.parametrize('without_legacy', [False, True])
@pytest.mark.parametrize('resume', [False, True])
def test_build_only_record_cannot_be_released(tmp_path, without_legacy, resume):
    fixture = module('build_record_fixture', ROOT / 'tests/python/test_build_local_sif_record.py')
    source = fixture.write_source_seal(tmp_path)
    seal = json.loads(source.read_text())
    base = tmp_path / 'base.sif'
    base.write_bytes(b'unit base')
    text = (SCRIPTS.parent / 'templates/development-runtime.def.in').read_text()
    for key, value in {'@BUNDLE@': str(tmp_path), '@BASE_SIF@': str(base),
                       '@RELEASE@': 'unit-build-only', '@SEAL_DIGEST@': seal['sealDigest'],
                       '@NATIVE_DIGEST@': 'sha256:' + 'b' * 64}.items():
        text = text.replace(key, value)
    definition = tmp_path / 'candidate.def'
    definition.write_text(text)
    apptainer = tmp_path / 'apptainer'
    apptainer.write_text('''#!/usr/bin/python3
import json, pathlib, shutil, sys
if sys.argv[1] == 'version': print('1.5.3')
elif sys.argv[1] == 'exec':
    # The verifier writes its manifest; a read-only SIF needs a disposable overlay.
    if '/opt/ndnsf-di/current/manifest/verify-native.py' in sys.argv:
        assert '--writable-tmpfs' in sys.argv
        assert '--no-mount' in sys.argv
        assert 'home,cwd,hostfs,bind-paths' in sys.argv
elif sys.argv[1] == 'build':
    source = pathlib.Path(sys.argv[-1])
    if source.is_dir():
        assert '--fakeroot' in sys.argv  # Preserve container users and private directories.
        source = source / '.singularity.d/Singularity'
    shutil.copyfile(source, sys.argv[-2])
elif sys.argv[1] == 'inspect':
    labels = {}
    active = False
    for raw in pathlib.Path(sys.argv[-1]).read_text().splitlines():
        line = raw.strip()
        if line.startswith('%'): active = line == '%labels'
        elif active and line and not line.startswith('#'):
            key, value = line.split(None, 1)
            labels[key] = value
    print(json.dumps({'data': {'attributes': {'labels': labels}}}))
else: sys.exit(97)
''')
    apptainer.chmod(0o755)
    sif, record = tmp_path / 'candidate.sif', tmp_path / 'build.json'
    entry = SCRIPTS / 'build-local-sif.sh'
    if without_legacy:
        isolated = tmp_path / 'isolated/Experiments/TigerCluster/adapters/slurm-apptainer/scripts'
        isolated.mkdir(parents=True)
        for name in ['build-local-sif.sh', 'validate-local-sif-source.py']:
            shutil.copy2(SCRIPTS / name, isolated / name)
        lib = isolated.parents[2] / 'lib'
        lib.mkdir()
        shutil.copy2(ROOT / 'packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py', lib)
        entry = isolated / 'build-local-sif.sh'
    args = [str(entry), '--definition', str(definition),
            '--sif', str(sif), '--record', str(record), '--source-seal', str(source),
            '--apptainer', str(apptainer), '--expected-apptainer', '1.5.3']
    if resume:
        rootfs = tmp_path / 'final-rootfs'
        (rootfs / '.singularity.d').mkdir(parents=True)
        (rootfs / 'opt/ndnsf-di/replay').mkdir(parents=True)
        shutil.copyfile(definition, rootfs / '.singularity.d/Singularity')
        shutil.copyfile(source, rootfs / 'opt/ndnsf-di/replay/source-seal.json')
        args += ['--resume-final-rootfs', str(rootfs)]
        (rootfs / '.singularity.d/Singularity').write_text('wrong definition')
        rejected = subprocess.run(args + ['--build-only'], text=True, capture_output=True)
        assert rejected.returncode != 0 and not sif.exists() and not record.exists()
        shutil.copyfile(definition, rootfs / '.singularity.d/Singularity')
        (rootfs / 'opt/ndnsf-di/replay/source-seal.json').write_text('{}')
        rejected = subprocess.run(args + ['--build-only'], text=True, capture_output=True)
        assert rejected.returncode != 0 and not sif.exists() and not record.exists()
        shutil.copyfile(source, rootfs / 'opt/ndnsf-di/replay/source-seal.json')
    # The ordinary release path still requires a host gate.
    rejected = subprocess.run(args, text=True, capture_output=True)
    assert rejected.returncode != 0 and not sif.exists() and not record.exists()
    result = subprocess.run(args + ['--build-only'], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    value = json.loads(record.read_text())
    assert value['status'] == 'BUILT_UNQUALIFIED'
    if resume:
        assert value['buildInput']['method'] == 'local-apptainer-definition-final-rootfs-resume'
    assert value['tigerAction'] == 'NOT_AUTHORIZED'
    for field in ['hostGate', 'spec175InputPreflight', 'spec175Preflight']:
        assert value[field]['status'] == 'NOT_RUN'
    assert value['containerNativeBuild']['status'] == 'PASS'
    validator = module('unqualified_build_record', SCRIPTS / 'validate-local-sif-build-record.py')
    with pytest.raises(validator.BuildRecordError, match='BUILD_RECORD_NOT_PASS'):
        validator.validate(record, sif, value['sif']['sha256'])
