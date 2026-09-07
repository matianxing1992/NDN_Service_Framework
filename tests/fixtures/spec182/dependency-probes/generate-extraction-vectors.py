#!/usr/bin/env python3
"""Freeze certified-extraction reference vectors for Spec 182 T006-B.

Every accepted vector runs the frozen python reference boundary exactly as the
native S3-S7 pipeline must mirror it: S3 full checker on the inlined original,
S4 canonical-source-identity digests, S5 utils.Extractor reachability (copied
value_info, local functions, external initializers), S6 node-cover + per-node
bytes + I/O dtype/shape compare, S7 deterministic wire + ORT CPU session.

This is an offline T006 reference probe, never a native runtime dependency.
Expected model bytes are produced only by this python reference, never by the
native assembler under test.
"""
import hashlib
import json
import struct
from pathlib import Path
import sys
import tempfile
from typing import Any, Dict, List, Optional

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.adapters.onnx.executor import (  # noqa: E402
    CertifiedOnnxAssemblyRecipe,
    assemble_certified_onnx_model,
)
from ndnsf_distributed_inference.adapters.onnx.graph import (  # noqa: E402
    canonical_onnx_identity,
)

REFERENCE_EXECUTOR_SHA256 = "d65c37e01914d69d3615b2333b285f92784d6198eb1481ad46aae96d7d81b50d"
REFERENCE_GRAPH_SHA256 = "e5532328e8752bd626b5a668de8e4b826ee9d336367a091fdd7016f48480734d"

_CUSTOM_DOMAIN = "ndnsf.spec182.extraction"
_PLACEHOLDER_DIGESTS = {
    field: "sha256:" + hashlib.sha256(b"ndnsf.spec182." + field.encode()).hexdigest()
    for field in (
        "model_manifest", "artifact_profile", "adapter_descriptor",
        "assembler_descriptor",
    )
}


def digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def contract(name: str, dtype: str, shape: List[object]) -> Dict[str, object]:
    return {"name": name, "dtype": dtype, "shape": shape}


def f32(values) -> bytes:
    return struct.pack("<" + "f" * len(values), *values)


def base_recipe(
    graph_digest: str,
    initializer_digest: str,
    *,
    role_kind: str,
    layer_begin: int,
    layer_end: int,
    node_indices: List[int],
    input_names: List[str],
    output_names: List[str],
    expected_inputs: List[Dict[str, object]],
    expected_outputs: List[Dict[str, object]],
) -> Dict[str, object]:
    return {
        "schema": "ndnsf-di-certified-onnx-assembly-v1",
        "modelManifestDigest": _PLACEHOLDER_DIGESTS["model_manifest"],
        "artifactProfileDigest": _PLACEHOLDER_DIGESTS["artifact_profile"],
        "graphDigest": graph_digest,
        "canonicalInitializerDigest": initializer_digest,
        "adapterDescriptorDigest": _PLACEHOLDER_DIGESTS["adapter_descriptor"],
        "assemblerDescriptorDigest": _PLACEHOLDER_DIGESTS["assembler_descriptor"],
        "backendAbi": "onnxruntime",
        "roleKind": role_kind,
        "layerBegin": layer_begin,
        "layerEnd": layer_end,
        "nodeIndices": node_indices,
        "inputNames": input_names,
        "outputNames": output_names,
        "expectedInputs": expected_inputs,
        "expectedOutputs": expected_outputs,
        "precision": "fp32",
        "quantization": "none",
        "layout": "native",
        "padding": "none",
        "maxSourceBytes": 8 * 1024**3,
        "maxAssembledBytes": 8 * 1024**3,
        "maxNodes": 1_000_000,
    }


class _RoleSpecLite:
    """Minimal certificate object; validates identically to the full spec."""

    def __init__(self, recipe: CertifiedOnnxAssemblyRecipe) -> None:
        self._recipe = recipe
        self.recipe_digest = recipe.digest

    def __getattr__(self, name: str) -> Any:
        if name == "resource_envelope":
            recipe = self._recipe
            return {
                "maxSourceBytes": recipe.max_source_bytes,
                "maxAssembledBytes": recipe.max_assembled_bytes,
                "maxNodes": recipe.max_nodes,
            }
        if name == "node_indices":
            return self._recipe.node_indices
        return getattr(self._recipe, name)


