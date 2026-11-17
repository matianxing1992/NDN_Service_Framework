"""Explicit test descriptor inputs shared by maintained SDK oracle authors."""
import hashlib
from dataclasses import replace
from ndnsf_distributed_inference.splitter import AdapterDescriptor, ModelDescriptor
from ndnsf_distributed_inference.splitter import _canonical_bytes


def digest(label):
    return 'sha256:' + hashlib.sha256(label.encode()).hexdigest()


def integer_attribute_graph(model, summary, snapshot):
    """Independent ONNX attribute extraction for the native planning identity.

    The historical Python graph view predates integer-attribute binding. Keep
    its topology/tensor analysis, and bind the actual source attributes here;
    never obtain expected identities from the C++ implementation under test.
    """
    from onnx import AttributeProto

    metadata = summary.to_dict()
    metadata.pop('modelPath')
    for node, source in zip(metadata['nodes'], model.graph.node):
        node['integerAttributes'] = {
            attr.name: attr.i for attr in source.attribute if attr.type == AttributeProto.INT}
    identity = dict(metadata)
    identity['adapter_descriptor_digest'] = snapshot.adapter.descriptor_digest
    identity['tensor_producers'] = identity.pop('tensorProducers')
    identity['tensor_consumers'] = identity.pop('tensorConsumers')
    return replace(snapshot, graph_digest=digest(_canonical_bytes(identity).decode())), metadata


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
