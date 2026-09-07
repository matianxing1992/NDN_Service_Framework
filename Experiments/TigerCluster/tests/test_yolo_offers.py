"""Real offer keys and certificate wire parsing, not ACK authentication."""
import hashlib
import json
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.identities import issue_yolo_offers
from test_yolo_recipients import setup
from test_yolo_certificate_binding import wire


def inputs_for_offer(tmp_path):
    inputs, names = setup(tmp_path)
    for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'):
        (inputs['public'] / (role + '.cert')).write_bytes(wire(names[role] + '/KEY/k/issuer/v=1'))
    options = dict(service='/YOLO', candidate_id='spec183-test', candidate_digest='sha256:' + 'a' * 64,
                   trust_schema='/run/trust')
    return inputs, names, options


def test_offer_policy_and_keys_bind_exact_certificates(tmp_path):
    inputs, names, options = inputs_for_offer(tmp_path)
    issue_yolo_offers('/run', inputs['homes'], inputs['public'], names, **options)
    policy = json.loads((inputs['public'] / 'offer-trust-root.json').read_text())
    public_map = json.loads((inputs['public'] / 'offer-public-key-map.json').read_text())
    assert policy['candidateDigest'] == options['candidate_digest']
    assert len(policy['entries']) == len(public_map) == 4
    for entry in policy['entries']:
        role = entry['provider'].rsplit('/', 1)[1]
        assert entry['certificateName'] == names[role] + '/KEY/k/issuer/v=1'
        assert entry['keyLocatorPrefix'] == names[role] + '/KEY/k'
        key = serialization.load_pem_private_key((inputs['homes'][role] / 'offer.pem').read_bytes(),
                                                  password=None, backend=default_backend())
        public = serialization.load_pem_public_key(
            (inputs['public'] / 'offers' / (role + '.pub')).read_bytes(), backend=default_backend())
        public.verify(key.sign(b'actual offer signing test'), b'actual offer signing test')
        assert entry['signerKeyId'] == 'sha256:' + hashlib.sha256(public.public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)).hexdigest()
        assert public_map[entry['signerKeyId']] == '/config/offers/' + role + '.pub'
    with pytest.raises(ValueError, match='REUSE'):
        issue_yolo_offers('/run', inputs['homes'], inputs['public'], names, **options)


@pytest.mark.parametrize('fault', ['foreign-cert', 'missing-cert', 'bad-digest', 'private-exists'])
def test_offer_preparation_rejects_before_key_generation(tmp_path, fault):
    inputs, names, options = inputs_for_offer(tmp_path)
    cert = inputs['public'] / 'Merge.cert'
    if fault == 'foreign-cert':
        cert.write_bytes(wire('/foreign/KEY/k/issuer/v=1'))
    elif fault == 'missing-cert':
        cert.unlink()
    elif fault == 'bad-digest':
        options['candidate_digest'] = 'unbound'
    else:
        (inputs['homes']['Merge'] / 'offer.pem').write_bytes(b'preserve')
    with pytest.raises((ValueError, OSError)):
        issue_yolo_offers('/run', inputs['homes'], inputs['public'], names, **options)
    assert not (inputs['public'] / 'offers').exists()
    assert not (inputs['homes']['BackboneNeck'] / 'offer.pem').exists()