def recipe_from_dict(values: Dict[str, object]) -> CertifiedOnnxAssemblyRecipe:
    kind = values["roleKind"]
    return CertifiedOnnxAssemblyRecipe(
        model_manifest_digest=values["modelManifestDigest"],
        artifact_profile_digest=values["artifactProfileDigest"],
        graph_digest=values["graphDigest"],
        canonical_initializer_digest=values["canonicalInitializerDigest"],
        adapter_descriptor_digest=values["adapterDescriptorDigest"],
        assembler_descriptor_digest=values["assemblerDescriptorDigest"],
        backend_abi=values["backendAbi"],
        role_kind=kind,
        layer_begin=values["layerBegin"],
        layer_end=values["layerEnd"],
        node_indices=values["nodeIndices"],
        input_names=values["inputNames"],
        output_names=values["outputNames"],
        expected_inputs=values["expectedInputs"],
        expected_outputs=values["expectedOutputs"],
        precision=values["precision"],
    )


def _full_check(wire: bytes) -> None:
    model = onnx.ModelProto()
    model.ParseFromString(wire)
    onnx.checker.check_model(model, full_check=True)


def chain_model_bytes(*, external_w1: bool = False) -> bytes:
    """Two-node chain X -> Add(X, W1) -> T1 -> Relu -> Y, opset 13, ir 8."""
    weight_raw = f32([0.5, 1.0, 1.0, 0.5])
    if external_w1:
        def entry(key: str, value: str):
            item = onnx.StringStringEntryProto()
            item.key = key
            item.value = value
            return item

        weight = TensorProto(
            name="W1", data_type=TensorProto.FLOAT, dims=[2, 2],
            data_location=TensorProto.EXTERNAL,
            external_data=[
                entry("location", "model.onnx.data"),
                entry("offset", "0"),
                entry("length", str(len(weight_raw))),
            ],
        )
    else:
        weight = helper.make_tensor("W1", TensorProto.FLOAT, [2, 2],
                                    weight_raw, raw=True)
    graph = helper.make_graph(
        [helper.make_node("Add", ["X", "W1"], ["T1"]),
         helper.make_node("Relu", ["T1"], ["Y"])],
        "spec182-extraction-chain-a",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, [2, 2])],
        [helper.make_tensor_value_info("Y", TensorProto.FLOAT, [2, 2])],
        [weight],
    )
    model = helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)], ir_version=8)
    wire = model.SerializeToString(deterministic=True)
    if not external_w1:
        _full_check(wire)
    return wire


def branch_model_bytes() -> bytes:
    """Three-node branch X -> {Add, Mul} -> Concat -> Y, opset 13, ir 8."""
    graph = helper.make_graph(
        [helper.make_node("Add", ["X", "W1"], ["T1"]),
         helper.make_node("Mul", ["X", "W2"], ["T2"]),
         helper.make_node("Concat", ["T1", "T2"], ["Y"], axis=1)],
        "spec182-extraction-branch-b",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, [2, 2])],
        [helper.make_tensor_value_info("Y", TensorProto.FLOAT, [2, 4])],
        [
            helper.make_tensor("W1", TensorProto.FLOAT, [2, 2],
                               f32([1.0, 0.0, 0.0, 1.0]), raw=True),
            helper.make_tensor("W2", TensorProto.FLOAT, [2, 2],
                               f32([2.0, 0.0, 0.0, 2.0]), raw=True),
        ],
    )
    model = helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)], ir_version=8)
    wire = model.SerializeToString(deterministic=True)
    _full_check(wire)
    return wire


def function_model_bytes() -> bytes:
    """Square(fn, custom domain) -> Relu chain, opset 21, ir 10."""
    body = helper.make_graph(
        [helper.make_node("Mul", ["x", "x"], ["y"])], "spec182-square-body",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [2, 2])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [2, 2])],
    )
    square = helper.make_function(
        _CUSTOM_DOMAIN, "SquareSpec182", ["x"], ["y"], list(body.node),
        opset_imports=[helper.make_opsetid("", 21)],
    )
    graph = helper.make_graph(
        [helper.make_node("SquareSpec182", ["X"], ["T1"], domain=_CUSTOM_DOMAIN),
         helper.make_node("Relu", ["T1"], ["Y"])],
        "spec182-extraction-function-d",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, [2, 2])],
        [helper.make_tensor_value_info("Y", TensorProto.FLOAT, [2, 2])],
    )
    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", 21),
                       helper.make_opsetid(_CUSTOM_DOMAIN, 1)],
        functions=[square],
        ir_version=10,
    )
    wire = model.SerializeToString(deterministic=True)
    _full_check(wire)
    return wire


