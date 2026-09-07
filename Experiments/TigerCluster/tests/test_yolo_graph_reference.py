"""Real small CPU ONNX/ORT experiment, not native/SIF/GPU qualification."""
import hashlib
import json
from pathlib import Path
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
    ('artifact_digest', 'sha256:'+'b'*64, 'MODEL_BYTES'),
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
