#!/usr/bin/env python3
"""Freeze every SplitCandidate field and real Qwen splitter output through the SDK."""
from dataclasses import replace
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.splitter import (
    SplitCandidate, SplitterDescriptor, RoleExecutionPlan, RoleDependency, RoleResourceRequirement,
    TensorContract, ModelGraphSnapshot, GraphNodeView, TensorEdgeView, _canonical_bytes)
from ndnsf_distributed_inference.core.hybrid_contracts import HybridPlan, RedistributionEdge
from ndnsf_distributed_inference.adapters.base import AdapterPortDescriptor
from ndnsf_distributed_inference.adapters.qwen.placement import QwenThreeStageSplitter
from candidate_oracle_support import fixture_model, digest

model = fixture_model('Candidate模型', digest('content'), digest('graph'), 'fixture')
roles = ('front', 'end')
artifacts = {r: (digest(r),) for r in roles}
tensor = TensorContract('state', 'float32', ('batch', -1, 4), None)
base = SplitCandidate('PRE_SPLIT', SplitterDescriptor('fixture', '1', digest('split')),
    model, model.graph_digest, RoleExecutionPlan(roles, (RoleDependency('front', 'end', ('hidden',)),),
    {'z-node': 'front', 'a-node': 'end'}), {r: digest(r) for r in roles}, artifacts,
    {r: RoleResourceRequirement(('onnxruntime',), 1, 2, 3, 4, 5) for r in roles}, ('hidden',),
    {'unknown': None, 'signed': -7, 'large': 2**63, 'fraction': 1.25, 'negative-zero': -0.0},
    tensor_degrees_by_role={'front': 1, 'end': 1}, rank_artifact_digests_by_role=artifacts,
    role_state_inputs_by_role={r: (tensor,) for r in roles},
    role_state_outputs_by_role={r: (replace(tensor, name='out', estimated_bytes=16),) for r in roles},
    selection_priority=5, input_ingress_role='front', result_egress_role='end',
    merge_kind='NATIVE_POSTPROCESS', postprocessing={'unicode': '配置', 'nested': [True, None, {'threshold': 0.25}]})
redistribution = RedistributionEdge((0,), (1, 2), 'hidden', 'SCATTER', 'epoch', digest('integrity'),
    digest('layout-a'), digest('layout-b'), 64, True, -1)
ranked = dict(artifacts, end=(digest('end'), digest('end-rank1')))
hybrid = replace(base, artifacts_by_role=ranked, rank_artifact_digests_by_role=ranked,
    tensor_degrees_by_role={'front': 1, 'end': 2},
    hybrid_plan=HybridPlan(2, (1, 2), ('S0R0', 'S1R0', 'S1R1'), (redistribution,)))
values = [('complete', base), ('hybrid-scatter', hybrid),
          ('generated-nondeterministic', replace(base, source='GENERATED',
              splitter=replace(base.splitter, deterministic=False))),
          ('ordinary-no-rank-metadata', replace(base, tensor_degrees_by_role={}, rank_artifact_digests_by_role={})),
          ('unknown-resource', replace(base, requirements_by_role={
              r: replace(base.requirements_by_role[r], kv_bytes=None) for r in roles}))]
for name, degrees, operation in [('gather', (2, 1), 'GATHER'), ('reshard', (2, 2), 'RESHARD'),
                                  ('layout-reshard', (1, 1), 'RESHARD')]:
    artifacts = {r: tuple(digest(f'{r}-rank{i}') for i in range(degree)) for r, degree in zip(roles, degrees)}
    edge = replace(redistribution, producer_ranks=tuple(range(degrees[0])),
                   consumer_ranks=tuple(range(degrees[0], sum(degrees))), operation=operation)
    values.append(('hybrid-' + name, replace(base, artifacts_by_role=artifacts,
        rank_artifact_digests_by_role=artifacts, tensor_degrees_by_role=dict(zip(roles, degrees)),
        hybrid_plan=HybridPlan(2, degrees,
            tuple(f'S{s}R{r}' for s, d in enumerate(degrees) for r in range(d)), (edge,)))))
rows = [{'name': name, 'input': json.loads(_canonical_bytes(value)),
         'canonical_json': _canonical_bytes(value).decode(), 'candidate_digest': value.candidate_digest}
        for name, value in values]

# Real rank-one Qwen algorithm, with an explicitly supplied native strategy identity.
roles = ('front', 'end')
artifacts = {r: digest('qwen-' + r) for r in roles}
state = 'qwen-layer-split|1|' + ''.join(f'{r}:{i}-{i+1}:{artifacts[r]};' for i, r in enumerate(roles))
model = fixture_model('QwenFixture', digest('QwenFixture-content'), digest('qwen-graph'), 'qwen')
nodes = ('embedding', 'layer-00', 'layer-01', 'final-norm-head')
graph = ModelGraphSnapshot(model.graph_digest, model.adapter,
    tuple(GraphNodeView(n, 'op') for n in nodes),
    (TensorEdgeView('hidden-layer-0-to-1', 'layer-00', ('layer-01',), 'float32', (1,), 4),),
    nodes, ('hidden-layer-0-to-1',), (), ())
splitter = QwenThreeStageSplitter(AdapterPortDescriptor('fixture', '1', digest('port')),
    SplitterDescriptor('native-qwen-layer-split', '1', digest(state)), artifacts,
    {'front': 1, 'end': 1}, ((0, 1), (1, 2)), tensor_degrees=(1, 1), roles=roles)
value = splitter.enumerate_candidates(model, graph)[0]
rows.append({'name': 'qwen-default', 'input': json.loads(_canonical_bytes(value)),
             'canonical_json': _canonical_bytes(value).decode(), 'candidate_digest': value.candidate_digest})
(root / 'candidate-oracle.json').write_text(json.dumps(rows, indent=2, ensure_ascii=False) + '\n')
print(f'{len(rows)} maintained complete candidate cases generated')
