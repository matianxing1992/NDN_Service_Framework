#!/usr/bin/env python3
"""Author frozen real SDK offer vectors; never a native runtime dependency."""
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'NDNSF-DistributedInference'))
from ndnsf_distributed_inference.sdk.placement import (
    ProviderOfferV3, DeviceTopologyProfile, DeviceResourceSnapshot, ResidencyProofV3,
    canonical_bytes)

def digest(text):
    return 'sha256:' + hashlib.sha256(text.encode()).hexdigest()

vectors = []
for name in ('cpu', 'cuda', 'integer-cost', 'defaults', 'exact', 'reject'):
    cuda = name == 'cuda'
    topology = DeviceTopologyProfile('/provider/a', ('cuda:0',) if cuda else (),
                                     'onnxruntime-cuda' if cuda else 'cpu')
    proof = ResidencyProofV3(digest('artifact'), '/role', 0, 'GPU', ('cuda:0',),
        'boot', 'process', topology.digest(), 100, 1000,
        residency_class='LOADED_RUNTIME', identity_digest=digest('identity'),
        assembly_spec_digest=digest('assembly'), model_manifest_digest=digest('manifest'),
        artifact_profile_digest=digest('profile'), graph_digest=digest('graph'),
        backend='onnxruntime-cuda', protection_epoch='epoch', runtime_generation=7,
        fencing_token='fence', missing_verified_bytes=11, estimated_assembly_ms=1.5,
        estimated_load_ms=2.5)
    offer = ProviderOfferV3('request', 1, '/service', '/provider/a', digest('model'),
        digest('graph'), name != 'reject',
        'REJECT' if name == 'reject' else 'ACCEPT_IF_EXACT_REUSE' if name == 'exact'
        else 'ACCEPT_WITH_PREPARATION', name not in ('exact', 'reject'), topology,
        resources=(DeviceResourceSnapshot('cuda:0', 12000, 9000, 2, 3, 100, topology.digest()),) if cuda else (),
        residency=(proof,) if cuda else (), accepted_roles=('/role',), backends=(topology.backend,),
        estimated_wait_ms=0 if name == 'integer-cost' else 0.0,
        boot_epoch='boot', captured_at_ms=100, expires_at_ms=1000,
        signer_key_id='key', signature='fixture-signature-not-authentication', has_model=True)
    wire = offer.to_bytes()
    if name == 'defaults':
        raw = json.loads(wire)
        for key in ('resources', 'residency', 'queue_depth', 'rtt_ms', 'bandwidth_mbps',
                    'can_provision', 'ack_reservation'):
            raw.pop(key)
        wire = canonical_bytes(raw)
    decoded = ProviderOfferV3.from_bytes(wire)
    vectors.append({'name': name, 'wire': wire.decode(), 'digest': decoded.digest(),
                    'topology_digest': decoded.topology.digest()})
Path(__file__).with_name('offer-python-oracle.json').write_text(
    json.dumps(vectors, indent=2, sort_keys=True) + '\n')
