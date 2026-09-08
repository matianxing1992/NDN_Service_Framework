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
from ndnsf_distributed_inference.splitter import AdapterDescriptor, ModelDescriptor, ModelGraphSnapshot, GraphNodeView, SplitterDescriptor

def digest(label):
    return 'sha256:' + hashlib.sha256(label.encode()).hexdigest()

adapter_values = json.loads((root / 'model-descriptor-oracle.json').read_text())[0]['model']['adapter']
adapter = AdapterDescriptor(**adapter_values)
nodes = ('z-node', 'a-node', '中-node')
graph_digest = digest('yolo-fragment-graph')
graph = ModelGraphSnapshot(graph_digest, adapter, tuple(GraphNodeView(n, 'Identity') for n in nodes),
                           (), nodes, (), (), ())
model = ModelDescriptor('YOLOFixture', digest('content'), digest('semantics'), graph_digest,
                        'onnx', 'float32', adapter)
rows = []
for label in ('registered-one', 'registered-two'):
    registered = RegisteredYoloCandidate('atomic-v1', 1, ({'role': 'FullModel'},),
        'FullModel', 'FullModel', 'NATIVE_POSTPROCESS', (), digest(label))
    splitter = Yolo26Splitter(AdapterPortDescriptor('fixture', '1', digest('port')),
        SplitterDescriptor('fixture', '1', digest('splitter')), (registered,))
    candidate = splitter.enumerate_candidates(model, graph)[0]
    requirement = candidate.requirements_by_role['FullModel']
    rows.append({'registered_digest': registered.candidate_digest, 'graph_digest': graph_digest,
        'nodes': nodes, 'fragment_digest': candidate.fragments_by_role['FullModel'],
        'backends': requirement.backends, 'weight_bytes': requirement.weight_bytes,
        'safety_margin': requirement.safety_margin, 'merge_kind': candidate.merge_kind})
(root / 'yolo-fragment-oracle.json').write_text(json.dumps(rows, indent=2, ensure_ascii=False) + '\n')
print(f'{len(rows)} actual Yolo26Splitter candidates generated')
