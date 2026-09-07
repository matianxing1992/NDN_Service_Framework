"""Pinned material import and real authority-key matching, no model execution."""
import hashlib
import json
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.identities import install_yolo_trust
from test_yolo_recipients import setup
from ndnsf_distributed_inference.security.registry_keys import (
    load_artifact_policy_authority_registry, load_artifact_policy_authority_private_key)


def trust_inputs(tmp_path):
    inputs, _ = setup(tmp_path)
    source = tmp_path / 'source/contracts'
    source.mkdir(parents=True)
    document = {'schemaVersion': 1, 'status': 'CONFIGURED'}
    keys = {}
    for name in ('catalogue', 'modelManifest', 'artifactPolicyAuthority'):
        keys[name] = key = ed25519.Ed25519PrivateKey.generate()
        payload = key.public_key().public_bytes(serialization.Encoding.PEM,
                                               serialization.PublicFormat.SubjectPublicKeyInfo)
        (source / (name + '.pub')).write_bytes(payload)
        document[name] = dict(authorityId=name, keyId=name + '-key', publicKeyAlgorithm='ed25519',
            signatureAlgorithm='ed25519', publicKeyPath='contracts/' + name + '.pub',
            publicKeySha256='sha256:' + hashlib.sha256(payload).hexdigest(),
            acceptedModelFamilies=['YOLO26n'])
    document['artifactPolicyAuthority'].update(grantSchema='ndnsf-di-key-grant-v1',
                                               protectionEpochs=['epoch-1'])
    registry = source / 'trust-root-registry-v1.json'
    registry.write_text(json.dumps(document))
    private = tmp_path / 'operator.key'
    private.write_bytes(keys['artifactPolicyAuthority'].private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    private.chmod(0o600)
    options = dict(expected_registry_digest='sha256:' + hashlib.sha256(registry.read_bytes()).hexdigest(),
        authority_private=private, user_home=inputs['homes']['user'], public=inputs['public'],
        protection_epoch='epoch-1')
    return registry, options


def test_import_preserves_registry_and_existing_authority(tmp_path):
    registry, options = trust_inputs(tmp_path)
    before = registry.read_bytes()
    install_yolo_trust(registry, **options)
    target = options['public'] / 'contracts/trust-root-registry-v1.json'
    assert target.read_bytes() == before == registry.read_bytes()
    policy = load_artifact_policy_authority_registry(target, model_family='YOLO26n', protection_epoch='epoch-1')
    load_artifact_policy_authority_private_key(options['user_home'] / 'authority',
                                              expected_public_key=policy.public_key)
    assert not list(options['public'].rglob('*.key'))
    assert (options['public'] / 'contracts/authority.pub').read_bytes() == (
        registry.parent / 'artifactPolicyAuthority.pub').read_bytes()
    with pytest.raises(ValueError, match='REUSE'):
        install_yolo_trust(registry, **options)


@pytest.mark.parametrize('fault', ['registry-hash', 'public-hash', 'wrong-private', 'private-mode', 'epoch'])
def test_mismatched_trust_material_rejected_before_output(tmp_path, fault):
    registry, options = trust_inputs(tmp_path)
    if fault == 'registry-hash':
        options['expected_registry_digest'] = 'sha256:' + '0' * 64
    elif fault == 'public-hash':
        (registry.parent / 'catalogue.pub').write_bytes(b'changed')
    elif fault == 'wrong-private':
        options['authority_private'].write_bytes(ed25519.Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    elif fault == 'private-mode':
        options['authority_private'].chmod(0o644)
    else:
        options['protection_epoch'] = 'unregistered'
    with pytest.raises(ValueError):
        install_yolo_trust(registry, **options)
    assert not (options['public'] / 'contracts').exists()
    assert not (options['user_home'] / 'authority').exists()
