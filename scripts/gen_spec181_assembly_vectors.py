#!/usr/bin/env python3
"""Generate fixed, non-secret production-entry assembly parity vectors."""

import argparse
from copy import deepcopy
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import sys
import tempfile

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
import onnxruntime

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.adapters.onnx.executor import (
    CertifiedOnnxAssemblyRecipe, assemble_certified_onnx_model)
from ndnsf_distributed_inference.adapters.onnx.graph import canonical_onnx_identity
from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def model_bytes(*, external, symbolic):
    batch = "batch" if symbolic else 1
    model = helper.make_model(helper.make_graph(
        [helper.make_node("MatMul", ["x", "weight"], ["middle"]),
         helper.make_node("Add", ["middle", "bias"], ["y"])],
        "spec181-assembly-v1",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [batch, 1])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [batch, 1])],
        [numpy_helper.from_array(np.asarray([[2.0]], dtype=np.float32), "weight"),
         numpy_helper.from_array(np.asarray([[1.0]], dtype=np.float32), "bias")],
        value_info=[helper.make_tensor_value_info("middle", TensorProto.FLOAT, [batch, 1])]),
        ir_version=8, opset_imports=[helper.make_opsetid("", 13)],
        producer_name="spec181-fixed-vector")
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "canonical.onnx"
        if external:
            onnx.save_model(model, path, save_as_external_data=True,
                            all_tensors_to_one_file=True, location="model.onnx.data",
                            size_threshold=0)
        else:
            path.write_bytes(model.SerializeToString(deterministic=True))
        identity = canonical_onnx_identity(path)
        return path.read_bytes(), ((path.parent / "model.onnx.data").read_bytes()
                                   if external else None), identity


def bind_root(row):
    source = bytes.fromhex(row["canonicalModelHex"])
    initializers = bytes.fromhex(row["initializerHex"]) if row["initializerHex"] else None
    metadata = {"canonicalSourceDataName": "/spec181/assembly/source",
                "canonicalSourceDigest": digest(source), "canonicalSourceBytes": len(source)}
    if initializers is not None:
        metadata.update(canonicalInitializerDataName="/spec181/assembly/initializers",
                        canonicalInitializerObjectDigest=digest(initializers),
                        canonicalInitializerBytes=len(initializers))
    row["rootManifest"] = canonical({
        "schema": "ndnsf-di-canonical-model-manifest-v1", "state": "ACTIVE",
        "modelName": "spec181-assembly-v1", "modelIdentityDigest": digest(b"model"),
        "artifactProfileDigest": row["recipe"]["artifact_profile_digest"], "metadata": metadata})
    root_digest = digest(row["rootManifest"].encode())
    row["role"]["model_manifest_digest"] = root_digest
    row["recipe"]["model_manifest_digest"] = root_digest
    row["role"]["recipe_digest"] = CertifiedOnnxAssemblyRecipe(**row["recipe"]).digest


def positive(name, *, external=False, symbolic=False, full=False):
    source, initializers, identity = model_bytes(external=external, symbolic=symbolic)
    shape = ["batch" if symbolic else 1, 1]
    recipe = CertifiedOnnxAssemblyRecipe(
        model_manifest_digest=digest(b"placeholder"), artifact_profile_digest=digest(b"profile"),
        graph_digest=identity.graph_digest,
        canonical_initializer_digest=identity.normalized_initializer_content_digest,
        adapter_descriptor_digest=digest(b"adapter"), assembler_descriptor_digest=digest(b"assembler"),
        backend_abi="onnxruntime-cpu-v1", role_kind="COMPONENT_SET" if full else "PIPELINE_RANGE",
        layer_begin=0, layer_end=0 if full else 1,
        node_indices=(0, 1) if full else (0,), input_names=("x",),
        output_names=("y" if full else "middle",),
        expected_inputs=({"name": "x", "dtype": "float32", "shape": shape},),
        expected_outputs=({"name": "y" if full else "middle", "dtype": "float32", "shape": shape},),
        precision="float32", max_source_bytes=65536, max_assembled_bytes=65536, max_nodes=8)
    role = dict(role="/spec181/role", rank=0, artifact_digest=digest(b"assignment"),
                adapter_id="onnx", adapter_version="1", backend="onnxruntime", device_set=["cpu"],
                recipe_digest=recipe.digest, protection_epoch="plaintext-v1",
                resource_envelope={"maxSourceBytes": 65536, "maxAssembledBytes": 65536, "maxNodes": 8})
    for key in ("model_manifest_digest", "artifact_profile_digest", "graph_digest",
                "canonical_initializer_digest", "adapter_descriptor_digest", "assembler_descriptor_digest",
                "backend_abi", "role_kind", "layer_begin", "layer_end", "node_indices",
                "expected_inputs", "expected_outputs", "precision", "quantization", "layout", "padding"):
        role[key] = getattr(recipe, key)
    row = dict(id=name, canonicalModelHex=source.hex(), initializerHex=(initializers or b"").hex(),
               role=role, recipe=asdict(recipe), expected={"accepted": True})
    bind_root(row)
    assembled = assemble_certified_onnx_model(source, canonical_initializer=initializers,
        role_spec=RoleAssemblySpec(**row["role"]), recipe=CertifiedOnnxAssemblyRecipe(**row["recipe"]))
    row["expected"].update(modelHex=assembled.model_bytes.hex(), modelDigest=assembled.model_digest)
    return row


def vectors():
    rows = [positive("inline-range"), positive("inline-component", full=True),
            positive("external-component", external=True, full=True),
            positive("symbolic-range", symbolic=True)]
    for name, key, value in [("wrong-recipe-digest", "recipe_digest", digest(b"wrong")),
                             ("wrong-backend-abi", "backend_abi", "different-abi"),
                             ("wrong-node-cover", "node_indices", [1])]:
        row = deepcopy(rows[0]); row["id"] = name; row["role"][key] = value
        row["expected"] = {"accepted": False, "boundary": "RECIPE_BINDING"}; rows.append(row)
    row = deepcopy(rows[2]); row["id"] = "authenticated-initializer-tamper"
    content = bytearray.fromhex(row["initializerHex"]); content[0] ^= 1
    row["initializerHex"] = content.hex(); bind_root(row)
    row["expected"] = {"accepted": False, "boundary": "INITIALIZER_IDENTITY"}; rows.append(row)
    return {"schema": "spec181-assembly-vectors-v1", "formatOwner": "certified-python-onnx-assembler",
            "entryPoints": ["assemble_certified_onnx_model", "prepareNativeCanonicalOnnxRole"],
            "toolchain": {"onnx": onnx.__version__, "onnxruntime": onnxruntime.__version__},
            "cases": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "tests/fixtures/spec181/assembly-vectors-v1.json")
    args = parser.parse_args()
    args.output.write_text(json.dumps(vectors(), sort_keys=True, indent=2, ensure_ascii=False) + "\n")
