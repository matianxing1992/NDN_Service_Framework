"""Real small CPU ONNX/ORT experiment, not native/SIF/GPU qualification."""
import hashlib
import json
from pathlib import Path
import re
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_graph_reference import prepare_role_reference

onnx = pytest.importorskip('onnx')
ort = pytest.importorskip('onnxruntime')
np = pytest.importorskip('numpy')


def model():
    helper = onnx.helper
    graph = helper.make_graph([
        helper.make_node('Add', ['x', 'x'], ['y'], name='twice'),
        helper.make_node('Identity', ['y'], ['z'], name='removable'),
    ], 'small', [helper.make_tensor_value_info('x', onnx.TensorProto.FLOAT, [1])],
        [helper.make_tensor_value_info('z', onnx.TensorProto.FLOAT, [1])])
    return helper.make_model(graph, ir_version=9,
        opset_imports=[helper.make_opsetid('', 13)]).SerializeToString()


def arguments(tmp_path, payload):
    return dict(artifact_digest='sha256:'+hashlib.sha256(payload).hexdigest(),
        assembled_model_digest='sha256:'+hashlib.sha256(payload).hexdigest(),
        model_manifest_digest='sha256:'+'a'*64, role='BackboneNeck',
        backend='CPUExecutionProvider', ort_version=ort.__version__, scratch=tmp_path)


def test_reference_before_execution_matches_separate_real_cpu_profile(tmp_path):
    payload = model()
    reference = prepare_role_reference(payload, **arguments(tmp_path, payload))
    assert list(tmp_path.iterdir()) == []  # No plaintext model retained.
    assert reference['qualification'] == 'ORT_GRAPH_PREPARATION_COMPONENT_ONLY'
    assert reference['expected']['optimizedNodeNames'] == ['twice_kernel_time']
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    options.enable_profiling = True
    options.profile_file_prefix = str(tmp_path / 'actual')
    session = ort.InferenceSession(payload, sess_options=options,
                                   providers=['CPUExecutionProvider'])
    assert session.run(None, {'x': np.array([3.0], dtype=np.float32)})[0].tolist() == [6.0]
    events = json.loads(Path(session.end_profiling()).read_text())
    observed = [e['name'] for e in events if e.get('cat') == 'Node'
                and e.get('args', {}).get('provider')]
    assert observed == reference['expected']['optimizedNodeNames']


@pytest.mark.parametrize('field,value,reason', [
    ('assembled_model_digest', 'sha256:'+'b'*64, 'MODEL_BYTES'),
    ('artifact_digest', 'not-a-digest', 'ARTIFACT'),
    ('role', 'Merge', 'ROLE'),
    ('ort_version', 'incorrect', 'ORT_VERSION'),
    ('backend', 'unknown', 'BACKEND'),
    ('model_manifest_digest', '', 'MANIFEST'),
])
def test_bad_reference_inputs_leave_no_files(tmp_path, field, value, reason):
    payload = model()
    kwargs = arguments(tmp_path, payload)
    kwargs[field] = value
    with pytest.raises(ValueError, match='GRAPH_REFERENCE_'+reason):
        prepare_role_reference(payload, **kwargs)
    assert list(tmp_path.iterdir()) == []


def test_options_follow_native_runner_source():
    root = Path(__file__).resolve().parents[3]
    source = (root / 'NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp').read_text()
    options = source.split('makeSessionOptions(', 1)[1].split('\nvoid\nrequireCudaDevice', 1)[0]
    assert 'options.SetIntraOpNumThreads(1);' in options
    assert 'options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);' in options
    assert 'options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");' in options


def test_external_initializer_is_rejected_before_session(tmp_path, monkeypatch):
    value = onnx.load_model_from_string(model())
    tensor = value.graph.initializer.add()
    tensor.name = 'outside'
    tensor.data_type = onnx.TensorProto.FLOAT
    tensor.dims.append(1)
    tensor.data_location = onnx.TensorProto.EXTERNAL
    entry = tensor.external_data.add()
    entry.key, entry.value = 'location', '/etc/passwd'
    payload = value.SerializeToString()
    monkeypatch.setattr(ort, 'InferenceSession', lambda *a, **k: pytest.fail('session called'))
    with pytest.raises(ValueError, match='GRAPH_REFERENCE_EXTERNAL_DATA'):
        prepare_role_reference(payload, **arguments(tmp_path, payload))
    assert list(tmp_path.iterdir()) == []


