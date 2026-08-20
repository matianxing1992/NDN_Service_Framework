#!/usr/bin/env python3
"""Generate deterministic tiny ONNX models for the native CPU adapter gates."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def write_model(path: Path, output_names: list[str], width: int) -> None:
    input_info = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, width])
    output_info = [
        helper.make_tensor_value_info(name, TensorProto.FLOAT, [1, width])
        for name in output_names
    ]
    nodes = []
    initializers = []
    for index, output_name in enumerate(output_names, start=1):
        constant_name = f"constant_{index}"
        initializers.append(
            numpy_helper.from_array(
                np.full((1, width), float(index), dtype=np.float32),
                name=constant_name,
            )
        )
        nodes.append(helper.make_node("Add", ["x", constant_name], [output_name]))
    graph = helper.make_graph(
        nodes,
        "ndnsf-spec170-cpu-fixture",
        [input_info],
        output_info,
        initializer=initializers,
    )
    model = helper.make_model(
        graph,
        producer_name="ndnsf-spec170-tests",
        opset_imports=[helper.make_opsetid("", 17)],
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    write_model(args.output / "linear.onnx", ["y"], 3)
    write_model(args.output / "linear-multi.onnx", ["y", "z"], 3)
    write_model(args.output / "slice.onnx", ["y"], 2)
    write_model(args.output / "unsplit.onnx", ["y"], 4)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
