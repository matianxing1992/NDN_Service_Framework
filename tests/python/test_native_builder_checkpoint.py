"""Packing recovery checks; synthetic files are never inference/build evidence."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'Experiments/TigerCluster/adapters/slurm-apptainer/scripts'
SPEC = importlib.util.spec_from_file_location('builder_checkpoint', SCRIPTS / 'native-builder-checkpoint.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def sha(path):
    return 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, body):
    path.write_text(json.dumps(body, indent=2, sort_keys=True) + '\n')


@pytest.fixture
def retained(tmp_path):
    root = tmp_path / 'checkpoint'
    root.mkdir(mode=0o700)
    (root / '.active.lock').touch(mode=0o600)
    stage = root / 'rootfs/opt/ndnsf-stage'
    (stage / 'manifest').mkdir(parents=True)
    (stage / 'bin').mkdir()
    (stage / 'python/package').mkdir(parents=True)
    (stage / 'bin/fixture').write_bytes(b'synthetic-native-payload')
    (stage / 'bin/alias').symlink_to('fixture')
    (stage / 'python/package/__init__.py').write_bytes(b'# synthetic python payload\n')
    source = tmp_path / 'source-seal.json'
    source.write_text('{"sealDigest":"synthetic-unit-input"}\n')
    (stage / 'manifest/source-seal.json').write_bytes(source.read_bytes())
    for name in ('dependency-lock.json', 'container-configure-closure.json'):
        write_json(stage / 'manifest' / name, {'unitTestOnly': True})
    write_json(stage / 'manifest/waf-install.json', {
        'bin/fixture': {'sha256': sha(stage / 'bin/fixture')[7:]},
        'bin/alias': {'symlink': 'fixture'}})
    base = tmp_path / 'base.sif'
    base.write_bytes(b'synthetic dependency base')
    sdk = 'sha256:' + 'a' * 64
    native = dict(schemaVersion='spec170-container-native-build-v1', status='PASS',
                  buildBoundary='container-runtime-in-sif', runtimeLayout='installed-v1',
                  baseSifSha256=sha(base), baseSdkSha256=sdk,
                  artifacts=[{'builderPath': '/opt/ndnsf-stage/bin/fixture',
                              'bytes': (stage / 'bin/fixture').stat().st_size,
                              'sha256': sha(stage / 'bin/fixture')}],
                  repositoryPythonFiles={'package/__init__.py': {
                      'sha256': sha(stage / 'python/package/__init__.py'),
                      'mode': (stage / 'python/package/__init__.py').stat().st_mode & 0o7777}})
    for field, name in (('sourceSealSha256', 'source-seal.json'),
                        ('dependencyLockSha256', 'dependency-lock.json'),
                        ('configureClosureSha256', 'container-configure-closure.json'),
                        ('wafInstallSha256', 'waf-install.json')):
        native[field] = sha(stage / 'manifest' / name)
    write_json(stage / 'manifest/container-native-build.json', native)
    template = ROOT / 'Experiments/TigerCluster/adapters/slurm-apptainer/templates/development-runtime.def.in'
    definition = tmp_path / 'original.def'
    definition.write_text(template.read_text().replace('@BASE_SIF@', str(base)))
    (root / 'original-runtime.def').write_bytes(definition.read_bytes())
    (root / 'original-build.log').write_text(
        '+ PYTHONPATH=/opt/ndnsf-stage/python /opt/venv/bin/python '
        '/opt/ndnsf-stage/manifest/verify-native.py builder\n'
        '+ rm -rf /src/ndnsf /src/ndn-svs /src/nac-abe /src/ndn-sd '
        '/build-input /opt/ndnsf-build-work\nINFO:    Extracting local image...\n')
    rows = MODULE.snapshot(stage)
    write_json(root / 'stage-tree.json', rows)
    receipt = root / 'retention-2.json'
    write_json(receipt, dict(status='RETAINED_VERIFIED_COPY_PENDING_ADMISSION',
        scope='RETAINED_CONTAINER_STAGE_NOT_YET_CHECKPOINT_ADMISSION', priorBuildJob='1', job='2',
        destination=str(root / 'rootfs'), fileCount=len(rows),
        regularFileBytes=sum(row.get('bytes', 0) for row in rows.values()),
        stageTreeSha256=sha(root / 'stage-tree.json')[7:],
        originalDefinitionSha256=sha(definition)[7:],
        originalBuildLogSha256=sha(root / 'original-build.log')[7:],
        nativeManifestSha256=sha(stage / 'manifest/container-native-build.json')[7:],
        sourceSeal=sha(source), baseSif=sha(base), baseSdk=sdk))
    return dict(root=root, stage=stage, receipt=receipt, definition=definition, source=source,
                sdk=sdk, base=base, derived=tmp_path / 'derived.def', record=tmp_path / 'checkpoint.json')


def command(case, operation):
    return ['python3', str(SCRIPTS / 'native-builder-checkpoint.py'), operation,
            '--checkpoint-root', str(case['root']), '--retention-record', str(case['receipt']),
            '--definition', str(case['definition']), '--source-seal', str(case['source']),
            '--base-sdk-sha256', case['sdk'], '--derived-definition', str(case['derived']),
            '--record', str(case['record'])]


def prepare(case):
    result = subprocess.run(command(case, 'prepare'), capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_two_stage_recovery_preserves_final_and_original_proof(retained):
    boundary = prepare(retained)
    derived = retained['derived'].read_text()
    original = retained['definition'].read_text()
    assert derived.split('Stage: builder\n\n', 1)[1] == original[original.index('Bootstrap:', 1):]
    assert '%post' not in derived.split('Stage: final', 1)[0]
    assert boundary['baseImage'] == str(retained['base'])
    assert boundary['hostBinaryInputs'] == []
    assert boundary['nativeCompilation'] == 'reused-container-builder'
    result = subprocess.run(command(retained, 'verify') + ['--expected-record-sha256',
                            sha(retained['record'])], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    # The normal entry point retains its same-base/fresh-compilation contract.
    with pytest.raises(Exception, match='BASE_IDENTITY_MISMATCH'):
        MODULE.normal_boundary(retained['derived'])


@pytest.mark.parametrize('change', ['bytes', 'mode', 'link', 'source', 'base', 'sdk',
                                    'completion', 'host-payload', 'final', 'record'])
def test_recovery_rejects_changed_inputs_after_prepare(retained, change):
    prepare(retained)
    record_sha = sha(retained['record'])
    if change == 'bytes':
        (retained['stage'] / 'bin/fixture').write_bytes(b'changed payload')
    elif change == 'mode':
        (retained['stage'] / 'bin/fixture').chmod(0o777)
    elif change == 'link':
        path = retained['stage'] / 'bin/alias'
        path.unlink()
        path.symlink_to('/etc/passwd')
    elif change == 'source':
        retained['source'].write_text('{}')
    elif change == 'base':
        retained['base'].write_bytes(b'wrong dependency base')
    elif change == 'sdk':
        retained['sdk'] = 'sha256:' + 'b' * 64
    elif change == 'completion':
        (retained['root'] / 'original-build.log').unlink()
    elif change == 'host-payload':
        (retained['root'] / 'rootfs/host.so').write_bytes(b'arbitrary host input')
    elif change == 'final':
        retained['derived'].write_text(retained['derived'].read_text().replace('Stage: final',
                                                                            'Stage: changed'))
    else:
        retained['record'].write_text('{}')
    result = subprocess.run(command(retained, 'verify') + ['--expected-record-sha256', record_sha],
                            capture_output=True, text=True)
    assert result.returncode != 0, result.stdout
    assert 'BUILDER_CHECKPOINT_' in result.stderr or 'No such file' in result.stderr


@pytest.mark.parametrize('flags', [
    ['--builder-checkpoint', '/missing'],
    ['--builder-checkpoint-retention', '/missing'],
    ['--builder-checkpoint-sdk-sha256', 'sha256:' + 'a' * 64],
    ['--builder-checkpoint', '/missing', '--builder-checkpoint-retention', '/missing',
     '--builder-checkpoint-sdk-sha256', 'sha256:' + 'a' * 64,
     '--resume-final-rootfs', '/missing'],
])
def test_incomplete_or_conflicting_recovery_flags_fail_before_execution(flags):
    result = subprocess.run(['bash', str(SCRIPTS / 'build-local-sif.sh'), '--build-only', *flags],
                            capture_output=True, text=True)
    assert result.returncode == 2
    assert 'usage:' in result.stderr
