"""Prepare model-node expectations without reading measured execution traces.

Internal preparation component, not release authority. The caller must obtain
model bytes/digest from the certified DI assembler and bind the returned record
to the actual runtime and publication manifest before using it in collection.
Never call this on the login node. CUDA preparation belongs on its allocated
device, outside the measured request window.
"""
from __future__ import annotations

import hashlib
import json
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


_ORT_ROLES = frozenset(('BackboneNeck', 'DetectShard0', 'DetectShard1'))
_REFERENCE_SCHEMA = 'tiger-yolo-role-reference-v1'
_CERTIFIED_GRAPH_SCHEMA = 'tiger-yolo-certified-graph-v1'
_REFERENCE_QUALIFICATION = 'ORT_GRAPH_PREPARATION_COMPONENT_ONLY'
_BACKENDS = ('CPUExecutionProvider', 'CUDAExecutionProvider')
_CANONICAL_SESSION_OPTIONS = dict(intraOpThreads=1,
                                  graphOptimization='ORT_ENABLE_BASIC',
                                  allowCpuFallback=False, deviceId=0)
_REFERENCE_FIELDS = {'schema', 'role', 'qualification', 'ortVersion',
                     'optimizedModelDigest', 'sessionOptions', 'expected'}
_EXPECTED_FIELDS = {'modelManifestDigest', 'artifactDigest', 'backend',
                    'optimizedNodeNames'}
_PROVENANCE_FIELDS = {'schema', 'qualification', 'ortVersion',
                      'optimizedModelDigest', 'sessionOptions', 'backend'}


def _role_reference_expected(record, role):
    """Validate one role-reference record and return its expected block."""
    if not isinstance(record, dict) or set(record) != _REFERENCE_FIELDS:
        raise ValueError('CERTIFIED_GRAPH_REFERENCE_SCHEMA')
    if (record['schema'] != _REFERENCE_SCHEMA or record['role'] != role
            or record['qualification'] != _REFERENCE_QUALIFICATION):
        raise ValueError('CERTIFIED_GRAPH_REFERENCE_PROVENANCE')
    if not isinstance(record['ortVersion'], str) or not record['ortVersion']:
        raise ValueError('CERTIFIED_GRAPH_REFERENCE_ORT_VERSION')
    if (not isinstance(record['optimizedModelDigest'], str)
            or re.fullmatch(r'sha256:[0-9a-f]{64}',
                            record['optimizedModelDigest']) is None):
        raise ValueError('CERTIFIED_GRAPH_REFERENCE_OPTIMIZED_DIGEST')
    if (not isinstance(record['sessionOptions'], dict)
            or dict(record['sessionOptions']) != _CANONICAL_SESSION_OPTIONS):
        raise ValueError('CERTIFIED_GRAPH_REFERENCE_SESSION_OPTIONS')
    expected = record['expected']
    if not isinstance(expected, dict) or set(expected) != _EXPECTED_FIELDS:
        raise ValueError('CERTIFIED_GRAPH_ROLE_SCHEMA')
    if any(not isinstance(expected[k], str)
           or re.fullmatch(r'sha256:[0-9a-f]{64}', expected[k]) is None
           for k in ('modelManifestDigest', 'artifactDigest')):
        raise ValueError('CERTIFIED_GRAPH_ROLE_SCHEMA')
    if expected['backend'] not in _BACKENDS:
        raise ValueError('CERTIFIED_GRAPH_ROLE_BACKEND')
    names = expected['optimizedNodeNames']
    if (not isinstance(names, list) or not names or len(names) > 10000
            or any(not isinstance(name, str) or not name for name in names)
            or len(set(names)) != len(names)):
        raise ValueError('CERTIFIED_GRAPH_NODE_VOCABULARY')
    return expected


