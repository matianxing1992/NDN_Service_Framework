#!/usr/bin/env python3
"""Offline SDK signing with a public deterministic test key; C++ consumes JSON only."""
import base64
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'),
               str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.sdk.placement import ProviderOfferV3, DeviceTopologyProfile
from ndnsf_distributed_inference.app_sdk.provider import ProviderOfferTrustVerifier
from ndnsf_distributed_inference.core.contracts import canonical_digest

def digest(text):
    return 'sha256:' + hashlib.sha256(text.encode()).hexdigest()

signed = json.loads((root / 'signed-offer-oracle.json').read_text())
key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
policy = dict(signed['policy'], entries=[])
model_intent = canonical_digest({'model_name': 'fixture-model', 'content_digest': digest('model'),
    'semantics_digest': digest('semantics'), 'source_revision': None})
offers = []
for suffix, role in [('a', '/role'), ('b', '/Merge')]:
    provider = '/provider/' + suffix
    policy['entries'].append(dict(signed['policy']['entries'][0], provider=provider,
        keyLocatorPrefix=provider + '/KEY/fixture',
        certificateName=provider + '/KEY/fixture/issuer/v=1'))
    topology = DeviceTopologyProfile(provider, (), 'onnxruntime-cpu')
    offer = ProviderOfferV3('/request', 1, '/service', provider, model_intent, digest('planning'),
        True, 'ACCEPT_WITH_PREPARATION', True, topology, accepted_roles=(role,),
        backends=('onnxruntime-cpu',), boot_epoch='fixture-boot', captured_at_ms=100,
        expires_at_ms=2000000000000, signer_key_id=signed['key_id'], signature='pending')
    offer = replace(offer, signature=base64.b64encode(key.sign(offer.digest().encode())).decode())
    offers.append(offer)
verifier = ProviderOfferTrustVerifier(policy, {signed['key_id']: signed['public_pem'].encode()},
    trust_schema_verifier=lambda ack: False, candidate_digest=signed['candidate'])
assert all(verifier(offer) for offer in offers)
(root / 'native-merge-offers.json').write_text(json.dumps({
    'policy': policy, 'candidate': signed['candidate'], 'key_id': signed['key_id'],
    'public_pem': signed['public_pem'], 'offers': [o.to_bytes().decode() for o in offers]
}, indent=2, sort_keys=True) + '\n')
