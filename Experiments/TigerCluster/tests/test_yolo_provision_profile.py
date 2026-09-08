"""Profile-to-issuer mapping with real readers; fixtures are not credentials."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_profile import ClosureError, resolve_provision_inputs, resolve_run_plan
from test_yolo_submit import dispatch_profile, file_ref, refresh_effective_profile


def fixture(tmp_path):
    path, profile, _ = dispatch_profile(tmp_path)
    template = tmp_path / 'template.json'
    template.write_text('{"services": []}')
    package = tmp_path / 'model'
    package.mkdir()
    manifest = package / 'manifest.json'
    manifest.write_text(json.dumps({'catalogue': {'candidates': [{
        'candidateId': 'shared-backbone-two-shard-v1', 'candidateDigest': 'sha256:' + 'b' * 64}]}}))
    contracts = tmp_path / 'trust/contracts'
    contracts.mkdir(parents=True)
    registry = {}
    for owner in ('catalogue', 'modelManifest', 'artifactPolicyAuthority'):
        key = contracts / (owner + '.pub')
        key.write_text('public fixture, not a key')
        registry[owner] = {'publicKeyPath': 'contracts/' + key.name,
                           'publicKeySha256': file_ref(key)['sha256']}
    registry['artifactPolicyAuthority']['protectionEpochs'] = ['epoch-1']
    registry_path = contracts / 'trust-root-registry-v1.json'
    registry_path.write_text(json.dumps(registry))
    secret = tmp_path / 'authority.key'
    secret.write_text('private fixture; never an actual key')
    secret.chmod(0o600)
    profile['workload'] = {'descriptor': file_ref(template), 'packageManifest': file_ref(manifest)}
    profile['security'].update(trustPolicy=file_ref(registry_path), authorityPrivateKey=secret.name)
    path.write_text(json.dumps(profile))
    refresh_effective_profile(path, profile)
    plan = resolve_run_plan(path, stage='dispatch', case='two-node-gpu', run_id='mapping-test', output=tmp_path / 'runs')
    return path, profile, plan


def test_profile_resolves_every_issuer_input_without_launch_or_copy(tmp_path, monkeypatch):
    path, profile, plan = fixture(tmp_path)
    before = set(tmp_path.rglob('*'))
    monkeypatch.chdir(tmp_path.parent)
    result = resolve_provision_inputs(path, plan=plan, runtime_candidate_digest='sha256:' + 'a' * 64)
    assert result['qualification'] == 'NOT_EVALUATED'
    desc = result['descriptor']
    assert desc['runtimeCandidateDigest'] == 'sha256:' + 'a' * 64
    assert desc['placementCandidateDigest'] == 'sha256:' + 'b' * 64
    assert desc['registryDigest'] == profile['security']['trustPolicy']['sha256']
    assert desc['templateDigest'] == profile['workload']['descriptor']['sha256']
    assert desc['manifestDigest'] == profile['workload']['packageManifest']['sha256']
    assert result['runtimeProfile']['sif'] == str(tmp_path / 'runtime/sif')
    assert result['authorityPrivateKey'] == str(tmp_path / 'authority.key')
    assert result['package'] == str(tmp_path / 'model')
    assert len(result['publicInputs']) == 5
    assert set(tmp_path.rglob('*')) == before
    assert 'authorityPrivateKey' not in plan['effectiveBehavior']['profile']['security']
    assert 'private fixture' not in json.dumps(result)


@pytest.mark.parametrize('case,expected', [('local-cpu', '1.5.3'),
    ('two-node-gpu', '1.3.4'), ('negative-dependency', '1.3.4')])
def test_explicit_local_tools_preserve_the_shared_composition(tmp_path, case, expected):
    path, profile, _ = fixture(tmp_path)
    profile['runtime']['local'] = dict(apptainer='/local/apptainer', apptainerVersion='1.5.3')
    path.write_text(json.dumps(profile))
    refresh_effective_profile(path, profile)
    plan = resolve_run_plan(path, stage='dispatch', case=case,
                            run_id='environment-test', output=tmp_path/'runs')
    result = resolve_provision_inputs(path, plan=plan,
                                      runtime_candidate_digest='sha256:'+'a'*64)
    assert result['runtimeProfile']['apptainerVersion'] == expected
    assert result['runtimeProfile']['sif'] == str(tmp_path/'runtime/sif')
    local = plan['effectiveBehavior']['profile']['runtime']['local']
    assert local == {'apptainerVersion': '1.5.3'}


@pytest.mark.parametrize('fault,reason', [('template', 'FILE_DIGEST'),
    ('key-mode', 'PROVISION_PRIVATE_KEY_PERMISSIONS'), ('key-missing', 'PROVISION_PRIVATE_KEY_UNAVAILABLE'),
    ('old-plan', 'PROVISION_PROFILE_CHANGED'), ('candidate', 'PROVISION_RUNTIME_CANDIDATE')])
def test_mapping_rejects_unusable_inputs(tmp_path, fault, reason):
    path, profile, plan = fixture(tmp_path)
    candidate = 'sha256:' + 'a' * 64
    if fault == 'template':
        target = Path(profile['workload']['descriptor']['path'])
        target.write_bytes(b'x' * target.stat().st_size)
    elif fault == 'key-mode':
        (tmp_path / 'authority.key').chmod(0o644)
    elif fault == 'key-missing':
        (tmp_path / 'authority.key').unlink()
    elif fault == 'old-plan':
        plan['documentDigest'] = 'sha256:' + 'f' * 64
    else:
        candidate = None
    with pytest.raises(ClosureError, match=reason):
        resolve_provision_inputs(path, plan=plan, runtime_candidate_digest=candidate)
    assert not (tmp_path / 'runs').exists()


def test_resolved_inputs_stage_into_fixed_private_issuer_layout(tmp_path):
    from runtime.yolo_operator import stage_provision_inputs
    from apps.yolo import preparation_arguments
    path, _, plan = fixture(tmp_path)
    resolved = resolve_provision_inputs(path, plan=plan, runtime_candidate_digest='sha256:' + 'a' * 64)
    target = tmp_path / 'issuer-inputs'
    digest = stage_provision_inputs(resolved, target)
    decoded = preparation_arguments(target / 'prepare.json', digest)
    assert decoded['runtime_candidate_digest'] == 'sha256:' + 'a' * 64
    assert decoded['placement_candidate_digest'] == 'sha256:' + 'b' * 64
    secret = target / 'private/artifact-policy-authority.key'
    assert secret.stat().st_mode & 0o777 == 0o600
    assert target.stat().st_mode & 0o777 == 0o700
    assert secret.read_bytes() == (tmp_path / 'authority.key').read_bytes()
    assert not (target / 'model').exists()
    with pytest.raises(ValueError, match='DESTINATION'):
        stage_provision_inputs(resolved, target)


def test_staging_rechecks_resolved_public_bytes_before_writing(tmp_path):
    from runtime.yolo_operator import stage_provision_inputs
    path, _, plan = fixture(tmp_path)
    resolved = resolve_provision_inputs(path, plan=plan, runtime_candidate_digest='sha256:' + 'a' * 64)
    Path(resolved['publicInputs']['template.json']['path']).write_text('changed')
    target = tmp_path / 'issuer-inputs'
    with pytest.raises(ValueError, match='INPUT_CHANGED'):
        stage_provision_inputs(resolved, target)
    assert not target.exists()
