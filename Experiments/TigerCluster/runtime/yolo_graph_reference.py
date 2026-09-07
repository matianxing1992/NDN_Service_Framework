"""Prepare model-node expectations without reading measured execution traces.

Internal preparation component, not release authority. The caller must obtain
model bytes/digest from the certified DI assembler and bind the returned record
to the actual runtime and publication manifest before using it in collection.
Never call this on the login node. CUDA preparation belongs on its allocated
device, outside the measured request window.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import tempfile


def _digest(payload):
    return 'sha256:' + hashlib.sha256(payload).hexdigest()


def prepare_role_reference(model_bytes: bytes, *, artifact_digest: str,
                           model_manifest_digest: str, role: str,
                           backend: str, ort_version: str, scratch: Path) -> dict:
    """Initialize an independent ORT session, export its graph, never run it.

    Supports inline assembled ONNX only. External-data references are rejected
    rather than resolved against an arbitrary cwd. Temporary optimized models
    may contain plaintext weights, so they stay in a private temporary directory
    and are never copied into evidence. Only hashes/node identities leave it.

    The settings mirror makeSessionOptions in OnnxRuntimeModelRunner.cpp;
    a source-parity regression guards this boundary. Runtime/version/backend
    provenance and native-library parity remain the enclosing caller's duty.
    """
    if role not in ('BackboneNeck', 'DetectShard0', 'DetectShard1'):
        raise ValueError('GRAPH_REFERENCE_ROLE')
    if (not isinstance(model_bytes, bytes) or not 0 < len(model_bytes) <= 32 * 1024 * 1024
            or _digest(model_bytes) != artifact_digest):
        raise ValueError('GRAPH_REFERENCE_MODEL_BYTES')
    if (not isinstance(model_manifest_digest, str)
            or re.fullmatch(r'sha256:[0-9a-f]{64}', model_manifest_digest) is None):
        raise ValueError('GRAPH_REFERENCE_MANIFEST')
    if backend not in ('CPUExecutionProvider', 'CUDAExecutionProvider'):
        raise ValueError('GRAPH_REFERENCE_BACKEND')
    scratch = Path(scratch)
    if (not scratch.is_absolute() or not scratch.is_dir()
            or any(p.is_symlink() for p in (scratch, *scratch.parents))):
        raise ValueError('GRAPH_REFERENCE_SCRATCH')
    import onnx
    import onnxruntime as ort
    if not isinstance(ort_version, str) or ort.__version__ != ort_version:
        raise ValueError('GRAPH_REFERENCE_ORT_VERSION')
    if backend not in ort.get_available_providers():
        raise ValueError('GRAPH_REFERENCE_BACKEND_UNAVAILABLE')
    model = onnx.load_model_from_string(model_bytes)
    # Traverse all protobuf fields: tensors can appear in attributes/subgraphs,
    # not only in graph.initializer. No external filesystem reads are allowed.
    def reject_external(message):
        if message.DESCRIPTOR.full_name == 'onnx.TensorProto':
            if message.data_location == onnx.TensorProto.EXTERNAL or message.external_data:
                raise ValueError('GRAPH_REFERENCE_EXTERNAL_DATA')
        for field, value in message.ListFields():
            if field.message_type is not None:
                if field.label == field.LABEL_REPEATED:
                    for child in value:
                        reject_external(child)
                else:
                    reject_external(value)
    reject_external(model)
    if (not 0 < len(model.graph.node) <= 10000
            or any(attr.type in (onnx.AttributeProto.GRAPH, onnx.AttributeProto.GRAPHS)
                   for node in model.graph.node for attr in node.attribute)):
        raise ValueError('GRAPH_REFERENCE_GRAPH_SCOPE')
    onnx.checker.check_model(model)
    with tempfile.TemporaryDirectory(prefix='ort-reference-', dir=str(scratch)) as directory:
        optimized = Path(directory) / 'optimized.onnx'
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
        options.optimized_model_filepath = str(optimized)
        providers = [backend]
        if backend == 'CUDAExecutionProvider':
            options.add_session_config_entry('session.disable_cpu_ep_fallback', '1')
            providers = [(backend, {'device_id': 0})]
        session = ort.InferenceSession(model_bytes, sess_options=options, providers=providers)
        if session.get_providers()[0] != backend:
            raise ValueError('GRAPH_REFERENCE_BACKEND_FALLBACK')
        session.disable_fallback()
        del session
        payload = optimized.read_bytes()
        if not 0 < len(payload) <= 64 * 1024 * 1024:
            raise ValueError('GRAPH_REFERENCE_OPTIMIZED_SIZE')
        graph = onnx.load_model_from_string(payload).graph
        names = [node.name for node in graph.node]
        if (not names or len(names) > 10000 or any(not name for name in names)
                or len(set(names)) != len(names)):
            raise ValueError('GRAPH_REFERENCE_NODE_IDENTITIES')
        return dict(schema='tiger-yolo-role-reference-v1', role=role,
            qualification='ORT_GRAPH_PREPARATION_COMPONENT_ONLY',
            ortVersion=ort.__version__, optimizedModelDigest=_digest(payload),
            sessionOptions=dict(intraOpThreads=1, graphOptimization='ORT_ENABLE_BASIC',
                                allowCpuFallback=False, deviceId=0),
            expected=dict(modelManifestDigest=model_manifest_digest,
                          artifactDigest=artifact_digest, backend=backend,
                          optimizedNodeNames=[name + '_kernel_time' for name in names]))
