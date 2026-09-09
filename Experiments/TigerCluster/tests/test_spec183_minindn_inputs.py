"""Actual file/key binding checks; process boundary is a double, no MiniNDN PASS."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import spec183_minindn as runner
from test_yolo_operator_profile import profile_fixture
from test_yolo_public_inventory import prepared_public, seal
from test_yolo_bundle import fixture_manifest


def digest(path):
    return 'sha256:'+hashlib.sha256(path.read_bytes()).hexdigest()


def key_pair(secret, public):
    key = ed25519.Ed25519PrivateKey.generate()
    secret.parent.mkdir(parents=True, exist_ok=True)
    secret.write_bytes(key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    secret.chmod(0o600)
    public.write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo))
    raw = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return 'sha256:'+hashlib.sha256(raw).hexdigest()


@pytest.fixture
def inputs(tmp_path):
    from runtime.yolo_bundle import freeze_harness
    from runtime.yolo_profile import load_operator_profile
    from jobs.yolo.submit import _prepared_candidate
    from apps.yolo import configuration_for_run
    output = tmp_path/'runs'
    root = output/'run-1'
    root.mkdir(parents=True)
    public, private = root/'public', root/'private'
    plan = prepared_public(public)
    plan.update(schema='tiger-yolo-run-plan-v1', case='local-cpu', output=str(root), applicationName='/run')
    template = {'services': [dict(name='/YOLO', roles=list(runner.ROLES))]}
    case = configuration_for_run(template, plan)
    (public/'case.json').write_text(json.dumps(case))
    offer, recipients, entries = {}, {}, []
    for role in runner.ROLES:
        identity = plan['identities'][role]
        key_id = key_pair(private/role/'offer.pem', public/'offers'/(role+'.pub'))
        offer[key_id] = '/config/offers/'+role+'.pub'
        entries.append(dict(provider=identity, signerKeyId=key_id, service='/YOLO'))
        key_pair(private/role/'recipient.pem', public/'recipients'/(role+'.pub'))
        recipients[identity] = dict(path='recipients/'+role+'.pub', sha256=digest(public/'recipients'/(role+'.pub')))
    key_pair(private/'user/authority/artifact-policy-authority.key', public/'contracts/authority.pub')
    for name in ('request-envelope.key', 'requester.key'):
        (private/'user'/name).write_bytes(os.urandom(32))
        (private/'user'/name).chmod(0o600)
    (public/'offer-public-key-map.json').write_text(json.dumps(offer))
    (public/'recipient-public-keys.json').write_text(json.dumps(recipients))
    (public/'offer-trust-root.json').write_text(json.dumps(dict(candidateId='graph',
        candidateDigest='sha256:'+'b'*64, entries=entries)))
    profile, _ = profile_fixture(tmp_path)
    loaded = load_operator_profile(profile, stage='inputs')
    plan['documentDigest'] = loaded['documentDigest']
    manifest, manifest_digest = fixture_manifest(tmp_path/'harness-source')
    bundle = root/'bundle'
    freeze_harness(manifest, bundle, expected_manifest_sha256=manifest_digest)
    prepared = dict(schema='tiger-yolo-prepared-run-v2', status='PREPARED', qualification='NOT_EVALUATED',
        runId='run-1', case='local-cpu', profileDigest=loaded['documentDigest'], plan=plan,
        bundle=str(bundle), harnessManifestSha256=manifest_digest,
        contentIdentities=dict.fromkeys(('inputs','runtime','dispatch'), 'sha256:'+'c'*64))
    prepared['candidateDigest'] = _prepared_candidate(prepared)
    (root/'prepare.json').write_text(json.dumps(prepared))
    seal(public, plan)
    path = public/'preparation.json'
    receipt = json.loads(path.read_text())
    receipt.update(candidateDigest=prepared['candidateDigest'], placementCandidateId='graph',
        placementCandidateDigest='sha256:'+'b'*64, protectionEpoch='epoch-1',
        packageManifestDigest=loaded['profile']['workload']['packageManifest']['sha256'],
        catalogueDataName='/run/controller/NDNSF/DI/catalogue/v1', catalogueSigner='/run/controller')
    path.write_text(json.dumps(receipt))
    return dict(output=output, run_id='run-1', profile_path=profile, preparation_sha256=digest(path))


def test_inputs_bind_real_per_run_keys_and_preserve_source_files(inputs):
    before = {p:p.read_bytes() for p in inputs['output'].rglob('*') if p.is_file()}
    observed = runner.validated_inputs(**inputs)
    assert observed['receipt']['qualification'] == 'NOT_EVALUATED'
    assert observed['package'] == inputs['profile_path'].parent
    assert set(observed['privateMap']) == {'/run/'+r for r in runner.ROLES}
    assert all('/private/' in v and v.endswith('/offer.pem') for v in observed['privateMap'].values())
    assert not any('.keys/offers' in v for v in observed['privateMap'].values())
    assert before == {p:p.read_bytes() for p in inputs['output'].rglob('*') if p.is_file()}
    assert not (inputs['output']/'run-1/host-minindn').exists()


def test_y_n_matrix_uses_declared_cluster_walltime_budget(inputs):
    profile = json.loads(inputs['profile_path'].read_text())
    assert runner._case_runtime_seconds(profile, 'Y-B') == 300
    assert runner._case_runtime_seconds(profile, 'Y-N') == 900
    profile['cluster'].pop('wallTimeSeconds')
    with pytest.raises(ValueError, match='MININDN_MATRIX_WALLTIME'):
        runner._case_runtime_seconds(profile, 'Y-N')


@pytest.mark.parametrize('fault', ['receipt-pin', 'prepare-marker', 'prepared-run', 'profile',
    'public-file', 'package', 'offer-key', 'recipient-key', 'authority-key', 'private-mode', 'private-symlink'])
def test_invalid_input_starts_no_driver_and_writes_no_host_state(inputs, monkeypatch, fault):
    root = inputs['output']/'run-1'
    if fault == 'receipt-pin': inputs['preparation_sha256'] = 'sha256:'+'0'*64
    elif fault == 'prepare-marker': (root/'prepare.json').write_text('{"status":"PREPARED"}')
    elif fault == 'prepared-run':
        path = root/'prepare.json'; value = json.loads(path.read_text()); value['plan']['runId'] = 'other'
        path.write_text(json.dumps(value))
    elif fault == 'profile':
        path = inputs['profile_path']; value=json.loads(path.read_text());value['timing']['startupSeconds'] += 1
        path.write_text(json.dumps(value))
    elif fault == 'public-file': (root/'public/case.json').write_text('{}')
    elif fault == 'package': (inputs['profile_path'].parent/'fixture-only.txt').write_text('changed package')
    else:
        secret = root/'private/BackboneNeck'/('recipient.pem' if fault=='recipient-key' else 'offer.pem')
        if fault == 'authority-key': secret = root/'private/user/authority/artifact-policy-authority.key'
        if fault == 'private-mode': secret.chmod(0o644)
        elif fault == 'private-symlink':
            secret.unlink();secret.symlink_to(root/'private/DetectShard0/offer.pem')
        else:
            key=ed25519.Ed25519PrivateKey.generate()
            secret.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    called=[]
    monkeypatch.setattr(subprocess,'run',lambda *a,**k:called.append(True))
    with pytest.raises((ValueError, OSError)):
        runner.main(['--run-id', inputs['run_id'], '--output', str(inputs['output']),
            '--profile', str(inputs['profile_path']), '--preparation-sha256', inputs['preparation_sha256']])
    assert not called and not (root/'host-minindn').exists()


def test_main_maps_verified_inputs_into_exclusive_host_output(inputs, monkeypatch, capsys):
    from runtime import host_minindn
    called=[]
    def run(command, env, output, **kwargs):
        kwargs.update(env=env, output=output)
        called.append((command, kwargs))
        return 0
    monkeypatch.setattr(host_minindn,'supervise',run)
    monkeypatch.setenv('UNRELATED_PRIVATE_CREDENTIAL', 'do-not-forward')
    monkeypatch.setenv('NDNSF_SPEC180_SIF_RUNTIME', '1')
    argv=['--run-id',inputs['run_id'],'--output',str(inputs['output']),
          '--profile',str(inputs['profile_path']),'--preparation-sha256',inputs['preparation_sha256']]
    assert runner.main(argv) == 0
    root = inputs['output']/'run-1'
    env=called[0][1]['env']
    assert 'UNRELATED_PRIVATE_CREDENTIAL' not in env
    assert 'NDNSF_SPEC180_SIF_RUNTIME' not in env
    assert called[0][1]['output'] == root/'host-minindn/supervisor'
    assert called[0][1]['seconds'] > 0 and called[0][1]['cleanup_seconds'] > 0
    assert env['SPEC181_PROTECTION_EPOCH']=='epoch-1'
    assert env['NDNSF_SPEC180_CONFIG_ROOT']==str(root/'private/user/authority')
    assert env['SPEC181_REQUESTER_PRIVATE_KEY']==str(root/'private/user/requester.key')
    assert env['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']==str(root/'public/contracts/authority.pub')
    assert env['SPEC180_YOLO_CANONICAL_PACKAGE']==str(inputs['profile_path'].parent)
    assert env['SPEC180_CASE_OUTPUT_DIR']==str(root/'host-minindn/output')
    for p in (root/'host-minindn/inputs').iterdir():
        assert p.stat().st_mode & 0o777 == 0o600
    assert json.loads(capsys.readouterr().out.splitlines()[-1])['qualification']=='NOT_EVALUATED'
    with pytest.raises(FileExistsError): runner.main(argv)
    assert len(called)==1


def test_exact_sif_keeps_explicit_host_policy_loader_closure(inputs, monkeypatch,
                                                              capsys):
    """The outer policy preflight may use a separately declared host closure."""
    from runtime import host_minindn
    called = []

    def run(command, env, output, **kwargs):
        called.append(dict(env))
        return 0

    monkeypatch.setattr(host_minindn, 'supervise', run)
    monkeypatch.setenv('SPEC180_RUNTIME_SIF', '/base/runtime.sif')
    monkeypatch.setenv('SPEC180_RUNTIME_APP_ROOT', str(inputs['output']))
    monkeypatch.setenv('SPEC180_HOST_LIBRARY_PATH', '/tmp/matching-core:/usr/local/lib')
    argv = ['--run-id', inputs['run_id'], '--output', str(inputs['output']),
            '--profile', str(inputs['profile_path']),
            '--preparation-sha256', inputs['preparation_sha256']]
    assert runner.main(argv) == 0
    assert called[0]['LD_LIBRARY_PATH'] == '/tmp/matching-core:/usr/local/lib'
    assert json.loads(capsys.readouterr().out.splitlines()[-1])['qualification'] == 'NOT_EVALUATED'