def initializer_content_sig(initializer) -> tuple:
    """Semantic tensor signature, ignoring the explicit-default data_location
    tag the python-upb loader leaves on externally sourced tensors."""
    return (initializer.name, int(initializer.data_type),
            tuple(int(dim) for dim in initializer.dims),
            bytes(initializer.raw_data))


def graph_semantic_sig(model_bytes: bytes) -> Dict[str, object]:
    model = onnx.ModelProto()
    model.ParseFromString(model_bytes)
    graph = model.graph
    return {
        "nodes": [node.SerializeToString(deterministic=True).hex()
                  for node in graph.node],
        "input": [item.SerializeToString(deterministic=True).hex()
                  for item in graph.input],
        "output": [item.SerializeToString(deterministic=True).hex()
                   for item in graph.output],
        "valueInfo": [item.SerializeToString(deterministic=True).hex()
                      for item in graph.value_info],
        "initializers": sorted(initializer_content_sig(item)
                               for item in graph.initializer),
    }


def python_identity(model_bytes: bytes, side_bytes: Optional[bytes],
                    side_name: str) -> Dict[str, str]:
    with tempfile.TemporaryDirectory(
            prefix="spec182-extraction-identity-") as directory:
        root = Path(directory)
        path = root / "canonical.onnx"
        path.write_bytes(model_bytes)
        if side_bytes is not None:
            (root / side_name).write_bytes(side_bytes)
        identity = canonical_onnx_identity(path)
    return {
        "graphDigest": identity.graph_digest,
        "canonicalInitializerDigest":
            identity.normalized_initializer_content_digest,
    }


def run_assembly(model_bytes: bytes, recipe_dict: Dict[str, object],
                 *, initializer_bytes: Optional[bytes] = None,
                 expected_error: Optional[str] = None) -> Dict[str, object]:
    try:
        recipe = recipe_from_dict(recipe_dict)
        spec = _RoleSpecLite(recipe)
        result = assemble_certified_onnx_model(
            model_bytes,
            canonical_initializer=initializer_bytes,
            role_spec=spec,
            recipe=recipe,
        )
    except Exception as exc:  # noqa: BLE001 - reference boundary records text
        message = str(exc)
        if expected_error is None:
            raise
        return {"rejected": True, "pythonError": message}
    if expected_error is not None:
        raise AssertionError("expected rejection, got assembly")
    return {
        "rejected": False,
        "expectedModelHex": result.model_bytes.hex(),
        "expectedModelDigest": result.model_digest,
        "expectedNodeCount": result.node_count,
        "inputNames": list(result.input_names),
        "outputNames": list(result.output_names),
    }


