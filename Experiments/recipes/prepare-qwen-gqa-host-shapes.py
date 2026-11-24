from collections import Counter
from pathlib import Path
import re
import sys

import onnx
from onnx import TensorProto, helper, numpy_helper

source, output = map(Path, sys.argv[1:3])
family = sys.argv[3]
attention_count = {"4b": 36, "27b": 16}[family]
model = onnx.load(str(source), load_external_data=False)
graph = model.graph
source_nodes = list(graph.node)
source_info = {entry.name: entry for entry in graph.value_info}
producer = {name: node for node in graph.node for name in node.output if name}
attentions = [node for node in graph.node
              if (node.domain, node.op_type) == ("com.microsoft", "GroupQueryAttention")]
assert len(attentions) == attention_count
assert len({(node.input[5], node.input[6]) for node in attentions}) == 1
prefix = "/model/attn_mask_reformat/attn_mask_subgraph"
lengths = tuple(attentions[0].input[index] for index in (5, 6))
assert producer[lengths[1]].op_type == "Cast"
expected = {"Cast", "Gather", "ReduceSum", "Shape", "Sub"}
names = set()
pending = list(lengths)
while pending:
    value = pending.pop()
    if value not in producer:
        assert value == "attention_mask" or any(init.name == value for init in graph.initializer)
        continue
    node = producer[value]
    if node.op_type == "Constant":
        assert not node.domain and len(node.output) == 1 and not node.input
        literal = next(attr.t for attr in node.attribute if attr.name == "value")
        assert literal.data_type in (onnx.TensorProto.INT32, onnx.TensorProto.INT64)
        assert len(literal.dims) <= 1 and len(literal.raw_data) <= 16
        names.add(node.name)
        continue
    assert node.name.startswith(prefix) and not node.domain and node.op_type in expected, node
    if node.name not in names:
        names.add(node.name)
        pending.extend(node.input)
assert set(node.op_type for node in graph.node if node.name in names) - {"Constant"} == expected
closure = [node for node in source_nodes if node.name in names]
cloned = {}
for index, attention in enumerate(attentions):
    layer = re.match(r"/model/layers\.(\d+)/", attention.name)
    assert layer is not None, attention.name
    layer_prefix = layer.group(0)
    wires = {}
    copies = []
    for node in closure:
        copy = onnx.NodeProto()
        copy.CopyFrom(node)
        copy.name = f"{layer_prefix}spec235_attention_shape{node.name}"
        for i, wire in enumerate(node.input):
            copy.input[i] = wires.get(wire, wire)
        for i, wire in enumerate(node.output):
            wires[wire] = f"/spec235/AttentionShape{index}{wire}"
            copy.output[i] = wires[wire]
        copies.append(copy)
    cloned[index] = (copies, wires)

position_branches = {}
position_users = Counter()
literal_aliases = {}
literal_types = {}
constant_prefix = "/model/constants/"
if family == "27b":
    # A physical role that owns GQA binds both raw generation inputs. Derive the
    # three text position planes alongside the rotary operators that consume them.
    mrope = "ndnsf_text_positions_mrope"
    tile = producer[mrope]
    unsqueeze = producer[tile.input[0]]
    assert (tile.domain, tile.op_type, list(tile.input), list(tile.output)) == (
        "", "Tile", ["ndnsf_text_positions_expanded", "ndnsf_text_positions_repeats"], [mrope])
    assert (unsqueeze.domain, unsqueeze.op_type, list(unsqueeze.input), list(unsqueeze.output)) == (
        "", "Unsqueeze", ["position_ids", "ndnsf_text_positions_axes"], [tile.input[0]])
    for name, values in (("ndnsf_text_positions_axes", [0]),
                         ("ndnsf_text_positions_repeats", [3, 1, 1])):
        initializer, = (value for value in graph.initializer if value.name == name)
        assert numpy_helper.to_array(initializer).tolist() == values
    literals = {
        value: node for value, node in producer.items()
        if value.startswith(constant_prefix) and node.op_type == "Constant"
    }
    assert literals
    for node in literals.values():
        assert not node.domain and not node.input and len(node.output) == 1
        attribute, = node.attribute
        assert attribute.name == "value" and attribute.type == onnx.AttributeProto.TENSOR
        assert attribute.t.data_location != TensorProto.EXTERNAL