def test_unavailable_cuda_cannot_silently_make_cpu_reference(tmp_path, monkeypatch):
    payload = model()
    kwargs = arguments(tmp_path, payload)
    kwargs['backend'] = 'CUDAExecutionProvider'
    monkeypatch.setattr(ort, 'get_available_providers', lambda: ['CPUExecutionProvider'])
    monkeypatch.setattr(ort, 'InferenceSession', lambda *a, **k: pytest.fail('session called'))
    with pytest.raises(ValueError, match='GRAPH_REFERENCE_BACKEND_UNAVAILABLE'):
        prepare_role_reference(payload, **kwargs)


def test_failed_session_cleans_private_plaintext_directory(tmp_path, monkeypatch):
    payload = model()
    def fail(*args, **kwargs):
        path = Path(kwargs['sess_options'].optimized_model_filepath)
        assert path.parent.stat().st_mode & 0o777 == 0o700
        path.write_bytes(b'partial plaintext model')
        raise RuntimeError('session failure')
    monkeypatch.setattr(ort, 'InferenceSession', fail)
    with pytest.raises(RuntimeError, match='session failure'):
        prepare_role_reference(payload, **arguments(tmp_path, payload))
    assert list(tmp_path.iterdir()) == []


from runtime.yolo_graph_reference import (
    _ORT_ROLES, serialize_certified_graph, validate_certified_graph_provenance,
)


def role_models():
    """Three distinct tiny ONNX graphs; each keeps one named Add after BASIC."""
    helper = onnx.helper
    payloads = {}
    for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1'):
        graph = helper.make_graph(
            [helper.make_node('Add', ['x', 'x'], ['y'], name=role)],
            'graph-' + role,
            [helper.make_tensor_value_info('x', onnx.TensorProto.FLOAT, [1])],
            [helper.make_tensor_value_info('y', onnx.TensorProto.FLOAT, [1])])
        payloads[role] = helper.make_model(graph, ir_version=9,
            opset_imports=[helper.make_opsetid('', 13)]).SerializeToString()
    return payloads


def references(tmp_path, payloads, backend='CPUExecutionProvider'):
    result = {}
    for role, payload in payloads.items():
        result[role] = prepare_role_reference(
            payload, artifact_digest='sha256:' + hashlib.sha256(payload).hexdigest(),
            assembled_model_digest='sha256:' + hashlib.sha256(payload).hexdigest(),
            model_manifest_digest='sha256:' + 'a' * 64, role=role, backend=backend,
            ort_version=ort.__version__, scratch=tmp_path)
    return result


def test_logical_role_artifact_is_distinct_from_assembled_model_bytes(tmp_path):
    """YOLO fragment IDs hash the role contract, not serialized ONNX bytes."""
    records = {}
    for role, payload in role_models().items():
        options = arguments(tmp_path, payload)
        options['role'] = role
        logical = 'sha256:' + hashlib.sha256(('signed-role:' + role).encode()).hexdigest()
        options['artifact_digest'] = logical
        assert logical != options['assembled_model_digest']
        records[role] = prepare_role_reference(payload, **options)
        assert records[role]['expected']['artifactDigest'] == logical
        assert records[role]['assembledModelDigest'] == options['assembled_model_digest']
    document = serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)
    for role, record in records.items():
        assert document['referenceProvenance'][role]['assembledModelDigest'] == record['assembledModelDigest']
    validate_certified_graph_provenance(document)
    assert list(tmp_path.iterdir()) == []


def test_serialize_produces_comparator_document_and_join_passes(tmp_path):
    """Expected names are fixed by independent ORT preparation first; the
    retained observations below are synthetic fixtures matched to them (the
    established join-test direction), never the other way around."""
    payloads = role_models()
    graph_digest = 'sha256:' + 'c' * 64
    document = serialize_certified_graph(references(tmp_path, payloads),
                                         graph_digest=graph_digest)
    assert document['schema'] == 'tiger-yolo-certified-graph-v1'
    assert document['graphDigest'] == graph_digest
    assert set(document['roles']) == set(document['referenceProvenance']) == set(payloads)
    for role, record in references(tmp_path, payloads).items():
        assert document['roles'][role] == record['expected']
        provenance = document['referenceProvenance'][role]
        assert provenance['optimizedModelDigest'] == record['optimizedModelDigest']
        assert provenance['sessionOptions'] == record['sessionOptions']
        assert provenance['backend'] == record['expected']['backend']
    assert list(tmp_path.iterdir()) == []
    from runtime.yolo_result import validate_certified_graph_coverage
    observations, bindings = {}, {}
    for role, expected in document['roles'].items():
        bindings[role] = {'modelManifestDigest': expected['modelManifestDigest'],
                          'artifactDigest': expected['artifactDigest']}
        observations[role] = {'nodeProviderAssignments': [
            {'role': role, 'nodeName': name, 'provider': expected['backend'],
             'modelNode': True} for name in expected['optimizedNodeNames']]}
    checked = validate_certified_graph_coverage(observations, bindings, document,
                                                graph_digest=graph_digest)
    assert checked['qualification'] == 'CERTIFIED_GRAPH_COMPONENT_ONLY'
    assert checked['roles'] == {role: {'optimizedNodeCount': 1,
        'backend': 'CPUExecutionProvider'} for role in document['roles']}


