#!/usr/bin/env python3
"""Freeze extended legacy identities and explicitly retain conversion failures."""
import hashlib
import importlib.util
import json
from pathlib import Path


def main():
    path = Path(__file__).with_name("generate-identity-vectors.py")
    spec = importlib.util.spec_from_file_location("fixed_reference", path)
    ref = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ref)
    source = ref.ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/onnx/graph.py"
    if (hashlib.sha256(source.read_bytes()).hexdigest() != ref.REFERENCE_GRAPH_SHA256
            or ref.onnx.__version__ != "1.17.0" or ref.np.__version__ != "1.24.4"):
        raise RuntimeError("legacy reference identity changed")
    cases = []
    for dtype in ["complex64", "complex128"]:
        for encoding, raw in [("typed", False), ("raw", True)]:
            name = dtype + "-" + encoding
            try:
                result = ref.reference_case(name, dtype, [complex(1.25, -2.5), complex(-3, 4)], raw)
                result["accepted"] = True
            except TypeError as error:
                if raw or str(error) != "can't convert complex to float":
                    raise
                result = {"name": name, "encoding": encoding, "accepted": False,
                          "boundary": "legacy_numpy_helper_conversion",
                          "error": "TypeError: " + str(error),
                          "values": [[1.25, -2.5], [-3.0, 4.0]]}
            cases.append(result)
    for name, bits, expected in [
        ("FLOAT8E4M3FN", [0x38, 0x40], bytes([0x38, 0x40])),
        ("FLOAT8E4M3FNUZ", [0x38, 0x40], bytes([0x38, 0x40])),
        ("FLOAT8E5M2", [0x38, 0x40], bytes([0x38, 0x40])),
        ("FLOAT8E5M2FNUZ", [0x38, 0x40], bytes([0x38, 0x40])),
        ("UINT4", [0x73], bytes([3, 7])),
        ("INT4", [0x7d], bytes([0xfd, 7])),
    ]:
        pair = []
        for encoding in ["typed", "raw"]:
            tensor = ref.TensorProto(name="weight", dims=[2], data_type=getattr(ref.TensorProto, name))
            if encoding == "raw":
                tensor.raw_data = bytes(bits)
            else:
                tensor.int32_data.extend(bits)
            result = ref.inspect_tensor(tensor)
            if result["tensorIndex"][0]["contentDigest"] != ref.digest(expected):
                raise RuntimeError(name + " does not match declared bit pattern")
            result.update(name=name + "-" + encoding, encoding=encoding,
                          accepted=True, contentDigest=ref.digest(expected))
            pair.append(result)
        for key in ["graphDigest", "initializerDigest"]:
            if pair[0][key] != pair[1][key]:
                raise RuntimeError(name + " packing identity differs")
        cases.extend(pair)
    print(json.dumps({"schema": "spec182-onnx-identity-extended-reference-v1",
                      "referenceOnnx": ref.onnx.__version__, "referenceNumpy": ref.np.__version__,
                      "referenceSourceSha256": ref.REFERENCE_GRAPH_SHA256,
                      "cases": cases}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
