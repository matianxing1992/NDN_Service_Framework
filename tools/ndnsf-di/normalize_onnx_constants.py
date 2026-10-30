#!/usr/bin/env python3
"""Prepare dense ONNX Constants as initializers before native DI partitioning.

This offline transformation preserves tensor names and values. In particular,
shape constants must remain model material rather than artificial activation
inputs whose values cannot be inferred from their dtype and shape at warmup.
External weights are left untouched; write beside the source to preserve their
relative locations. Run native model preparation again for the new graph.
"""

import argparse
from pathlib import Path


def normalize(model):
    """Lower standard dense-tensor Constants; leave other operators unchanged."""
    import onnx

    graph = model.graph
    names = {tensor.name for tensor in graph.initializer}
    names.update(value.name for value in graph.input)
    retained = []
    converted = 0
    for node in graph.node:
        if (node.domain not in ("", "ai.onnx") or node.op_type != "Constant"
                or node.input or len(node.output) != 1
                or len(node.attribute) != 1
                or node.attribute[0].name != "value"
                or node.attribute[0].type != onnx.AttributeProto.TENSOR):
            retained.append(node)
            continue
        name = node.output[0]
        if not name or name in names:
            raise ValueError("constant collides with graph input/initializer: " + name)
        tensor = graph.initializer.add()
        tensor.CopyFrom(node.attribute[0].t)
        tensor.name = name
        names.add(name)
        converted += 1
    del graph.node[:]
    graph.node.extend(retained)
    return converted


def main():
    import onnx

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.source.resolve().parent != args.output.resolve().parent:
        parser.error("output must be beside source to preserve external weight locations")
    if args.output.exists():
        parser.error("output already exists; immutable source/output must not be overwritten")
    model = onnx.load(str(args.source), load_external_data=False)
    count = normalize(model)
    # Exclusive creation also protects against a concurrent preparation writer.
    with args.output.open("xb") as stream:
        stream.write(model.SerializeToString())
    print("Normalized {} dense Constants; regenerate native contracts: {}".format(
        count, args.output))


if __name__ == "__main__":
    main()
