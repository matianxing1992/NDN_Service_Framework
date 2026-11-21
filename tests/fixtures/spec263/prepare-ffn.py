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
    parser.add_argument("--packed-down", action="store_true",
                        help="exercise block-32 MatMulNBits on both down projections")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    x = np.array([[0.5, -0.25, 0.75]], dtype=np.float32)
    scale = np.array([0.5, 1.25, -0.75], dtype=np.float32)
    width1, width2 = (64, 64) if args.packed_down else (7, 5)
    weights = {
        "scale": scale,
        "W1": (np.arange(3 * width1, dtype=np.float32).reshape(3, width1) % 21 - 10) / 16,
        "B1": ((np.arange(width1, dtype=np.float32) % 7) - 3) / 8,
        "W2": ((np.arange(width1 * 4, dtype=np.float32).reshape(width1, 4) * 11)
               % 23 - 11) / 16,
        "W3": ((np.arange(4 * width2, dtype=np.float32).reshape(4, width2) * 7)
               % 19 - 9) / 16,
        "W4": ((np.arange(width2 * 3, dtype=np.float32).reshape(width2, 3) * 9)
               % 17 - 8) / 16,
    }
    op = helper.make_node
    packed_weights = {}

    def down(source, name, output):
        if not args.packed_down:
            return op("MatMul", [source, name], [output])
        k, n = weights[name].shape
        # Deterministic asymmetric block scales and nibble values. Build the
        # FP64 reference from unpacked values before forming the stored bytes.
        block_scales = ((np.arange(n * k // 32).reshape(n, k // 32) % 5) + 1) / 128
        quantized = np.rint(weights[name].T * 8 + 8).clip(0, 15).astype(np.uint8)
        weights[name] = ((quantized.astype(np.float64) - 8) *
                         np.repeat(block_scales, 32, axis=1)).T
        packed_weights[name + ".packed"] = (quantized[:, ::2] |
                                             (quantized[:, 1::2] << 4)).reshape(n, k // 32, 16)
        packed_weights[name + ".scale"] = block_scales.astype(np.float32).ravel()
        return op("MatMulNBits", [source, name + ".packed", name + ".scale"], [output],
                  domain="com.microsoft", K=k, N=n, bits=4, block_size=32, accuracy_level=1)

    nodes = [
        op("Mul", ["x", "scale"], ["hidden"]),
        op("MatMul", ["hidden", "W1"], ["up1"]),
        op("Add", ["up1", "B1"], ["biased1"]),
        op("Gelu", ["biased1"], ["gelu1"], approximate="tanh"),
        down("gelu1", "W2", "mid"),
        op("MatMul", ["mid", "W3"], ["up2"]),
        op("Gelu", ["up2"], ["gelu2"], approximate="tanh"),
        down("gelu2", "W4", "last"),
        op("Add", ["last", "hidden"], ["y"]),
    ]
    tensor = helper.make_tensor_value_info
    initializers = {name: value for name, value in weights.items()
                    if not args.packed_down or name not in ("W2", "W4")}
    initializers.update(packed_weights)
    graph = helper.make_graph(nodes, "spec263-two-paired-ffn",
                              [tensor("x", TensorProto.FLOAT, [1, 3])],
                              [tensor("y", TensorProto.FLOAT, [1, 3])],
                              [numpy_helper.from_array(value, name)
                               for name, value in initializers.items()])
    opsets = [helper.make_opsetid("", 20)]
    if args.packed_down:
        opsets.append(helper.make_opsetid("com.microsoft", 1))
        graph.value_info.extend([tensor("mid", TensorProto.FLOAT, [1, 4]),
                                 tensor("last", TensorProto.FLOAT, [1, 3])])
    model = helper.make_model(graph, opset_imports=opsets, ir_version=9)
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
