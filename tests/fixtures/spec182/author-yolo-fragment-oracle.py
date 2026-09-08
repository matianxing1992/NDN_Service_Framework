#!/usr/bin/env python3
"""Run the maintained YOLO splitter to freeze fragment/resource expectations."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.adapters.base import AdapterPortDescriptor
from ndnsf_distributed_inference.adapters.yolo.adapter import Yolo26Splitter
from ndnsf_distributed_inference.adapters.yolo.candidates import RegisteredYoloCandidate
from ndnsf_distributed_inference.splitter import ModelGraphSnapshot, GraphNodeView, SplitterDescriptor, _canonical_bytes
from candidate_oracle_support import fixture_model

def digest(label):
    return 'sha256:' + hashlib.sha256(label.encode()).hexdigest()

nodes = ('z-node', 'a-node', '中-node')
graph_digest = digest('yolo-fragment-graph')
model = fixture_model('YOLOFixture', digest('YOLOFixture-content'), graph_digest, 'yolo26n')
graph = ModelGraphSnapshot(graph_digest, model.adapter, tuple(GraphNodeView(n, 'Identity') for n in nodes),
                           (), nodes, (), (), ())
rows = []
for label in ('registered-one', 'registered-two'):
    registered = RegisteredYoloCandidate('atomic-v1', 1, ({'role': 'FullModel'},),
        'FullModel', 'FullModel', 'NATIVE_POSTPROCESS', (), digest(label))
    splitter = Yolo26Splitter(AdapterPortDescriptor('fixture', '1', digest('port')),
        SplitterDescriptor('native-yolo-component-split', '1', digest('yolo-component-split|1')), (registered,))
    candidate = splitter.enumerate_candidates(model, graph)[0]
    requirement = candidate.requirements_by_role['FullModel']
    rows.append({'registered_digest': registered.candidate_digest, 'graph_digest': graph_digest,
        'nodes': nodes, 'fragment_digest': candidate.fragments_by_role['FullModel'],
        'backends': requirement.backends, 'weight_bytes': requirement.weight_bytes,
        'safety_margin': requirement.safety_margin, 'merge_kind': candidate.merge_kind,
        'candidate_json': _canonical_bytes(candidate).decode(), 'candidate_digest': candidate.candidate_digest})
(root / 'yolo-fragment-oracle.json').write_text(json.dumps(rows, indent=2, ensure_ascii=False) + '\n')
print(f'{len(rows)} actual Yolo26Splitter candidates generated')