def test_comparator_rejects_document_without_producer_provenance(tmp_path):
    payloads = role_models()
    document = serialize_certified_graph(references(tmp_path, payloads),
                                         graph_digest='sha256:' + 'c' * 64)
    role = 'BackboneNeck'
    expected = document['roles'][role]
    observations = {role: {'nodeProviderAssignments': [
        {'role': role, 'nodeName': name, 'provider': expected['backend'],
         'modelNode': True} for name in expected['optimizedNodeNames']]}}
    bindings = {role: {'modelManifestDigest': expected['modelManifestDigest'],
                       'artifactDigest': expected['artifactDigest']}}
    del document['referenceProvenance']
    from runtime.yolo_result import EvidenceError, validate_certified_graph_coverage
    with pytest.raises(EvidenceError, match='CERTIFIED_GRAPH_PROVENANCE_DOCUMENT'):
        validate_certified_graph_coverage(observations, bindings, document,
                                          graph_digest='sha256:' + 'c' * 64)


def test_serialize_rejects_incomplete_or_extra_role_coverage(tmp_path):
    payloads = role_models()
    complete = references(tmp_path, payloads)
    missing = {role: record for role, record in complete.items()
               if role != 'DetectShard1'}
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_ROLE_COVERAGE'):
        serialize_certified_graph(missing, graph_digest='sha256:' + 'c' * 64)
    extra = dict(complete)
    extra['Merge'] = dict(complete['BackboneNeck'], role='Merge')
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_ROLE_COVERAGE'):
        serialize_certified_graph(extra, graph_digest='sha256:' + 'c' * 64)
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_ROLE_COVERAGE'):
        serialize_certified_graph({}, graph_digest='sha256:' + 'c' * 64)


@pytest.mark.parametrize('field,value,code', [
    ('ortVersion', '', 'CERTIFIED_GRAPH_REFERENCE_ORT_VERSION'),
    ('sessionOptions', {'intraOpThreads': 2, 'graphOptimization': 'ORT_ENABLE_BASIC',
                        'allowCpuFallback': False, 'deviceId': 0},
     'CERTIFIED_GRAPH_REFERENCE_SESSION_OPTIONS'),
    ('optimizedModelDigest', 'not-a-digest', 'CERTIFIED_GRAPH_REFERENCE_OPTIMIZED_DIGEST'),
    ('assembledModelDigest', 'not-a-digest', 'CERTIFIED_GRAPH_REFERENCE_ASSEMBLED_DIGEST'),
])
def test_serialize_rejects_tampered_reference_provenance(tmp_path, field, value, code):
    payloads = role_models()
    records = references(tmp_path, payloads)
    records['DetectShard0'][field] = value
    with pytest.raises(ValueError, match=code):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)


@pytest.mark.parametrize('field,value,code', [
    ('schema', 'tiger-yolo-role-reference-v2', 'CERTIFIED_GRAPH_REFERENCE_PROVENANCE'),
    ('role', 'Other', 'CERTIFIED_GRAPH_REFERENCE_PROVENANCE'),
    ('qualification', 'SOMEONE_ELSE', 'CERTIFIED_GRAPH_REFERENCE_PROVENANCE'),
])
def test_serialize_rejects_fabricated_reference_identity(tmp_path, field, value, code):
    payloads = role_models()
    records = references(tmp_path, payloads)
    records['BackboneNeck'][field] = value
    with pytest.raises(ValueError, match=code):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)


