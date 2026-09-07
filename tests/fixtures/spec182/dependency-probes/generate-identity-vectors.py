#!/usr/bin/env python3
"""Freeze stable pre-migration ONNX identity vectors; diagnose unstable encodings.

This is an offline T001 reference probe, never a native runtime dependency.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

ROOT = Path(__file__).resolve().parents[4]
REFERENCE_GRAPH_SHA256 = "e5532328e8752bd626b5a668de8e4b826ee9d336367a091fdd7016f48480734d"
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.adapters.onnx.graph import canonical_onnx_identity


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def inspect_tensor(tensor):
    model = helper.make_model(
        helper.make_graph(
            [helper.make_node("Identity", ["input"], ["output"])],
            "spec182-identity-reference",
            [helper.make_tensor_value_info("input", TensorProto.FLOAT, [2])],
            [helper.make_tensor_value_info("output", TensorProto.FLOAT, [2])],
            [tensor]),
        opset_imports=[helper.make_opsetid("", 13)], ir_version=8)
    onnx.checker.check_model(model, full_check=True)
    wire = model.SerializeToString(deterministic=True)
    with tempfile.TemporaryDirectory(prefix="spec182-identity-reference-") as directory:
        path = Path(directory) / "model.onnx"
        path.write_bytes(wire)
        identity = canonical_onnx_identity(path)
    return {
        "modelHex": wire.hex(), "modelDigest": digest(wire),
        "graphDigest": identity.graph_digest,
        "initializerDigest": identity.normalized_initializer_content_digest,
        "tensorIndex": list(identity.tensor_index),
    }


def reference_case(name, dtype, values, raw):
    array = np.asarray(values, dtype=dtype)
    if raw:
        tensor = numpy_helper.from_array(array, name="weight")
    else:
        tensor = helper.make_tensor("weight", helper.np_dtype_to_tensor_dtype(array.dtype),
                                    list(array.shape), array.flatten().tolist(), raw=False)
    result = inspect_tensor(tensor)
    expected = array.astype(array.dtype.newbyteorder("<"), copy=False).tobytes(order="C")
    assert result["tensorIndex"][0]["contentDigest"] == digest(expected), name
    result.update({"name": name, "encoding": "raw" if raw else "typed",
                   "contentDigest": digest(expected)})
    return result


def diagnostic_case(kind):
    tensor = TensorProto(name="weight", dims=[2])
    if kind == "bfloat16-raw":
        tensor.data_type = TensorProto.BFLOAT16
        tensor.raw_data = struct.pack("<HH", 0x3f80, 0x4000)
        expected = digest(tensor.raw_data)
    elif kind == "bfloat16-typed":
        tensor.data_type = TensorProto.BFLOAT16
        tensor.int32_data.extend([0x3f80, 0x4000])
        expected = digest(struct.pack("<HH", 0x3f80, 0x4000))
    elif kind == "string":
        tensor.data_type = TensorProto.STRING
        tensor.string_data.extend([b"alpha", "\u4f60".encode()])
        expected = None
    else:
        raise ValueError("unknown diagnostic")
    result = inspect_tensor(tensor)
    observed = result["tensorIndex"][0]
    # Deliberately omit pointer/uninitialized content bytes and unstable oracles.
    return {"name": kind, "modelDigest": result["modelDigest"],
            "dtype": observed["dtype"], "contentDigest": observed["contentDigest"],
            "expectedContentDigest": expected,
            "matchesDeclaredBits": None if expected is None else expected == observed["contentDigest"]}


def main():
    if onnx.__version__ != "1.17.0":
        raise RuntimeError("reference ONNX version changed")
    if np.__version__ != "1.24.4":
        raise RuntimeError("reference NumPy version changed")
    source = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/onnx/graph.py"
    if hashlib.sha256(source.read_bytes()).hexdigest() != REFERENCE_GRAPH_SHA256:
        raise RuntimeError("reference graph source changed; preserve the frozen v1 vectors")
    if len(sys.argv) == 2:
        print(json.dumps(diagnostic_case(sys.argv[1]), ensure_ascii=False, sort_keys=True))
        return
    if len(sys.argv) != 1:
        raise ValueError("expected no argument or one diagnostic name")
    cases = []
    for dtype, values in [
        ("float32", [1.25, -2.5]), ("float16", [1.25, -2.5]),
        ("float64", [1.25, -2.5]), ("int64", [-3, 7]),
        ("int32", [-3, 7]), ("int16", [-3, 7]), ("int8", [-3, 7]),
        ("uint64", [3, 7]), ("uint32", [3, 7]), ("uint16", [3, 7]),
        ("uint8", [3, 7]), ("bool", [True, False]),
    ]:
        pair = [reference_case(dtype + "-" + encoding, dtype, values, raw)
                for encoding, raw in [("typed", False), ("raw", True)]]
        assert pair[0]["graphDigest"] == pair[1]["graphDigest"], dtype
        assert pair[0]["initializerDigest"] == pair[1]["initializerDigest"], dtype
        cases.extend(pair)
    print(json.dumps({"schema": "spec182-onnx-identity-reference-v1",
                      "referenceOnnx": onnx.__version__, "referenceNumpy": np.__version__,
                      "referenceSourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                      "cases": cases}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
