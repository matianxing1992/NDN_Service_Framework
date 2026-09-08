"""Explicit test descriptor inputs shared by maintained SDK oracle authors."""
import hashlib
from ndnsf_distributed_inference.splitter import AdapterDescriptor, ModelDescriptor


def digest(label):
    return 'sha256:' + hashlib.sha256(label.encode()).hexdigest()


def fixture_model(name, content, graph, adapter_id, adapter_version='1',
                  precision='float32', semantics=None):
    adapter = AdapterDescriptor(
        name=adapter_id, version=adapter_version, state_digest=digest('fixture-adapter-state'),
        abi='fixture-abi-v1', model_formats=('onnx',), tasks=('task',),
        backends=('onnxruntime',), precisions=(precision,),
        input_schema_digest=digest('fixture-input-schema'), options_schema_digest=digest('fixture-options-schema'),
        result_schema_digest=digest('fixture-result-schema'), graph_schema_digest=digest('fixture-graph-schema'),
        split_schema_digest=digest('fixture-split-schema'), state_schema_digest=digest('fixture-state-schema'),
        graph_inspectable=True, splittable=True)
    return ModelDescriptor(name, content, semantics or digest(name + '-semantics'), graph,
                           'onnx', precision, adapter)
