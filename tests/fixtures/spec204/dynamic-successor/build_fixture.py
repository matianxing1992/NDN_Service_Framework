#!/usr/bin/env python3
"""Generate a tiny ONNX graph with a mapped dynamic successor state name."""

from pathlib import Path

import onnx
from onnx import TensorProto, helper


def build(output: Path) -> None:
    input_ids = "input_ids"
    past_name = "past_key_values.17.key"
    present_name = "present.17.key"
    axes = helper.make_tensor("token_axes", TensorProto.INT64, [1], [2])
    graph = helper.make_graph(
        [
            helper.make_node("Cast", [input_ids], ["token_float"], to=TensorProto.FLOAT),
            helper.make_node("Unsqueeze", ["token_float", "token_axes"], ["token_key"]),
            helper.make_node("Concat", [past_name, "token_key"], [present_name], axis=1),
        ],
        "spec204-dynamic-successor",
        [
            helper.make_tensor_value_info(input_ids, TensorProto.INT64, [1, "sequence"]),
            helper.make_tensor_value_info(
                past_name, TensorProto.FLOAT, [1, "past_sequence", 1]
            ),
        ],
        [helper.make_tensor_value_info(
            present_name, TensorProto.FLOAT, [1, "present_sequence", 1]
        )],
        initializer=[axes],
    )
    model = helper.make_model(
        graph,
        producer_name="ndnsf-spec204-dynamic-successor",
        producer_version="1",
        opset_imports=[helper.make_operatorsetid("", 17)],
    )
    model.ir_version = 9
    onnx.checker.check_model(model, full_check=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(model.SerializeToString(deterministic=True))


if __name__ == "__main__":
    build(Path(__file__).with_name("role.onnx"))
