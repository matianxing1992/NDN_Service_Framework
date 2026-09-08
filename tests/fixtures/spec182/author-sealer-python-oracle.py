#!/usr/bin/env python3
"""Freeze actual SDK core/security identity; authoring only, never native runtime."""
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'NDNSF-DistributedInference'))
from ndnsf_distributed_inference.sdk.placement import (
    RoleAssemblySpec, PlacementPlanCoreV3, GrantBindingV1, PlanSealerV3, canonical_digest)

def digest(value):
    return 'sha256:' + hashlib.sha256(value.encode()).hexdigest()

role = RoleAssemblySpec(
    role='/LLM/Pipeline/Stage/0', rank=0, layer_begin=0, layer_end=1,
    recipe_digest=digest('oracle-recipe'), artifact_digest=digest('oracle-artifact'),
    backend='onnxruntime-cpu', adapter_id='qwen', adapter_version='1',
    role_kind='PIPELINE_RANGE', model_manifest_digest=digest('oracle-manifest'),
    artifact_profile_digest=digest('fixture-profile'), graph_digest=digest('oracle-graph'),
    canonical_initializer_digest=digest('fixture-initializers'),
    adapter_descriptor_digest=digest('fixture-adapter'),
    assembler_descriptor_digest=digest('fixture-assembler'), backend_abi='onnxruntime-cpu-v1',
    node_indices=(0,), expected_inputs=({'name': 'x', 'dtype': 'float32', 'shape': [1, 'batch']},),
    expected_outputs=({'name': 'y', 'dtype': 'float32', 'shape': [1, 'batch']},), precision='fp32',
    resource_envelope={'maxSourceBytes': 4096, 'maxAssembledBytes': 8192, 'maxNodes': 16},
    protection_epoch='protected-v1')
core = PlacementPlanCoreV3(
    request_id='oracle-request', attempt=1, model_digest=digest('QwenFixture-content'),
    graph_digest=digest('oracle-graph'), roles=(role,),
    provider_by_role={role.role: 'provider-a'}, dependencies=(),
    ack_closed_digest=digest('oracle-ack'), candidate_digest=digest('candidate'),
    strategy_digest=canonical_digest({'name': 'test-placement', 'version': '1', 'state': digest('strategy')}))
grant = GrantBindingV1(provider='provider-a', grant_name='/grant/1', grant_digest=digest('grant'),
    request_id=core.request_id, attempt=core.attempt, plan_core_digest=core.digest(),
    security_policy_snapshot_digest=digest('policy'), protection_epoch='protected-v1')
result = {'schema': 'spec182-real-sdk-sealer-oracle-v1', 'core_digest': core.digest(),
          'plan_digest': PlanSealerV3.finalize_security(core, (grant,), digest('policy')),
          'unsigned_core': core.unsigned_dict()}
Path(__file__).with_name('sealer-python-oracle.json').write_text(
    json.dumps(result, indent=2, sort_keys=True) + '\n')
