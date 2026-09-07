"""Real bounded child processes with a fake container, never SIF qualification."""
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_operator import provision_run, OperatorError
from runtime.yolo_bundle import freeze_harness
from test_yolo_bundle import fixture_manifest
from test_yolo_public_inventory import prepared_public, seal


def sha(path):
    return 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(tmp_path, behavior='success'):
    manifest, harness_digest = fixture_manifest(tmp_path / 'source')
    bundle = tmp_path / 'bundle'
    freeze_harness(manifest, bundle, expected_manifest_sha256=harness_digest)
    roots = {name: tmp_path / name for name in ('inputs', 'package', 'public', 'private', 'output')}
    for root in roots.values():
        root.mkdir(mode=0o700)
    inputs = roots['inputs']
    (inputs / 'trust/contracts').mkdir(parents=True)
    (inputs / 'private').mkdir(mode=0o700)
    secret = inputs / 'private/artifact-policy-authority.key'
    secret.write_bytes(b'fake private key; not a real credential')
    secret.chmod(0o600)
    for path in (inputs / 'template.json', inputs / 'trust/contracts/trust-root-registry-v1.json',
                 roots['package'] / 'manifest.json'):
        path.write_text('{}')
    produced = tmp_path / 'produced'
    plan = prepared_public(produced)
    plan['schema'] = 'tiger-yolo-run-plan-v1'
    desc = dict(schema='tiger-yolo-prepare-input-v1', plan=plan,
                templateDigest=sha(inputs / 'template.json'),
                registryDigest=sha(inputs / 'trust/contracts/trust-root-registry-v1.json'),
                manifestDigest=sha(roots['package'] / 'manifest.json'),
                protectionEpoch='epoch-1', candidateId='shared-backbone-two-shard-v1',
                candidateDigest='sha256:' + 'a' * 64)
    descriptor = inputs / 'prepare.json'
    descriptor.write_text(json.dumps(desc))
    seal(produced, plan)
    receipt_path = produced / 'preparation.json'
    receipt = json.loads(receipt_path.read_text())
    receipt.update(templateDigest=desc['templateDigest'], registryDigest=desc['registryDigest'],
                   packageManifestDigest=desc['manifestDigest'], protectionEpoch=desc['protectionEpoch'])
    receipt_path.write_text(json.dumps(receipt))
    if behavior == 'wrong-epoch':
        receipt['protectionEpoch'] = 'other'
        receipt_path.write_text(json.dumps(receipt))
    if behavior == 'tampered':
        (produced / 'case.json').write_text('changed after receipt')
    launcher = tmp_path / 'fake-apptainer'
    launcher.write_text('#!/usr/bin/python3\nimport sys, json, shutil, time\n'
        'from pathlib import Path\n'
        f'behavior={behavior!r}\n'
        "args=sys.argv[1:]\nassert '--nv' not in args\n"
        "assert args[args.index('--pwd')+1] == '/bundle'\n"
        "assert args[-1].startswith('sha256:')\n"
        "print('PREPARATION_CHILD_STARTED', flush=True)\n"
        "if behavior == 'exit': sys.exit(7)\n"
        "if behavior == 'timeout': time.sleep(10)\n"
        "if behavior != 'missing':\n"
        "    public=next(a.rsplit(':',2)[0] for a in args if a.endswith(':/config:rw'))\n"
        f"    shutil.copytree({str(produced)!r}, public, dirs_exist_ok=True)\n")
    launcher.chmod(0o700)
    sif = tmp_path / 'fixture.sif'
    sif.write_bytes(b'fake image: never a real SIF')
    return dict(runtime_profile={'apptainer': str(launcher), 'sif': str(sif), 'sifSha256': sha(sif)[7:]},
                bundle=bundle, harness_digest=harness_digest,
                descriptor_digest=sha(descriptor), **roots,
                seconds=0.2 if behavior == 'timeout' else 3, cleanup_seconds=1)


def test_provision_invokes_child_and_validates_actual_receipt(tmp_path):
    kwargs = fixture(tmp_path)
    result = provision_run(**kwargs)
    assert result['status'] == 'PREPARED'
    assert result['qualification'] == 'NOT_EVALUATED'
    assert result['receiptDigest'] == sha(kwargs['public'] / 'preparation.json')
    cleanup = json.loads((kwargs['output'] / 'cleanup.json').read_text())['records']
    assert len(cleanup) == 1 and cleanup[0]['reaped'] and cleanup[0]['exitCode'] == 0
    assert not cleanup[0]['forced']
    with pytest.raises(ValueError, match='NOT_EMPTY'):
        provision_run(**kwargs)


@pytest.mark.parametrize('behavior,reason', [('exit', 'OPERATOR_PREPARATION_EXIT:7'),
    ('timeout', 'OPERATOR_PREPARATION_TIMEOUT'), ('wrong-epoch', 'OPERATOR_PREPARATION_RECEIPT_INPUTS')])
def test_provision_failure_retains_logs_and_reaps_child(tmp_path, behavior, reason):
    kwargs = fixture(tmp_path, behavior)
    with pytest.raises(OperatorError, match=reason):
        provision_run(**kwargs)
    assert (kwargs['output'] / 'logs/prepare.log').is_file()
    assert all(r['reaped'] for r in json.loads((kwargs['output'] / 'cleanup.json').read_text())['records'])


@pytest.mark.parametrize('behavior', ['missing', 'tampered'])
def test_zero_exit_does_not_replace_preparation_evidence(tmp_path, behavior):
    kwargs = fixture(tmp_path, behavior)
    with pytest.raises((ValueError, OSError)):
        provision_run(**kwargs)
    records = json.loads((kwargs['output'] / 'cleanup.json').read_text())['records']
    assert records[0]['exitCode'] == 0 and records[0]['reaped']


@pytest.mark.parametrize('fault', ['sif', 'descriptor', 'template', 'overlap'])
def test_provision_invalid_inputs_never_start_child(tmp_path, fault):
    kwargs = fixture(tmp_path)
    if fault == 'sif':
        Path(kwargs['runtime_profile']['sif']).write_bytes(b'changed')
    elif fault in ('descriptor', 'template'):
        (kwargs['inputs'] / ('prepare.json' if fault == 'descriptor' else 'template.json')).write_text('{"changed":true}')
    else:
        kwargs['public'] = kwargs['private']
    with pytest.raises(ValueError):
        provision_run(**kwargs)
    assert not any(kwargs['output'].iterdir())
