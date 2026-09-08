#!/usr/bin/env python3
"""Freeze SDK placement using publicly reproducible signed test offers."""
import base64
import hashlib
from dataclasses import fields, replace
import json
from pathlib import Path
import sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

root = Path(__file__).parent
repo = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.sdk.placement import (
    RoleAssemblySpec, ProviderOfferV3, ProviderPlanningViewV3, DeviceTopologyProfile,
    DeviceResourceSnapshot, ResidencyProofV3, PlacementPlanCoreV3, canonical_digest)
from ndnsf_distributed_inference.planner.presplit_first import PreSplitFirstStrategy
from ndnsf_distributed_inference.app_sdk.provider import ProviderOfferTrustVerifier
from ndnsf_distributed_inference.adapters.onnx.executor import CertifiedOnnxAssemblyRecipe

signed = json.loads((root / 'signed-offer-oracle.json').read_text())
base = json.loads((root / 'sealer-python-oracle.json').read_text())['unsigned_core']
role = replace(RoleAssemblySpec(**base['roles'][0]), backend='onnxruntime', required_device_memory_mb=1024)
def ranked_role(rank):
    return replace(role, rank=rank, artifact_digest=(role.artifact_digest if rank == 0 else
        'sha256:' + hashlib.sha256(f'rank-artifact-{rank}'.encode()).hexdigest()))
key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
policy = signed['policy']
entry = dict(policy['entries'][0], provider='/provider/b',
             keyLocatorPrefix='/provider/b/KEY/fixture', certificateName='/provider/b/KEY/fixture/issuer/v=1')
policy['entries'].append(entry)
verifier = ProviderOfferTrustVerifier(policy, {signed['key_id']: signed['public_pem'].encode()},
                                    trust_schema_verifier=lambda ack: False)
cases = []
for name in ('cpu', 'loaded_second_device', 'assembled', 'canonical', 'insufficient_memory',
             'wrong_boot', 'wrong_topology', 'wrong_epoch', 'wrong_artifact', 'wrong_recipe',
             'missing_fence', 'rank_cover', 'has_model_only', 'expired_proof', 'future_proof'):
    roles = (role, ranked_role(1)) if name == 'rank_cover' else (role,)
    offers = []
    for provider in ('/provider/a', '/provider/b'):
        is_a = provider.endswith('/a')
        devices = () if name == 'cpu' else ('cuda:0', 'cuda:1')
        topology = DeviceTopologyProfile(provider, devices, 'onnxruntime-cpu' if not devices else 'onnxruntime-cuda')
        resources = tuple(DeviceResourceSnapshot(d, 8192,
            512 if is_a and name == 'insufficient_memory' else 4096,
            0, 1, 100, topology.digest()) for d in devices)
        proof = ResidencyProofV3(role.artifact_digest, role.role, 0, 'GPU', ('cuda:1',),
            'boot', 'process', topology.digest(), 100, 1000,
            residency_class='LOADED_RUNTIME', identity_digest=role.artifact_digest,
            assembly_spec_digest=role.recipe_digest, model_manifest_digest=role.model_manifest_digest,
            artifact_profile_digest=role.artifact_profile_digest, graph_digest=role.graph_digest,
            backend='onnxruntime-cuda', protection_epoch=role.protection_epoch,
            runtime_generation=1, fencing_token='fence')
        if name == 'assembled': proof = replace(proof, residency_class='ASSEMBLED_FRAGMENT', tier='RAM')
        if name == 'canonical': proof = replace(proof, residency_class='CANONICAL', tier='CANONICAL')
        mutations = {'wrong_boot': {'boot_epoch': 'foreign'}, 'wrong_topology': {'topology_digest': role.recipe_digest},
                     'wrong_epoch': {'protection_epoch': 'foreign'}, 'wrong_artifact': {'artifact_digest': role.recipe_digest},
                     'wrong_recipe': {'assembly_spec_digest': role.artifact_digest}, 'missing_fence': {'fencing_token': ''},
                     'expired_proof': {'expires_at_ms': 150}, 'future_proof': {'captured_at_ms': 300}}
        proof = replace(proof, **mutations.get(name, {}))
        has_proof = is_a and name not in ('cpu', 'rank_cover', 'has_model_only')
        exact = is_a and name not in ('cpu', 'rank_cover', 'canonical')
        offer = ProviderOfferV3('request', 1, '/service', provider, base['model_digest'], base['graph_digest'],
            True, 'ACCEPT_IF_EXACT_REUSE' if exact else 'ACCEPT_WITH_PREPARATION', not exact, topology,
            resources=resources, residency=(proof,) if has_proof else (), accepted_roles=(role.role,),
            backends=(topology.backend,), boot_epoch='boot', captured_at_ms=100, expires_at_ms=1000,
            signer_key_id=signed['key_id'], signature='pending', has_model=True)
        offer = replace(offer, signature=base64.b64encode(key.sign(offer.digest().encode())).decode())
        assert verifier(offer)
        offers.append(offer)
    if name in ('has_model_only', 'expired_proof', 'future_proof'): offers = offers[:1]
    views = tuple(ProviderPlanningViewV3.from_offer(o, verify_signature=verifier) for o in offers)
    native_freshness = name in ('expired_proof', 'future_proof')
    expected = None
    try:
        proposal = PreSplitFirstStrategy().propose_v3(request_id='request', attempt=1,
            model_digest=base['model_digest'], graph_digest=base['graph_digest'], roles=roles,
            providers=views, ack_closed_digest=base['ack_closed_digest'])
        expected = {'providers': dict(proposal.provider_by_role), 'roles': [
            {'role': r.role, 'rank': r.rank, 'backend': r.backend, 'device_set': r.device_set} for r in proposal.roles]}
    except ValueError:
        pass
    cases.append({'name': name, 'ranks': [r.rank for r in roles], 'offers': [o.to_bytes().decode() for o in offers],
                  'expected': None if native_freshness else expected, 'native_freshness': native_freshness})
