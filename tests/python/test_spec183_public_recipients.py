"""Real public-key decoding without access to Provider private files."""
import hashlib
import json

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives.asymmetric import ed25519, ec

from ndnsf_distributed_inference.security import registry_keys


def prepare(tmp_path, key):
    payload = key.public_key().public_bytes(serialization.Encoding.PEM,
                                           serialization.PublicFormat.SubjectPublicKeyInfo)
    (tmp_path / 'recipient.pub').write_bytes(payload)
    path = tmp_path / 'recipients.json'
    path.write_text(json.dumps({'/provider/a': {'path': 'recipient.pub',
        'sha256': 'sha256:' + hashlib.sha256(payload).hexdigest()}}))
    return path


@pytest.mark.parametrize('algorithm', ['ed25519', 'p256'])
def test_requester_only_needs_recipient_public_bytes(tmp_path, algorithm):
    key = (ed25519.Ed25519PrivateKey.generate() if algorithm == 'ed25519'
           else ec.generate_private_key(ec.SECP256R1(), default_backend()))
    path = prepare(tmp_path, key)
    recipients = registry_keys.load_grant_recipient_public_map(path)
    assert recipients['/provider/a'].public_bytes(serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo) == key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    assert sorted(p.name for p in tmp_path.iterdir()) == ['recipient.pub', 'recipients.json']


@pytest.mark.parametrize('fault', ['hash', 'escape', 'symlink', 'private', 'curve', 'duplicate'])
def test_public_map_rejects_invalid_recipient_material(tmp_path, fault):
    key = ed25519.Ed25519PrivateKey.generate()
    path = prepare(tmp_path, key)
    value = json.loads(path.read_text())
    if fault == 'hash':
        (tmp_path / 'recipient.pub').write_bytes(b'changed')
    elif fault == 'escape':
        value['/provider/a']['path'] = '../recipient.pub'
    elif fault == 'symlink':
        original = tmp_path / 'recipient.pub'
        original.rename(tmp_path / 'elsewhere.pub')
        original.symlink_to(tmp_path / 'elsewhere.pub')
    elif fault in ('private', 'curve'):
        payload = (key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption()) if fault == 'private' else
                   ec.generate_private_key(ec.SECP384R1(), default_backend()).public_key().public_bytes(
                       serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        (tmp_path / 'recipient.pub').write_bytes(payload)
        value['/provider/a']['sha256'] = 'sha256:' + hashlib.sha256(payload).hexdigest()
    path.write_text(json.dumps(value))
    if fault == 'duplicate':
        path.write_text('{"/provider/a":{},"/provider/a":{}}')
    with pytest.raises((ValueError, OSError)):
        registry_keys.load_grant_recipient_public_map(path)
