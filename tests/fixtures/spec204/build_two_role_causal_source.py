#!/usr/bin/env python3
"""Compose the maintained Spec175 two-role ONNX fixtures for Spec212."""

from __future__ import annotations

import argparse
import copy
from pathlib import Path

import onnx
from onnx import helper


ROOT = Path(__file__).resolve().parents[3]
PARTITIONS = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1/two-role"


def compose() -> onnx.ModelProto:
    first = onnx.load(str(PARTITIONS / "role-0.onnx"))
    second = onnx.load(str(PARTITIONS / "role-1.onnx"))
    prefix = "stage1__"

    nodes = [copy.deepcopy(node) for node in first.graph.node]
    for original in second.graph.node:
        node = copy.deepcopy(original)
        node.name = prefix + node.name
        for index, name in enumerate(node.input):
            if name == "hidden_in":
                node.input[index] = "hidden_out"
            elif name:
                node.input[index] = prefix + name
        for index, name in enumerate(node.output):
            if name:
                node.output[index] = prefix + name
        nodes.append(node)

    initializers = [copy.deepcopy(item) for item in first.graph.initializer]
    for original in second.graph.initializer:
        item = copy.deepcopy(original)
        item.name = prefix + item.name
        initializers.append(item)

    inputs = [copy.deepcopy(item) for item in first.graph.input]
    for original in second.graph.input:
        if original.name == "hidden_in":
            continue
        item = copy.deepcopy(original)
        item.name = prefix + item.name
        inputs.append(item)

    outputs = []
    for original in second.graph.output:
        item = copy.deepcopy(original)
        item.name = prefix + item.name
        outputs.append(item)
    for original in first.graph.output:
        if original.name != "hidden_out":
            outputs.append(copy.deepcopy(original))

    graph = helper.make_graph(
        nodes, "spec212-two-role-causal-source", inputs, outputs,
        initializer=initializers,
    )
    model = helper.make_model(
        graph, producer_name="ndnsf-spec212-fixture",
        opset_imports=list(first.opset_import),
    )
    model.ir_version = first.ir_version
    onnx.checker.check_model(model, full_check=True)
    if len({item.name for item in model.graph.output}) != len(model.graph.output):
        raise ValueError("composed source has duplicate graph outputs")
    return model


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(compose().SerializeToString(deterministic=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
