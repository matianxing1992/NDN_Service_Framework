"""Source-profile preparation must preserve GQA's scalar INT32 boundary input."""
import hashlib
import importlib.util
from pathlib import Path

import pytest

onnx = pytest.importorskip('onnx')
pytest.importorskip('onnxruntime.tools.symbolic_shape_infer')
spec = importlib.util.spec_from_file_location('native_qwen_transfer_profile',
    Path(__file__).resolve().parents[2] / 'Experiments/native_qwen_transfer_profile.py')
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)


@pytest.mark.parametrize('length_type', [onnx.TensorProto.INT32, onnx.TensorProto.DOUBLE])
def test_shared_scalar_input_keeps_declared_dtype(tmp_path, length_type):
    helper = onnx.helper
    graph = helper.make_graph([
        helper.make_node('Cast', ['length'], ['length0'], to=onnx.TensorProto.INT64),
        helper.make_node('Add', ['input_ids', 'length0'], ['hidden']),
        helper.make_node('Cast', ['length'], ['length1'], to=onnx.TensorProto.INT64),
        helper.make_node('Add', ['hidden', 'length1'], ['output'])], 'boundary', [
        helper.make_tensor_value_info('input_ids', onnx.TensorProto.INT64,
                                      [1, 'sequence_length']),
        helper.make_tensor_value_info('length', length_type, [])], [
        helper.make_tensor_value_info('output', onnx.TensorProto.INT64,
                                      [1, 'sequence_length'])])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 13)])
    source = tmp_path / 'model.onnx'
    source.write_bytes(model.SerializeToString())
    mapping = {'embedding': [], 'final-norm-head': [], 'layer-00': [0, 1], 'layer-01': [2, 3]}
    args = (source, 'sha256:' + hashlib.sha256(source.read_bytes()).hexdigest(), mapping,
            [{'role': 'encoder'}, {'role': 'decoder'}], [[0, 1], [1, 2]], b'input', 3)
    if length_type == onnx.TensorProto.DOUBLE:
        with pytest.raises(ValueError, match='unsupported transfer tensor'):
            profiles.initial_transfer_profile(*args)
        return
    profile = profiles.initial_transfer_profile(*args)
    tensors = {row['name']: row for row in profile['dependencies'][0]['tensors']}
    assert tensors['length'] == {'name': 'length', 'dtype': 'int32', 'shape': []}
    assert tensors['hidden']['shape'] == [1, 3]
    template = {row['name']: row for row in profile['shape_template']['dependencies'][0]['tensors']}
    assert template['length'] == tensors['length']
    assert source.read_bytes() == model.SerializeToString()
