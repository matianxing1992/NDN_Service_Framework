#!/usr/bin/env python3
"""Freeze complete model/adapter canonical identities from maintained Python types."""
from dataclasses import asdict, replace
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'), str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.splitter import AdapterDescriptor, ModelDescriptor, _canonical_bytes

def digest(label):
    return 'sha256:' + hashlib.sha256(label.encode()).hexdigest()

adapter = AdapterDescriptor(
    name='fixture-适配器', version='2', state_digest=digest('state'), abi='adapter-abi-v2',
    model_formats=('onnx', 'fixture'), tasks=('generate', 'detect'),
    backends=('onnxruntime-cpu', 'onnxruntime-cuda'), precisions=('float32', 'float16'),
    input_schema_digest=digest('input'), options_schema_digest=digest('options'),
    result_schema_digest=digest('result'), graph_schema_digest=digest('graph-schema'),
    split_schema_digest=digest('split-schema'), state_schema_digest=digest('state-schema'),
    graph_inspectable=True, splittable=True, deterministic_analysis=True)
model = ModelDescriptor('Fixture/模型', digest('content'), digest('semantics'), digest('graph'),
                        'onnx', 'float32', adapter, source_revision='revision-α')
cases = []
for name, value in (
    ('complete', model),
    ('revision', replace(model, source_revision='revision-β')),
    ('schema', replace(model, adapter=replace(adapter, state_schema_digest=digest('state-schema-v2')))),
    ('abi', replace(model, adapter=replace(adapter, abi='adapter-abi-v3'))),
    ('capability-order', replace(model, adapter=replace(adapter, backends=tuple(reversed(adapter.backends))))),
    ('booleans', replace(model, adapter=replace(adapter, graph_inspectable=False, splittable=False, deterministic_analysis=False))),
):
    cases.append({'name': name, 'model': asdict(value),
                  'adapter_json': _canonical_bytes(value.adapter).decode(),
                  'model_json': _canonical_bytes(value).decode(),
                  'adapter_digest': value.adapter.descriptor_digest, 'model_digest': value.model_digest})
(root / 'model-descriptor-oracle.json').write_text(json.dumps(cases, indent=2, ensure_ascii=False) + '\n')
print(f'{len(cases)} maintained Python model descriptor cases generated')
