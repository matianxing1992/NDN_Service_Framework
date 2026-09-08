#!/usr/bin/env python3
"""Public deterministic test key only; freeze actual SDK signature verification."""
import base64
import dataclasses
import hashlib
import json
from pathlib import Path
import sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'NDNSF-DistributedInference'))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'NDNSF-DistributedRepo/pythonWrapper'))
from ndnsf_distributed_inference.sdk.placement import ProviderOfferV3
from ndnsf_distributed_inference.app_sdk.provider import ProviderOfferTrustVerifier

root = Path(__file__).parent
key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
public = key.public_key()
raw = public.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
key_id = 'sha256:' + hashlib.sha256(raw).hexdigest()
pem = public.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
candidate = 'sha256:' + hashlib.sha256(b'candidate').hexdigest()
policy = {'schema': 'spec180-provider-offer-trust-v1', 'candidateId': 'fixture',
          'candidateDigest': candidate, 'trustSchema': '/trust/schema', 'entries': [
              {'provider': '/provider/a', 'service': '/service',
               'keyLocatorPrefix': '/provider/a/KEY/fixture', 'signerKeyId': key_id,
               'certificateName': '/provider/a/KEY/fixture/issuer/v=1'}]}
verifier = ProviderOfferTrustVerifier(policy, {key_id: pem},
    trust_schema_verifier=lambda ack: False, candidate_digest=candidate)
vectors = []
for sample in json.loads((root / 'offer-python-oracle.json').read_text()):
    offer = dataclasses.replace(ProviderOfferV3.from_bytes(sample['wire'].encode()), signer_key_id=key_id)
    offer = dataclasses.replace(offer, signature=base64.b64encode(key.sign(offer.digest().encode())).decode())
    assert verifier(offer)
    vectors.append({'name': sample['name'], 'wire': offer.to_bytes().decode(), 'digest': offer.digest()})
(root / 'signed-offer-oracle.json').write_text(json.dumps(
    {'policy': policy, 'candidate': candidate, 'key_id': key_id, 'public_pem': pem.decode(),
     'vectors': vectors}, indent=2, sort_keys=True) + '\n')
