"""Prepare command/mount boundary; actual native preparation is a later gate."""
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo
from runtime.baseline import container_command


def descriptor(tmp_path):
    value = dict(schema='tiger-yolo-prepare-input-v1', plan={'schema': 'tiger-yolo-run-plan-v1'},
        templateDigest='sha256:' + 'a' * 64, manifestDigest='sha256:' + 'b' * 64,
        registryDigest='sha256:' + 'c' * 64, candidateDigest='sha256:' + 'd' * 64,
        protectionEpoch='epoch-1', candidateId='spec183-test')
    path = tmp_path / 'prepare.json'
    path.write_text(json.dumps(value))
    return path, 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()


def test_internal_command_passes_fixed_paths_and_propagates_failure(tmp_path, monkeypatch, capsys):
    path, digest = descriptor(tmp_path)
    calls = []
    def prepare(**kwargs):
        calls.append(kwargs)
        raise RuntimeError('SECRET MUST NOT APPEAR')
    monkeypatch.setattr(yolo, 'prepare_in_container', prepare)
    assert yolo.main(['prepare', '--descriptor', str(path), '--descriptor-sha256', digest]) == 2
    assert calls[0]['package'] == Path('/artifacts')
    assert calls[0]['authority_private'] == Path('/inputs/private/artifact-policy-authority.key')
    output = capsys.readouterr().out
    assert 'SECRET' not in output and json.loads(output)['status'] == 'FAILED'
    assert json.loads(output)['frames']


def test_changed_descriptor_never_calls_prepare(tmp_path, monkeypatch):
    path, digest = descriptor(tmp_path)
    path.write_text('{}')
    def forbidden(**kwargs):
        pytest.fail('prepare must not run on changed descriptor')
    monkeypatch.setattr(yolo, 'prepare_in_container', forbidden)
    assert yolo.main(['prepare', '--descriptor', str(path), '--descriptor-sha256', digest]) == 2


def test_empty_issuer_home_created_by_apptainer_is_allowed(tmp_path):
    public, private = tmp_path / 'public', tmp_path / 'private'
    public.mkdir()
    (private / 'root').mkdir(parents=True)
    yolo.validate_preparation_roots(public, private)
    (private / 'root' / 'old-key').write_bytes(b'preserve')
    with pytest.raises(ValueError, match='NOT_EMPTY'):
        yolo.validate_preparation_roots(public, private)


@pytest.mark.parametrize('scope', ['worker', 'network', 'gpu', 'offline'])
def test_private_inputs_only_mounted_for_offline_preparation(scope):
    kwargs = {'preparation_inputs': Path('/sealed-inputs')}
    if scope != 'worker':
        kwargs['prepare'] = Path('/homes')
    if scope == 'network':
        kwargs['node'] = Path('/node')
    if scope == 'gpu':
        kwargs.update(gpu=True, gpu_device='0')
    args = ({'apptainer': '/bin/apptainer', 'sif': '/image.sif'}, Path('/bundle'),
            Path('/homes/root'), Path('/public'), Path('/output'), ['python', '-m', 'apps.yolo'])
    if scope != 'offline':
        with pytest.raises(ValueError, match='PREPARATION_INPUT_SCOPE'):
            container_command(*args, **kwargs)
    else:
        command = container_command(*args, **kwargs)
        assert '/sealed-inputs:/inputs:ro' in command
        assert '/homes:/identities:rw' in command
        assert '/public:/config:rw' in command
        assert '--nv' not in command