normalized = []
attention_index = 0
for node in source_nodes:
    copy = onnx.NodeProto()
    copy.CopyFrom(node)
    if not copy.name:
        assert family == "4b" and copy.op_type == "Cast" and list(copy.output) == ["logits"]
        copy.name = "/model/logits_output_cast"
    if family == "4b":
        for i, name in enumerate(copy.input):
            if name == "/model/pos_ids_reformat/Reshape/output_0":
                assert (copy.domain, copy.op_type, i) == ("com.microsoft", "RotaryEmbedding", 1)
                copy.input[i] = "position_ids"
    if family == "27b" and mrope in copy.input:
        layer = re.fullmatch(r"(/model/layers\.\d+/)attn/[qk]_rotary/MRotaryEmbedding", copy.name)
        assert layer is not None and (copy.domain, copy.op_type) == (
            "com.microsoft", "MRotaryEmbedding")
        assert copy.input[1] == mrope and list(copy.input).count(mrope) == 1
        position_prefix = f"{layer[1]}spec235_text_positions/"
        position_users[layer[1]] += 1
        if position_users[layer[1]] == 1:
            wires = {unsqueeze.output[0]: position_prefix + "expanded",
                     mrope: position_prefix + "mrope"}
            position_branches[layer[1]] = wires
            for original in (unsqueeze, tile):
                derived = onnx.NodeProto()
                derived.CopyFrom(original)
                derived.name = position_prefix + original.op_type
                derived.input[0] = wires.get(original.input[0], original.input[0])
                derived.output[0] = wires[original.output[0]]
                normalized.append(derived)
        copy.input[1] = position_branches[layer[1]][mrope]
    if family == "27b":
        owner = re.match(r"/model/layers\.\d+/", copy.name)
        if owner:
            # Preserve literal dimensions inside the consuming layer. A PP
            # role should transfer activations, not exported constant values.
            for index, value in enumerate(copy.input):
                if value not in literals:
                    continue
                key = (owner[0], value)
                if key not in literal_aliases:
                    original = literals[value]
                    literal = onnx.NodeProto()
                    literal.CopyFrom(original)
                    literal.name = owner[0] + "spec235_literal/" + value[len(constant_prefix):]
                    alias = literal.name + "/output"
                    literal.output[0] = alias
                    literal_aliases[key] = alias
                    literal_types[alias] = original.attribute[0].t
                    normalized.append(literal)
                copy.input[index] = literal_aliases[key]
    if (copy.domain, copy.op_type) == ("com.microsoft", "GroupQueryAttention"):
        assert re.match(r"/model/layers\.(\d+)/", copy.name)
        replacements, wires = cloned[attention_index]
        normalized.extend(replacements)
        attention_index += 1
        for i in (5, 6):
            copy.input[i] = wires[copy.input[i]]
    normalized.append(copy)
assert attention_index == attention_count
if family == "27b":
    assert set(position_users) == {re.match(r"/model/layers\.\d+/", node.name)[0]
                                   for node in attentions}
    assert set(position_users.values()) == {2}
live = {port.name for port in graph.output}
kept = []
for node in reversed(normalized):
    if live.intersection(node.output):
        kept.append(node)
        live.update(value for value in node.input if value)
assert len({node.name for node in kept}) == len(kept)
graph.ClearField("node")
graph.node.extend(reversed(kept))
for index in range(len(graph.value_info)-1, -1, -1):
    if graph.value_info[index].name not in live:
        del graph.value_info[index]
branch_aliases = [wires for _, wires in cloned.values()]
branch_aliases.extend(position_branches.values())
for wires in branch_aliases:
    for value, alias in wires.items():
        if alias not in live:
            continue
        old_info = source_info.get(value)
        if old_info is not None:
            new_info = graph.value_info.add()
            new_info.CopyFrom(old_info)
            new_info.name = alias

for alias, literal in literal_types.items():
    if alias in live:
        graph.value_info.append(helper.make_tensor_value_info(alias, literal.data_type, literal.dims))

if family == "4b":
    # Preserve the declared FP32 logits without rounding the projection output to FP16.
    # The tied weight and the final hidden state retain their original FP16 storage.
    projection, = (node for node in graph.node if node.name == "/lm_head/MatMul")
    output_cast, = (node for node in graph.node if node.name == "/model/logits_output_cast")
    assert (projection.domain, projection.op_type, len(projection.input)) == ("", "MatMul", 2)
    assert (output_cast.domain, output_cast.op_type) == ("", "Cast")
    assert list(projection.output) == list(output_cast.input) == ["logits_fp16"]
    assert list(output_cast.output) == ["logits"]
    assert sum("logits_fp16" in node.input for node in graph.node) == 1
    logits, = (port for port in graph.output if port.name == "logits")
    assert logits.type.tensor_type.elem_type == TensorProto.FLOAT
    inputs = list(projection.input)
    graph.node.remove(projection)
    graph.node.remove(output_cast)
    for index in range(len(graph.value_info) - 1, -1, -1):
        if graph.value_info[index].name == "logits_fp16":
            del graph.value_info[index]
    # The Qwen TP assembler groups these nodes in its egress-only HEAD entry.
    for index, value in enumerate(inputs):
        graph.node.append(helper.make_node(
            "Cast", [value], [f"spec235_projection_fp32_input_{index}"],
            name=f"/lm_head/spec235_fp32/Cast{index}", to=TensorProto.FLOAT))
    graph.node.append(helper.make_node(
        "MatMul", ["spec235_projection_fp32_input_0", "spec235_projection_fp32_input_1"],
        ["logits"], name="/lm_head/spec235_fp32/MatMul"))

onnx.save(model, str(output))
print("HOST_SHAPE_SAME_OPERATOR_VARIANT", family, len(graph.node),
      attention_count, "one private shape branch per GQA", Counter(node.op_type for node in closure))
