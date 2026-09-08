#!/usr/bin/env python3
"""Freeze maintained metadata graph builder and Qwen splitter results."""
from dataclasses import asdict, replace
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from candidate_oracle_support import fixture_model, digest
from ndnsf_distributed_inference.adapters.qwen.placement import build_qwen_three_stage_adapter
from ndnsf_distributed_inference.splitter import SplitterDescriptor, _canonical_bytes

roles = ('front', 'end')
artifacts = {r: digest('qwen-' + r) for r in roles}
rows = []
for ranges in (((0, 1), (1, 2)), ((0, 5), (5, 12))):
    adapter = build_qwen_three_stage_adapter(model_name='QwenFixture', revision='pinned-r1',
        layer_ranges=ranges, artifact_digests_by_role=artifacts, weight_bytes_by_role={r: 1 for r in roles},
        tensor_degrees=(1, 1), precision='float32', adapter_name='qwen', stage_roles=roles)
    graph = adapter.graph.snapshot
    model = fixture_model('QwenFixture', digest('QwenFixture-content'), graph.graph_digest, 'qwen')
    graph = replace(graph, adapter=model.adapter)
    state = 'qwen-layer-split|1|' + ''.join(f'{r}:{start}-{end}:{artifacts[r]};'
        for r, (start, end) in zip(roles, ranges))
    splitter = replace(adapter.splitter, splitter_descriptor=SplitterDescriptor('native-qwen-layer-split', '1', digest(state)))
    candidate = splitter.enumerate_candidates(model, graph)[0]
    rows.append(dict(ranges=ranges, graph=asdict(graph), candidate_json=_canonical_bytes(candidate).decode(),
                     candidate_digest=candidate.candidate_digest))
(root / 'qwen-metadata-oracle.json').write_text(json.dumps(rows, indent=2) + '\n')
print('2 maintained Qwen metadata -> candidate oracles generated')