def serialize_certified_graph(role_references, *, graph_digest) -> dict:
    """Serialize per-role ORT preparation records into the certified graph.

    This is the production serializer for ``tiger-yolo-certified-graph-v1``,
    the document shape the retained comparator accepts. It consumes role
    reference records exactly as ``prepare_role_reference`` returns them and
    never reads execution observations. All three ONNX roles must be present:
    a certified graph that silently drops one role would let a missing head
    evade coverage. One backend is required across roles because the optimized
    node vocabulary is backend-specific; the caller prepares references with
    the same runtime/backend that executes the roles (CPU locally, CUDA on the
    allocated device, never on the login node). Reference provenance (producer
    schema/qualification, exact ORT version, optimizer-export digest and
    session options) is embedded in the returned document so collection can
    recheck it before invoking the comparator.
    """
    if (not isinstance(graph_digest, str)
            or re.fullmatch(r'sha256:[0-9a-f]{64}', graph_digest) is None):
        raise ValueError('CERTIFIED_GRAPH_GRAPH_DIGEST')
    if not isinstance(role_references, dict) or set(role_references) != _ORT_ROLES:
        raise ValueError('CERTIFIED_GRAPH_ROLE_COVERAGE')
    expected_by_role = {}
    backends = set()
    for role in sorted(role_references):
        expected = _role_reference_expected(role_references[role], role)
        expected_by_role[role] = expected
        backends.add(expected['backend'])
    if len(backends) != 1:
        raise ValueError('CERTIFIED_GRAPH_BACKEND_BINDING')
    return dict(
        schema=_CERTIFIED_GRAPH_SCHEMA, graphDigest=graph_digest,
        roles={role: dict(expected_by_role[role]) for role in sorted(expected_by_role)},
        referenceProvenance={
            role: dict(schema=record['schema'], qualification=record['qualification'],
                       ortVersion=record['ortVersion'],
                       optimizedModelDigest=record['optimizedModelDigest'],
                       sessionOptions=dict(record['sessionOptions']),
                       backend=expected_by_role[role]['backend'])
            for role, record in sorted(role_references.items())})


def validate_certified_graph_provenance(certified_graph) -> dict:
    """Recheck the retained producer provenance before any coverage comparison.

    The retained comparator calls this before comparing optimized node names so
    that an expected graph fabricated at collection time, or stripped of its
    producer identity, is rejected instead of trusted. It checks the
    provenance record that ``serialize_certified_graph`` embeds for every role
    the document covers: producer schema/qualification, exact ORT version,
    optimizer-export digest, canonical session options and declared backend
    consistency with the role expectation. Re-deriving the optimized node
    vocabulary from the export digest requires re-running preparation offline;
    mutation audits do that. This function never reads execution observations.
    """
    if (not isinstance(certified_graph, dict)
            or certified_graph.get('schema') != _CERTIFIED_GRAPH_SCHEMA
            or not isinstance(certified_graph.get('graphDigest'), str)
            or re.fullmatch(r'sha256:[0-9a-f]{64}',
                            certified_graph.get('graphDigest')) is None
            or not isinstance(certified_graph.get('roles'), dict)
            or not certified_graph['roles']
            or not set(certified_graph['roles']) <= _ORT_ROLES):
        raise ValueError('CERTIFIED_GRAPH_PROVENANCE_DOCUMENT')
    roles = certified_graph['roles']
    provenance = certified_graph.get('referenceProvenance')
    if not isinstance(provenance, dict) or not provenance:
        raise ValueError('CERTIFIED_GRAPH_PROVENANCE_DOCUMENT')
    if set(provenance) != set(roles):
        raise ValueError('CERTIFIED_GRAPH_PROVENANCE_COVERAGE')
    for role in sorted(roles):
        row = provenance[role]
        expected = roles[role]
        if (not isinstance(row, dict) or set(row) != _PROVENANCE_FIELDS
                or row['schema'] != _REFERENCE_SCHEMA
                or row['qualification'] != _REFERENCE_QUALIFICATION
                or not isinstance(row['ortVersion'], str)
                or not row['ortVersion']
                or not isinstance(row['optimizedModelDigest'], str)
                or re.fullmatch(r'sha256:[0-9a-f]{64}',
                                row['optimizedModelDigest']) is None
                or not isinstance(row['sessionOptions'], dict)
                or dict(row['sessionOptions']) != _CANONICAL_SESSION_OPTIONS
                or row['backend'] not in _BACKENDS
                or not isinstance(expected, dict)
                or expected.get('backend') != row['backend']):
            raise ValueError('CERTIFIED_GRAPH_PROVENANCE_BINDING')
    return dict(schema=_CERTIFIED_GRAPH_SCHEMA,
                referenceProvenanceDigest=_digest(json.dumps(
                    provenance, sort_keys=True, separators=(',', ':'),
                    ensure_ascii=False).encode()))