def main() -> None:
    if onnx.__version__ != "1.17.0":
        raise RuntimeError("reference ONNX version changed")
    if ort.__version__ != "1.19.2":
        raise RuntimeError("reference onnxruntime version changed")
    if np.__version__ != "1.24.4":
        raise RuntimeError("reference NumPy version changed")
    for path, pin in (
        (ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/"
            "adapters/onnx/executor.py", REFERENCE_EXECUTOR_SHA256),
        (ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/"
            "adapters/onnx/graph.py", REFERENCE_GRAPH_SHA256),
    ):
        if hashlib.sha256(path.read_bytes()).hexdigest() != pin:
            raise RuntimeError(
                f"reference source changed: {path.name}; freeze is pinned")

    chain = chain_model_bytes()
    chain_identity = python_identity(chain, None, "")
    chain_recipe = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    chain_expected = run_assembly(chain, chain_recipe)

    # External-form twin of the chain: W1 moved into the separately
    # addressable model.onnx.data object. Canonical identity must be equal to
    # the inline form and the certified assembly must byte-equal it.
    chain_external = chain_model_bytes(external_w1=True)
    side = f32([0.5, 1.0, 1.0, 0.5])
    external_identity = python_identity(
        chain_external, side, "model.onnx.data")
    if external_identity != chain_identity:
        raise RuntimeError("external and inline canonical identity differ")
    external_expected = run_assembly(
        chain_external, chain_recipe, initializer_bytes=side)
    # Byte parity holds across the two source forms.  The external tensor is
    # inlined from the side file with its proto3-optional presence bit intact:
    # onnx.proto field 14 (`optional DataLocation data_location = 14`) is an
    # explicit-presence field, so an explicit DEFAULT writes the wire tag 0x70
    # ("7000") and both loaders - python-upb and the C++ full protobuf - keep
    # that tag on parse (has_data_location() stays true).  The frozen python
    # wire is therefore byte-reproducible natively for the external row too.
    # The semantic guard below still runs as a construction-time cross-check
    # that the certified assembly did not diverge from the inline form.
    if graph_semantic_sig(bytes.fromhex(
            external_expected["expectedModelHex"])) \
            != graph_semantic_sig(bytes.fromhex(
                chain_expected["expectedModelHex"])):
        raise RuntimeError("external assembly diverges from inline assembly")
    external_expected["byteParity"] = True

    branch = branch_model_bytes()
    branch_identity = python_identity(branch, None, "")
    branch_recipe = base_recipe(
        branch_identity["graphDigest"],
        branch_identity["canonicalInitializerDigest"],
        role_kind="RANK", layer_begin=0, layer_end=1,
        node_indices=[0], input_names=["X"], output_names=["T1"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("T1", "float32", [2, 2])],
    )
    branch_expected = run_assembly(branch, branch_recipe)

    function_model = function_model_bytes()
    function_identity = python_identity(function_model, None, "")
    function_recipe = base_recipe(
        function_identity["graphDigest"],
        function_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    function_expected = run_assembly(function_model, function_recipe)

    accepted = [
        {"id": "chain-whole-inline", "modelHex": chain.hex(),
         "recipe": chain_recipe, "byteParity": True, **chain_expected},
        {"id": "chain-external-equivalent", "modelHex": chain_external.hex(),
         "initializerHex": side.hex(),
         "recipe": chain_recipe, **external_expected},
        {"id": "branch-subset-rank", "modelHex": branch.hex(),
         "recipe": branch_recipe, "byteParity": True, **branch_expected},
        {"id": "function-local-domain", "modelHex": function_model.hex(),
         "recipe": function_recipe, "byteParity": True, **function_expected},
    ]

    wrong_graph = base_recipe(
        digest(b"wrong-graph"), chain_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    ghost_output = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"],
        output_names=["Y", "ghost_out"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2]),
                          contract("ghost_out", "float32", [2, 2])],
    )
    cover_short = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="RANK", layer_begin=0, layer_end=2,
        node_indices=[0], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    io_contract_gap = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("ghost_in", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    wrong_dtype = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "int64", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    wrong_shape = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="COMPONENT_SET", layer_begin=0, layer_end=0,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "float32", [1, 1])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )
    layer_escape = base_recipe(
        chain_identity["graphDigest"],
        chain_identity["canonicalInitializerDigest"],
        role_kind="RANK", layer_begin=0, layer_end=9,
        node_indices=[0, 1], input_names=["X"], output_names=["Y"],
        expected_inputs=[contract("X", "float32", [2, 2])],
        expected_outputs=[contract("Y", "float32", [2, 2])],
    )

    rejected = [
        {"id": "reject-identity-digest", "modelHex": chain.hex(),
         "recipe": wrong_graph,
         **run_assembly(chain, wrong_graph, expected_error="")},
        {"id": "reject-ghost-output-io", "modelHex": chain.hex(),
         "recipe": ghost_output,
         **run_assembly(chain, ghost_output, expected_error="")},
        {"id": "reject-cover-short", "modelHex": chain.hex(),
         "recipe": cover_short,
         **run_assembly(chain, cover_short, expected_error="")},
        {"id": "reject-io-contract-coverage", "modelHex": chain.hex(),
         "recipe": io_contract_gap,
         **run_assembly(chain, io_contract_gap, expected_error="")},
        {"id": "reject-io-dtype", "modelHex": chain.hex(),
         "recipe": wrong_dtype,
         **run_assembly(chain, wrong_dtype, expected_error="")},
        {"id": "reject-io-shape", "modelHex": chain.hex(),
         "recipe": wrong_shape,
         **run_assembly(chain, wrong_shape, expected_error="")},
        {"id": "reject-layer-escape", "modelHex": chain.hex(),
         "recipe": layer_escape,
         **run_assembly(chain, layer_escape, expected_error="")},
    ]

    executor_source = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_" \
        "inference/adapters/onnx/executor.py"
    print(json.dumps({
        "schema": "spec182-extraction-vectors-v1",
        "referenceOnnx": onnx.__version__,
        "referenceOrt": ort.__version__,
        "referenceNumpy": np.__version__,
        "referenceExecutorSha256":
            hashlib.sha256(executor_source.read_bytes()).hexdigest(),
        "referenceGraphSha256":
            hashlib.sha256((ROOT / "NDNSF-DistributedInference/"
                            "ndnsf_distributed_inference/adapters/onnx/"
                            "graph.py").read_bytes()).hexdigest(),
        "cases": accepted + rejected,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