def test_serialize_rejects_altered_records_or_vocabulary(tmp_path):
    payloads = role_models()
    records = references(tmp_path, payloads)
    records['BackboneNeck']['extra'] = True
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_REFERENCE_SCHEMA'):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)
    records = references(tmp_path, payloads)
    records['BackboneNeck']['expected']['unknown'] = 1
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_ROLE_SCHEMA'):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)
    records = references(tmp_path, payloads)
    records['BackboneNeck']['expected']['optimizedNodeNames'] = []
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_NODE_VOCABULARY'):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)
    records = references(tmp_path, payloads)
    names = records['BackboneNeck']['expected']['optimizedNodeNames']
    records['BackboneNeck']['expected']['optimizedNodeNames'] = names + names[:1]
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_NODE_VOCABULARY'):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)
    records = references(tmp_path, payloads)
    records['BackboneNeck']['expected']['backend'] = 'CUDAExecutionProvider'
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_BACKEND_BINDING'):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)
    records = references(tmp_path, payloads)
    records['BackboneNeck']['expected']['backend'] = 'TpuProvider'
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_ROLE_BACKEND'):
        serialize_certified_graph(records, graph_digest='sha256:' + 'c' * 64)


def test_serialize_rejects_bad_graph_digest(tmp_path):
    payloads = role_models()
    with pytest.raises(ValueError, match='CERTIFIED_GRAPH_GRAPH_DIGEST'):
        serialize_certified_graph(references(tmp_path, payloads), graph_digest='nope')


@pytest.mark.parametrize('mutation,code', [
    ('missing-role', 'CERTIFIED_GRAPH_PROVENANCE_COVERAGE'),
    ('backend-mismatch', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('expected-backend-mismatch', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('session-tamper', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('export-digest', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('assembled-digest', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('ort-version', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('identity', 'CERTIFIED_GRAPH_PROVENANCE_BINDING'),
    ('no-roles', 'CERTIFIED_GRAPH_PROVENANCE_DOCUMENT'),
    ('bad-graph-digest', 'CERTIFIED_GRAPH_PROVENANCE_DOCUMENT'),
    ('foreign-schema', 'CERTIFIED_GRAPH_PROVENANCE_DOCUMENT'),
])
def test_provenance_recheck_rejects_mutated_documents(tmp_path, mutation, code):
    payloads = role_models()
    document = serialize_certified_graph(references(tmp_path, payloads),
                                         graph_digest='sha256:' + 'c' * 64)
    role = 'DetectShard0'
    if mutation == 'missing-role':
        del document['referenceProvenance'][role]
    elif mutation == 'backend-mismatch':
        document['referenceProvenance'][role]['backend'] = 'CUDAExecutionProvider'
    elif mutation == 'expected-backend-mismatch':
        document['roles'][role]['backend'] = 'CUDAExecutionProvider'
    elif mutation == 'session-tamper':
        document['referenceProvenance'][role]['sessionOptions']['intraOpThreads'] = 4
    elif mutation == 'export-digest':
        document['referenceProvenance'][role]['optimizedModelDigest'] = 'not-a-digest'
    elif mutation == 'assembled-digest':
        document['referenceProvenance'][role]['assembledModelDigest'] = 'not-a-digest'
    elif mutation == 'ort-version':
        document['referenceProvenance'][role]['ortVersion'] = ''
    elif mutation == 'identity':
        document['referenceProvenance'][role]['qualification'] = 'FABRICATED'
    elif mutation == 'no-roles':
        document['roles'] = {}
    elif mutation == 'bad-graph-digest':
        document['graphDigest'] = 'nope'
    else:
        document['schema'] = 'tiger-yolo-other-v1'
    with pytest.raises(ValueError, match=code):
        validate_certified_graph_provenance(document)


def test_provenance_digest_is_deterministic_and_retainable(tmp_path):
    payloads = role_models()
    graph_digest = 'sha256:' + 'c' * 64
    document = serialize_certified_graph(references(tmp_path, payloads),
                                         graph_digest=graph_digest)
    first = validate_certified_graph_provenance(document)
    second = validate_certified_graph_provenance(document)
    assert first == second
    assert re.fullmatch(r'sha256:[0-9a-f]{64}', first['referenceProvenanceDigest'])
    assert first['referenceProvenanceDigest'] != graph_digest


def test_ort_role_boundary_matches_result_owner():
    from runtime.yolo_result import _ORT_ROLES as result_roles
    from runtime.yolo_worker import MODEL_ROLES
    assert _ORT_ROLES == result_roles == MODEL_ROLES