strategy = {'name': 'bridge-fixture', 'version': '1',
            'state': 'sha256:' + hashlib.sha256(b'bridge-strategy').hexdigest()}
seal_cases = []
for original in (c for c in cases if c['name'] in ('cpu', 'loaded_second_device', 'rank_cover')):
    offers = []
    for wire in original['offers']:
        offer = ProviderOfferV3.from_bytes(wire.encode())
        offer = replace(offer, expires_at_ms=2000000000000,
                        residency=tuple(replace(p, expires_at_ms=2000000000000) for p in offer.residency))
        offer = replace(offer, signature=base64.b64encode(key.sign(offer.digest().encode())).decode())
        assert verifier(offer)
        offers.append(offer)
    selected = PreSplitFirstStrategy().propose_v3(request_id='request', attempt=1,
        model_digest=base['model_digest'], graph_digest=base['graph_digest'],
        roles=tuple(ranked_role(rank) for rank in original['ranks']),
        providers=tuple(ProviderPlanningViewV3.from_offer(o, verify_signature=verifier) for o in offers),
        ack_closed_digest=base['ack_closed_digest'])
    core = PlacementPlanCoreV3(request_id='request', attempt=1, model_digest=base['model_digest'],
        graph_digest=base['graph_digest'], roles=selected.roles, provider_by_role=selected.provider_by_role,
        dependencies=(), ack_closed_digest=base['ack_closed_digest'], strategy_digest=canonical_digest(strategy),
        candidate_digest='sha256:' + hashlib.sha256(b'placed-candidate').hexdigest())
    seal_cases.append(dict(original, offers=[o.to_bytes().decode() for o in offers],
                           deadline_ms=2000000000000, core_digest=core.digest()))
    if original['name'] == 'cpu':
        # The maintained YOLO coordinator certifies the canonical ONNX graph
        # after planning. The top-level planning digest stays unchanged.
        canonical_graph = 'sha256:' + hashlib.sha256(b'canonical-onnx-graph').hexdigest()
        distinct = replace(core, roles=tuple(replace(r, graph_digest=canonical_graph) for r in core.roles))
        seal_cases.append(dict(seal_cases[-1], name='distinct_graph_spaces',
                               canonical_graph_digest=canonical_graph, core_digest=distinct.digest()))
    # Real SDK recipe certification after publication of a new business root.
    # Source bytes/names are transport fixtures, not ONNX assembly qualification.
    metadata = {'canonicalSourceBytes': 6, 'canonicalSourceDataName': '/encrypted/source',
                'canonicalSourceDigest': 'sha256:' + hashlib.sha256(b'source').hexdigest(),
                'packageManifestDigest': role.model_manifest_digest}
    if original['name'] == 'rank_cover':
        metadata.update(canonicalInitializerBytes=7, canonicalInitializerDataName='/encrypted/initializer',
            canonicalInitializerObjectDigest='sha256:' + hashlib.sha256(b'weights').hexdigest())
    root_wire = json.dumps({'artifactProfileDigest': role.artifact_profile_digest, 'metadata': metadata,
        'modelIdentityDigest': base['model_digest'], 'modelName': 'QwenFixture',
        'schema': 'ndnsf-di-canonical-model-manifest-v1', 'state': 'ACTIVE'},
        sort_keys=True, separators=(',', ':'), ensure_ascii=False)
    manifest_digest = 'sha256:' + hashlib.sha256(root_wire.encode()).hexdigest()
    refreshed = []
    for placed in core.roles:
        values = {f.name: getattr(placed, f.name) for f in fields(CertifiedOnnxAssemblyRecipe)
                  if hasattr(placed, f.name)}
        values.update(model_manifest_digest=manifest_digest,
            input_names=tuple(t['name'] for t in placed.expected_inputs),
            output_names=tuple(t['name'] for t in placed.expected_outputs),
            max_source_bytes=placed.resource_envelope['maxSourceBytes'],
            max_assembled_bytes=placed.resource_envelope['maxAssembledBytes'],
            max_nodes=placed.resource_envelope['maxNodes'])
        recipe = CertifiedOnnxAssemblyRecipe(**values)
        refreshed.append(replace(placed, model_manifest_digest=manifest_digest, recipe_digest=recipe.digest))
    updated = replace(core, roles=tuple(refreshed))
    seal_cases.append(dict(original, name='published_' + original['name'],
        offers=[o.to_bytes().decode() for o in offers], deadline_ms=2000000000000,
        root_json=root_wire, published_manifest_digest=manifest_digest,
        published_recipe_digests=[r.recipe_digest for r in refreshed], core_digest=updated.digest(),
        publication_reject=original['name'] == 'loaded_second_device'))
(root / 'placement-v3-oracle.json').write_text(json.dumps({'policy': policy, 'public_pem': signed['public_pem'],
    'key_id': signed['key_id'], 'candidate': signed['candidate'], 'role': base['roles'][0],
    'model_digest': base['model_digest'], 'graph_digest': base['graph_digest'],
    'ack_digest': base['ack_closed_digest'], 'cases': cases, 'strategy': strategy,
    'seal_cases': seal_cases}, indent=2, sort_keys=True) + '\n')
