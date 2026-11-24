"""Exercise the real build driver with a fake Apptainer, never a runtime PASS."""
import hashlib
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
@pytest.mark.parametrize('damage', ['', 'tampered', 'missing', 'sdk-tampered'])
def test_build_only_record_cannot_be_released(tmp_path, without_legacy, resume, damage):
    fixture = module('build_record_fixture', ROOT / 'tests/python/test_build_local_sif_record.py')
    source = fixture.write_source_seal(tmp_path)
    seal = json.loads(source.read_text())
    base = tmp_path / 'base.sif'
    base.write_bytes(b'unit base')
    text = (SCRIPTS.parent / 'templates/development-runtime.def.in').read_text()
    for key, value in {'@BUNDLE@': str(tmp_path), '@BASE_SIF@': str(base),
                       '@RELEASE@': 'unit-build-only', '@SEAL_DIGEST@': seal['sealDigest'],
                       '@NATIVE_DIGEST@': 'sha256:' + 'b' * 64,
                       '@BASE_DIGEST@': 'sha256:' + 'c' * 64,
                       '@BASE_SDK_DIGEST@': 'sha256:' + 'd' * 64,
                       '@LOCK_DIGEST@': 'sha256:' + 'e' * 64,
                       '@RUNTIME_PROFILE@': 'di-runtime'}.items():
        text = text.replace(key, value)
    definition = tmp_path / 'candidate.def'
    definition.write_text(text)
    prefix = tmp_path / 'payload/opt/ndnsf-di/current'
    (prefix / 'manifest').mkdir(parents=True)
    ledger_path = prefix / 'share/ndnsf/stack-install-manifest.json'
    ledger_path.parent.mkdir(parents=True)
    sdk = tmp_path / 'payload/opt/ndn-base/manifest/dependency-sdk.json'
    sdk.parent.mkdir(parents=True)
    sdk_file = tmp_path / 'payload/opt/onnxruntime/lib/libonnxruntime.so.1'
    sdk_file.parent.mkdir(parents=True)
    sdk_file.write_bytes(b'fixture SDK library')
    sdk_link = sdk_file.with_name('libonnxruntime.so')
    sdk_link.symlink_to(sdk_file.name)
    sdk_digest = hashlib.sha256(sdk_file.read_bytes()).hexdigest()
    sdk_path = '/opt/onnxruntime/lib/'
    sdk.write_text(json.dumps({
        'schema': 'ndnsf-base-sdk-v1',
        'packages': 'fixture=1\n',
        'artifacts': {sdk_path + p.name: sdk_digest for p in (sdk_file, sdk_link)},
        'onnxRuntimeCpp': {'symlinks': {sdk_path + sdk_link.name: sdk_file.name}},
    }))
    manifest = prefix / 'manifest/container-native-build.json'
    artifact = prefix / 'bin/DI_NativeRequester'
    artifact.parent.mkdir()
    artifact.write_bytes(b'fixture')
    bindings = {
        key: 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()
        for key, path in [('sourceSealSha256', source), ('baseSifSha256', base),
                          ('baseSdkSha256', sdk)]
    }
    runtime_path = '/opt/ndnsf-di/current/bin/DI_NativeRequester'
    manifest.write_text(json.dumps({
        **bindings, 'finalVerification': 'PASS',
        'artifacts': [{'runtimePath': runtime_path}],
    }))
    ledger_path.write_text(json.dumps({
        **bindings, 'schema': 1,
        'containerNativeBuildSha256': 'sha256:' + hashlib.sha256(manifest.read_bytes()).hexdigest(),
        'files': {runtime_path: {'installed': {
            'kind': 'file', 'mode': artifact.stat().st_mode & 0o777,
            'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
        }}},
    }))
    if damage == 'tampered':
        artifact.write_bytes(b'tampered fixture')
    elif damage == 'missing':
        artifact.unlink()
    elif damage == 'sdk-tampered':
        sdk_file.write_bytes(b'tampered SDK library')
    apptainer = tmp_path / 'apptainer'
    apptainer.write_text('''#!/usr/bin/python3
import json, os, pathlib, shutil, sys
if sys.argv[1] == 'version': print('1.5.3')
elif sys.argv[1] == 'exec':
    assert '--writable-tmpfs' not in sys.argv
    if '--home' in sys.argv:
        assert 'cwd,hostfs,bind-paths' in sys.argv
        home_idx = sys.argv.index('--home')
        host_home, inside_home = sys.argv[home_idx + 1].split(':')
        assert inside_home == '/home/ndnsf-smoke'
        assert pathlib.Path(host_home).is_dir()
        command, *args = sys.argv[home_idx + 3:]
    else:
        assert 'home,cwd,hostfs,bind-paths' in sys.argv
        command, *args = sys.argv[7:]
    payload = pathlib.Path(__file__).parent / 'payload'
    if command == '/bin/cat':
        sys.stdout.buffer.write((payload / args[0].lstrip('/')).read_bytes())
    elif command == '/usr/bin/dpkg-query':
        assert args == ['-W', r'-f=${Package}=${Version}\\n']
        print('fixture=1')
    elif command == '/bin/tar':
        args[1] = str(payload)
        os.execvp('tar', ['tar', *args])
    else:
        assert '--home' in sys.argv
        assert command in ('/opt/ndnsf-di/current/bin/DI_NativeRequester',
                           '/opt/ndnsf-di/current/bin/DI_NativeArtifactAuthority')
        assert args == ['--help']
        with (payload.parent / 'native-started').open('a') as log:
            print(pathlib.Path(command).name, file=log)
elif sys.argv[1] == 'build':
    assert sys.argv[sys.argv.index('--mksquashfs-args') + 1] == '-processors 2 -mem 2G'
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
        (rootfs / 'opt/ndnsf-di/current/manifest').mkdir(parents=True)
        shutil.copyfile(definition, rootfs / '.singularity.d/Singularity')
        shutil.copyfile(source, rootfs / 'opt/ndnsf-di/current/manifest/source-seal.json')
        args += ['--resume-final-rootfs', str(rootfs)]
        (rootfs / '.singularity.d/Singularity').write_text('wrong definition')
        rejected = subprocess.run(args + ['--build-only'], text=True, capture_output=True)
        assert rejected.returncode != 0 and not sif.exists() and not record.exists()
        shutil.copyfile(definition, rootfs / '.singularity.d/Singularity')
        (rootfs / 'opt/ndnsf-di/current/manifest/source-seal.json').write_text('{}')
        rejected = subprocess.run(args + ['--build-only'], text=True, capture_output=True)
        assert rejected.returncode != 0 and not sif.exists() and not record.exists()
        shutil.copyfile(source, rootfs / 'opt/ndnsf-di/current/manifest/source-seal.json')
    # The ordinary release path still requires a host gate.
    rejected = subprocess.run(args, text=True, capture_output=True)
    assert rejected.returncode != 0 and not sif.exists() and not record.exists()
    result = subprocess.run(args + ['--build-only'], text=True, capture_output=True)
    if damage:
        assert result.returncode != 0 and not sif.exists() and not record.exists()
        assert 'LOCAL_SIF_INSTALLED_PAYLOAD_INVALID' in result.stderr
        assert not (tmp_path / 'native-started').exists()
        return
    assert result.returncode == 0, result.stdout + result.stderr
    assert (tmp_path / 'native-started').read_text().splitlines() == [
        'DI_NativeRequester', 'DI_NativeArtifactAuthority',
    ]
    value = json.loads(record.read_text())
    assert value['status'] == 'BUILT_UNQUALIFIED'
    assert value['buildInput']['mksquashfsArgs'] == '-processors 2 -mem 2G'
    if resume:
        assert value['buildInput']['method'] == 'local-apptainer-definition-final-rootfs-resume'
    assert value['tigerAction'] == 'NOT_AUTHORIZED'
    for field in ['hostGate', 'spec175InputPreflight', 'spec175Preflight']:
        assert value[field]['status'] == 'NOT_RUN'
    assert value['containerNativeBuild']['status'] == 'PASS'
    validator = module('unqualified_build_record', SCRIPTS / 'validate-local-sif-build-record.py')
    with pytest.raises(validator.BuildRecordError, match='BUILD_RECORD_NOT_PASS'):
        validator.validate(record, sif, value['sif']['sha256'])


def test_staged_ops_launcher_survives_isolated_reexec(tmp_path):
    stage = tmp_path / 'stage'
    site = stage / 'python'
    ops = site / 'ndnsf_distributed_inference/ops'
    ops.mkdir(parents=True)
    (ops / 'cli.py').write_text(
        "import sys\n"
        "def main():\n"
        "    assert sys.flags.isolated\n"
        "    assert sys.argv[1:] == ['cluster', '--help']\n"
        "    print('ISOLATED_OPS_CLI')\n"
    )
    shutil.copy2(ROOT / 'NDNSF-DistributedInference/ndnsf_di_launcher.py', site)
    entry = stage / 'bin/ndnsf-di'
    entry.parent.mkdir()
    entry.write_text("from ndnsf_di_launcher import main\nraise SystemExit(main())\n")

    template = (SCRIPTS.parent / 'templates/development-runtime.def.in').read_text()
    start = '    ops_smoke=/opt/ndnsf-build-work/ops-smoke\n'
    fragment = start + template.split(start, 1)[1].split('\n    unset TMPDIR', 1)[0]
    fragment = fragment.replace('/opt/ndnsf-build-work', str(tmp_path))
    fragment = fragment.replace('/opt/ndnsf-stage', str(stage))
    fragment = fragment.replace('/opt/venv/bin/python', sys.executable)
    result = subprocess.run(['bash', '-eu', '-c', fragment], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == 'ISOLATED_OPS_CLI'
