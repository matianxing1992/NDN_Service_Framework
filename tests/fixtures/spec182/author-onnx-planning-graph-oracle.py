#!/usr/bin/env python3
"""Freeze real ONNX source -> maintained planning graph and canonical identities."""
from dataclasses import asdict
import json
from pathlib import Path
import sys
import tempfile
import onnx
from onnx import TensorProto, helper

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from candidate_oracle_support import fixture_model, digest
from ndnsf_distributed_inference.adapters.onnx.graph import analyze_onnx_graph, to_model_graph_snapshot, canonical_onnx_identity
from ndnsf_distributed_inference.splitter import _canonical_bytes

adapter = fixture_model('OnnxFixture', digest('OnnxFixture-content'), digest('placeholder'), 'fixture').adapter
cases = []
for label, shape in [('static-branch', [1, 3]), ('dynamic-branch', ['batch', 3]), ('zero-branch', [0, 3])]:
    nodes = [helper.make_node('Add', ['images', 'bias'], ['中'], name='root'),
             helper.make_node('Identity', ['中'], ['left'], name='left'),
             helper.make_node('Relu', ['中'], ['right']),
             helper.make_node('Add', ['left', 'right'], ['predictions'], name='merge')]
    graph = helper.make_graph(nodes, label,
        [helper.make_tensor_value_info('images', TensorProto.FLOAT, shape),
         helper.make_tensor_value_info('bias', TensorProto.FLOAT, [1, 3])],
        [helper.make_tensor_value_info('predictions', TensorProto.FLOAT, shape)],
        [helper.make_tensor('bias', TensorProto.FLOAT, [1, 3], [1., 2., 3.])])
    cases.append((label, helper.make_model(graph, ir_version=8, opset_imports=[helper.make_opsetid('', 13)])))
for label, shape in [('scalar', []), ('unknown-dimension', [None, 3])]:
    graph = helper.make_graph([helper.make_node('Identity', ['x'], ['y'])], label,
        [helper.make_tensor_value_info('x', TensorProto.FLOAT, shape)],
        [helper.make_tensor_value_info('y', TensorProto.FLOAT, shape)])
    cases.append((label, helper.make_model(graph, ir_version=8, opset_imports=[helper.make_opsetid('', 13)])))
graph = helper.make_graph([helper.make_node('Mystery', ['x'], ['hidden'], domain='fixture'),
                           helper.make_node('Identity', ['hidden'], ['y'])], 'custom-unknown',
    [helper.make_tensor_value_info('x', TensorProto.FLOAT, [1, 3])],
    [helper.make_tensor_value_info('y', TensorProto.FLOAT, [1, 3])])
cases.append(('custom-unknown', helper.make_model(graph, ir_version=8,
    opset_imports=[helper.make_opsetid('', 13), helper.make_opsetid('fixture', 1)])))

rows = []
with tempfile.TemporaryDirectory() as temp:
    for label, model in cases:
        path = Path(temp) / (label + '.onnx')
        onnx.save(model, path)
        summary = analyze_onnx_graph(path)
        snapshot = to_model_graph_snapshot(summary, adapter)
        identity = canonical_onnx_identity(path)
        metadata = summary.to_dict()
        metadata.pop('modelPath')
        rows.append({'name': label, 'model_hex': path.read_bytes().hex(),
            'snapshot': asdict(snapshot), 'metadata_json': _canonical_bytes(metadata).decode(),
            'canonical_graph_digest': identity.graph_digest,
            'initializer_digest': identity.normalized_initializer_content_digest})
(root / 'onnx-planning-graph-oracle.json').write_text(json.dumps(rows, indent=2, ensure_ascii=False) + '\n')
print(f'{len(rows)} maintained ONNX {onnx.__version__} planning graph cases generated')
