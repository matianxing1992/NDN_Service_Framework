#!/usr/bin/env python3
"""Freeze maintained YOLO semantic splitter results from real ONNX bytes."""
import json
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from candidate_oracle_support import fixture_model, digest
from ndnsf_distributed_inference.adapters.base import AdapterPortDescriptor
from ndnsf_distributed_inference.adapters.onnx.graph import analyze_onnx_graph, to_model_graph_snapshot
from ndnsf_distributed_inference.adapters.yolo.adapter import Yolo26Splitter
from ndnsf_distributed_inference.adapters.yolo.candidates import RegisteredYoloCandidate
from ndnsf_distributed_inference.splitter import SplitterDescriptor, _canonical_bytes

source = json.loads((root / 'onnx-planning-graph-oracle.json').read_text())[0]['model_hex']
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp) / 'semantic.onnx'
    path.write_bytes(bytes.fromhex(source))
    summary = analyze_onnx_graph(path)
    adapter = fixture_model('YOLOFixture', digest('YOLOFixture-content'), digest('placeholder'), 'yolo26n').adapter
    graph = to_model_graph_snapshot(summary, adapter)
    model = fixture_model('YOLOFixture', digest('YOLOFixture-content'), graph.graph_digest, 'yolo26n')
    metadata = summary.to_dict()
    names = [node['name'] for node in metadata['nodes']]
    roles = ['Front', 'Branch', 'Merge']
    owners = dict(zip(graph.topological_order, ['Front', 'Branch', 'Branch', 'Merge']))
    name_by_id = dict(zip(graph.topological_order, names))
    interfaces, dependencies = [], {}
    for edge in graph.edges:
        consumers = [n for n in edge.consumers if owners[n] != owners[edge.producer]]
        if not consumers:
            continue
        consumer_roles = sorted({owners[n] for n in consumers})
        interfaces.append(dict(edgeId=edge.edge_id, producerNode=name_by_id[edge.producer],
            producerRole=owners[edge.producer], consumerNodes=[name_by_id[n] for n in consumers],
            consumerRoles=consumer_roles, dtype=edge.dtype, shape=list(edge.shape)))
        for role in consumer_roles:
            dependencies.setdefault((owners[edge.producer], role), []).append(edge.edge_id)
    def endpoint(name):
        info = metadata['tensors'][name]
        return dict(name=name, dtype=info['dtype'], shape=info['shape'])
    partition = dict(roleNodeSets={'Front': names[:1], 'Branch': names[1:3], 'Merge': names[3:]},
        tensorInterfaces=interfaces,
        dependencyEdges=[dict(fromRole=p, toRole=c, tensorEdges=v) for (p, c), v in dependencies.items()],
        safeCuts=[dict(fromRole=p, toRole=c, boundaryTensors=v) for (p, c), v in dependencies.items()],
        roleInterfaces={role: dict(inputs=[endpoint(n) for n in ins], outputs=[endpoint(n) for n in outs])
            for role, ins, outs in [('Front', ['images'], ['中']), ('Branch', ['中'], ['left', 'right']),
                                    ('Merge', ['left', 'right'], ['predictions'])]})
    registered = RegisteredYoloCandidate('semantic-v1', 1, tuple({'role': r} for r in roles),
        'Front', 'Merge', 'NATIVE_POSTPROCESS', (), digest('semantic-registered'), partition)
    splitter = Yolo26Splitter(AdapterPortDescriptor('fixture', '1', digest('port')),
        SplitterDescriptor('native-yolo-component-split', '1', digest('yolo-component-split|1')),
        (registered,), graph_node_names=tuple(names), graph_metadata=metadata)
    candidate = splitter.enumerate_candidates(model, graph)[0]
    result = dict(model_hex=source, graph_digest=graph.graph_digest, registered_digest=registered.candidate_digest,
        partition=partition, node_roles=owners, candidate_json=_canonical_bytes(candidate).decode(),
        candidate_digest=candidate.candidate_digest)
    (root / 'yolo-semantic-oracle.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
print('Actual ONNX -> maintained Yolo26Splitter semantic candidate generated')
