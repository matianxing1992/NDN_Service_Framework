"""Operator-owned initial-request transfer shapes from the pinned ONNX source."""

from __future__ import annotations

import ast
import hashlib
from pathlib import Path


def _dimension(expression, bindings):
    """Evaluate only positive integer shape expressions, never Python code."""
    if isinstance(expression, int):
        value = expression
    else:
        def evaluate(node):
            if isinstance(node, ast.Constant) and type(node.value) is int:
                return node.value
            if isinstance(node, ast.Name) and node.id in bindings:
                return bindings[node.id]
            if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
                return evaluate(node.left) + evaluate(node.right)
            raise ValueError("unresolved source shape")
        value = evaluate(ast.parse(expression, mode="eval").body)
    if not 0 < value < 2**63:
        raise ValueError("nonpositive or overflowing transfer dimension")
    return value


def _shape_expression(expression):
    """Retain a bounded arithmetic tree for the native current-input binder."""
    if type(expression) is int:
        return expression
    def convert(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name):
            return node.id
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            return {"add": [convert(node.left), convert(node.right)]}
        raise ValueError("unsupported source shape expression")
    return convert(ast.parse(expression, mode="eval").body)


def initial_transfer_profile(source: Path, source_digest: str, node_mapping: dict,
                             stages: list[dict], ranges: list, input_payload: bytes,
                             token_count: int) -> dict:
    """Analyse source shapes without saving or replacing the inference graph.

    The exact input digest and empty initial KV context bound this profile.
    Continued, recovered or different inputs cannot reuse it in C++ planning.
    """
    import onnx
    from onnxruntime.tools.symbolic_shape_infer import SymbolicShapeInference

    source_bytes = source.read_bytes()
    if "sha256:" + hashlib.sha256(source_bytes).hexdigest() != source_digest:
        raise ValueError("transfer profile source digest mismatch")
    model = onnx.load_model_from_string(source_bytes)
    del source_bytes
    inference = SymbolicShapeInference(2**31 - 1, True, False, 0)
    # ORT's installed analyser handles ReduceSum axes inputs but leaves
    # opset-18 ReduceMean without a rank. Both have the same shape rule.
    opset = next(item.version for item in model.opset_import if item.domain == "")
    if opset >= 18:
        inference.dispatcher_["ReduceMean"] = inference._infer_ReduceSum
    inference._preprocess(model)
    complete = False
    for _ in range(8):
        if not inference.run_:
            break
        complete = inference._infer_impl()
    if inference.run_ or not complete:
        raise ValueError("incomplete source symbolic shape analysis")
    inferred = inference.out_mp_
    if (len(model.graph.node) != len(inferred.graph.node) or
            any(a.SerializeToString() != b.SerializeToString()
                for a, b in zip(model.graph.node, inferred.graph.node)) or
            len(model.graph.initializer) != len(inferred.graph.initializer) or
            any(a.SerializeToString() != b.SerializeToString()
                for a, b in zip(model.graph.initializer, inferred.graph.initializer))):
        raise ValueError("shape analysis changed source computation")
    ports = {value.name: value for value in
             list(inferred.graph.value_info) + list(inferred.graph.input) +
             list(inferred.graph.output)}
    weights = {value.name for value in model.graph.initializer}
    owners = []
    for index, (begin, end) in enumerate(ranges):
        keys = [f"layer-{layer:02d}" for layer in range(begin, end)]
        if index == 0:
            keys.append("embedding")
        if index == len(ranges) - 1:
            keys.append("final-norm-head")
        owners.append({node for key in keys for node in node_mapping[key]})
    all_consumers = {}
    for index, node in enumerate(model.graph.node):
        for name in node.input:
            all_consumers.setdefault(name, set()).add(index)
    boundaries = []
    for owned in owners:
        outputs = {name for index in owned for name in model.graph.node[index].output}
        inputs = {name for index in owned for name in model.graph.node[index].input}
        boundaries.append(((inputs - outputs) - weights,
                           {name for name in outputs
                            if all_consumers.get(name, set()) - owned}))
    bindings = {"batch_size": 1, "sequence_length": token_count,
                "past_sequence_length": 0}
    dtype_names = {onnx.TensorProto.FLOAT: "float32",
                   onnx.TensorProto.FLOAT16: "float16",
                   onnx.TensorProto.INT64: "int64",
                   onnx.TensorProto.BOOL: "bool",
                   onnx.TensorProto.UINT8: "uint8"}
    dependencies = []
    template_dependencies = []
    for index in range(len(stages) - 1):
        shared = (boundaries[index][0] | boundaries[index][1]) & boundaries[index + 1][0]
        if not shared:
            raise ValueError("empty transfer bundle")
        tensors = []
        template_tensors = []
        for name in sorted(shared):
            tensor = ports[name].type.tensor_type
            if not tensor.HasField("shape") or tensor.elem_type not in dtype_names:
                raise ValueError("unsupported transfer tensor")
            shape = [_dimension(dim.dim_value if dim.HasField("dim_value") else
                                dim.dim_param, bindings) for dim in tensor.shape.dim]
            tensors.append({"name": name, "dtype": dtype_names[tensor.elem_type],
                            "shape": shape})
            expressions = [dim.dim_value if dim.HasField("dim_value") else
                           dim.dim_param for dim in tensor.shape.dim]
            template_tensors.append({"name": name, "dtype": dtype_names[tensor.elem_type],
                                     "shape": [_shape_expression(x) for x in expressions]})
        dependencies.append({"producer_role": stages[index]["role"],
                             "consumer_role": stages[index + 1]["role"],
                             "tensors": tensors})
        template_dependencies.append({"producer_role": stages[index]["role"],
                                      "consumer_role": stages[index + 1]["role"],
                                      "tensors": template_tensors})
    token_input = next((value for value in model.graph.input if value.name == "input_ids"), None)
    if token_input is None:
        raise ValueError("source token input is missing")
    input_bindings = []
    for axis, dim in enumerate(token_input.type.tensor_type.shape.dim):
        if dim.dim_param:
            if not dim.dim_param.isidentifier():
                raise ValueError("unsupported source input dimension")
            input_bindings.append({"symbol": dim.dim_param, "tensor": token_input.name,
                                   "axis": axis})
    return {"schema": "ndnsf-di-request-transfer-profile-v1",
            "source_digest": source_digest,
            "input_digest": "sha256:" + hashlib.sha256(input_payload).hexdigest(),
            "initial_context": True, "dependencies": dependencies,
            "shape_template": {"bindings": input_bindings,
                               "past_sequence_symbol": "past_sequence_length",
                               "constants": {"past_sequence_length": 0},
                               "dependencies": template_dependencies}}
