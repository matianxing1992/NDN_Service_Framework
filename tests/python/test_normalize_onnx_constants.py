import importlib.util
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import pytest
from onnx import TensorProto, helper


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "normalize_constants", ROOT / "tools/ndnsf-di/normalize_onnx_constants.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def make_model():
    graph = helper.make_graph([
        helper.make_node("Constant", [], ["target"], value=helper.make_tensor(
            "value", TensorProto.INT64, [4], [1, 1, -1, 128])),
        helper.make_node("Reshape", ["hidden", "target"], ["output"]),
    ], "shape-constant", [helper.make_tensor_value_info(
        "hidden", TensorProto.FLOAT, [1, 128])], [helper.make_tensor_value_info(
        "output", TensorProto.FLOAT, [1, 1, 1, 128])])
    return helper.make_model(graph, ir_version=8,
                             opset_imports=[helper.make_opsetid("", 18)])


def test_constant_lowering_preserves_backend_output_and_is_idempotent():
    model = make_model()
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    original = ort.InferenceSession(model.SerializeToString(), options,
                                    providers=["CPUExecutionProvider"])
    assert module.normalize(model) == 1
    onnx.checker.check_model(model)
    assert module.normalize(model) == 0
    normalized = ort.InferenceSession(model.SerializeToString(), options,
                                      providers=["CPUExecutionProvider"])
    values = np.arange(128, dtype=np.float32).reshape(1, 128)
    np.testing.assert_array_equal(original.run(None, {"hidden": values})[0],
                                  normalized.run(None, {"hidden": values})[0])
    assert [value.name for value in normalized.get_inputs()] == ["hidden"]


def test_cpu_reproduces_zero_filled_boundary_shape_failure():
    model = make_model()
    del model.graph.node[0]
    model.graph.input.append(helper.make_tensor_value_info(
        "target", TensorProto.INT64, [4]))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    session = ort.InferenceSession(model.SerializeToString(), options,
                                   providers=["CPUExecutionProvider"])
    with pytest.raises(Exception, match="dimension with value zero exceeds"):
        session.run(None, {"hidden": np.zeros((1, 128), np.float32),
                           "target": np.zeros(4, np.int64)})


def test_constant_lowering_rejects_overridable_initializer_collision():
    model = make_model()
    model.graph.input.append(helper.make_tensor_value_info(
        "target", TensorProto.INT64, [4]))
    with pytest.raises(ValueError, match="collides"):
        module.normalize(model)


def test_external_weights_and_unsupported_constants_remain_unchanged():
    model = make_model()
    external = model.graph.initializer.add()
    external.name = "weight"
    external.data_type = TensorProto.FLOAT
    external.dims.append(128)
    external.data_location = TensorProto.EXTERNAL
    for key, value in (("location", "model.onnx_data"), ("offset", "16"),
                       ("length", "512")):
        entry = external.external_data.add()
        entry.key, entry.value = key, value
    weight_bytes = external.SerializeToString()
    custom = helper.make_node("Constant", [], ["custom"], domain="application",
                              value=helper.make_tensor("value", TensorProto.INT64, [], [128]))
    non_tensor = helper.make_node("Constant", [], ["integer"], value_int=128)
    model.graph.node.extend([custom, non_tensor])
    assert module.normalize(model) == 1
    assert model.graph.initializer[0].SerializeToString() == weight_bytes
    assert model.graph.node[-2].SerializeToString() == custom.SerializeToString()
    assert model.graph.node[-1].SerializeToString() == non_tensor.SerializeToString()
