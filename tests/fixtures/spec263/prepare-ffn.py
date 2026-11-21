#!/usr/bin/env python3
"""Generate the deterministic Spec 263 CPU source and independent output oracle."""

import argparse
import json
from pathlib import Path

import numpy as np
from onnx import TensorProto, checker, helper, numpy_helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    x = np.array([[0.5, -0.25, 0.75]], dtype=np.float32)
    scale = np.array([0.5, 1.25, -0.75], dtype=np.float32)
    weights = {
        "scale": scale,
        "W1": (np.arange(21, dtype=np.float32).reshape(3, 7) - 10) / 16,
        "B1": (np.arange(7, dtype=np.float32) - 3) / 8,
        "W2": ((np.arange(28, dtype=np.float32).reshape(7, 4) * 11) % 23 - 11) / 16,
        "W3": ((np.arange(20, dtype=np.float32).reshape(4, 5) * 7) % 19 - 9) / 16,
        "W4": ((np.arange(15, dtype=np.float32).reshape(5, 3) * 9) % 17 - 8) / 16,
    }
    op = helper.make_node
    nodes = [
        op("Mul", ["x", "scale"], ["hidden"]),
        op("MatMul", ["hidden", "W1"], ["up1"]),
        op("Add", ["up1", "B1"], ["biased1"]),
        op("Gelu", ["biased1"], ["gelu1"], approximate="tanh"),
        op("MatMul", ["gelu1", "W2"], ["mid"]),
        op("MatMul", ["mid", "W3"], ["up2"]),
        op("Gelu", ["up2"], ["gelu2"], approximate="tanh"),
        op("MatMul", ["gelu2", "W4"], ["last"]),
        op("Add", ["last", "hidden"], ["y"]),
    ]
    tensor = helper.make_tensor_value_info
    graph = helper.make_graph(nodes, "spec263-two-paired-ffn",
                              [tensor("x", TensorProto.FLOAT, [1, 3])],
                              [tensor("y", TensorProto.FLOAT, [1, 3])],
                              [numpy_helper.from_array(value, name)
                               for name, value in weights.items()])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 20)], ir_version=9)
    checker.check_model(model, full_check=True)
    (args.output / "model.onnx").write_bytes(model.SerializeToString(deterministic=True))
    (args.output / "input.f32").write_bytes(x.astype("<f4").tobytes())

    arrays = {name: value.astype(np.float64) for name, value in weights.items()}
    hidden = x.astype(np.float64) * arrays["scale"]

    def gelu(value):
        return 0.5 * value * (1 + np.tanh(np.sqrt(2 / np.pi) *
                                         (value + 0.044715 * value ** 3)))

    first = gelu(hidden @ arrays["W1"] + arrays["B1"]) @ arrays["W2"]
    output = gelu(first @ arrays["W3"]) @ arrays["W4"] + hidden
    oracle = {"tensor": "y", "float32": output.ravel().tolist(), "tolerance": 1e-6}
    (args.output / "oracle.json").write_text(json.dumps(oracle) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
