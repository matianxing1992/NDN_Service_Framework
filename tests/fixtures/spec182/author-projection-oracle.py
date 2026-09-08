#!/usr/bin/env python3
"""Offline SDK oracle for role dataflow and admitted snapshot device binding."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root.parents[2] / 'NDNSF-DistributedInference'))
from ndnsf_distributed_inference.sdk.placement import (
    ProviderOfferV3, DeviceBinding, RoleDataflowContract, canonical_digest, canonical_bytes)

def digest(value):
    return 'sha256:' + hashlib.sha256(value.encode()).hexdigest()

rows = {}
for sample in json.loads((root / 'placement-v3-oracle.json').read_text())['seal_cases']:
    if len(sample['ranks']) != 1 or sample.get('publication_reject'):
        continue
    providers = sample['expected']['providers']
    role, provider = next(iter(providers.items()))
    offer = next(ProviderOfferV3.from_bytes(wire.encode()) for wire in sample['offers']
                 if json.loads(wire)['provider'] == provider)
    grant = (provider, '/grant/' + role, digest('grant-' + role))
    plan = canonical_digest({'core': sample['core_digest'], 'grants': (grant,),
                             'securityPolicySnapshotDigest': digest('projection-policy')})
    dataflow = RoleDataflowContract('request', 1, plan, role, terminal_response_owner=True)
    placed = sample['expected']['roles'][0]
    device = DeviceBinding('CPU' if placed['backend'].endswith('-cpu') else 'SINGLE_DEVICE',
        provider, role, offer.digest(), offer.topology.digest(), canonical_digest(offer.resources),
        max((r.resource_sequence for r in offer.resources), default=1),
        '' if placed['backend'].endswith('-cpu') else placed['device_set'][0])
    rows[sample['name']] = {'plan_digest': plan,
        'dataflow': json.loads(canonical_bytes(dataflow)), 'device': json.loads(canonical_bytes(device))}
(root / 'projection-oracle.json').write_text(json.dumps(rows, indent=2, sort_keys=True) + '\n')
print('projection SDK cases:', len(rows))
