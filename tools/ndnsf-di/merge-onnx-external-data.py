#!/usr/bin/env python3
"""Seal an ONNX graph with one external initializer file for native DI."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

GRAPH_LIMIT = 64 * 1024 * 1024
SIDECAR = "model.onnx_data"


def tensors(message):
    import onnx

    if isinstance(message, onnx.TensorProto):
        yield message
        return
    for field, value in message.ListFields():
        if field.type == field.TYPE_MESSAGE:
            children = value if field.label == field.LABEL_REPEATED else (value,)
            for child in children:
                yield from tensors(child)


def normalize_graph_shapes(model):
    import onnx
    from normalize_onnx_constants import normalize

    converted = normalize(model)
    initializers = {tensor.name: tensor for tensor in model.graph.initializer}
    for value in model.graph.value_info:
        tensor = initializers.get(value.name)
        if tensor is None:
            continue
        # Some exported Constants have scalar value_info despite a vector payload.
        metadata = onnx.helper.make_tensor_value_info(tensor.name, tensor.data_type, tensor.dims)
        value.type.CopyFrom(metadata.type)
    return onnx.shape_inference.infer_shapes(model), converted


def rewrite_external_ranges(model, sidecar_sizes):
    import onnx

    offsets = {}
    extent = 0
    for name in sorted(sidecar_sizes):
        offsets[name] = extent
        extent += sidecar_sizes[name]

    referenced = set()
    for tensor in tensors(model):
        if tensor.data_location != onnx.TensorProto.EXTERNAL:
            continue
        fields = {entry.key: entry.value for entry in tensor.external_data}
        if (len(tensor.external_data) != 3 or set(fields) != {"location", "offset", "length"} or
                fields["location"] not in offsets or
                any(re.fullmatch(r"[0-9]+", fields[key]) is None for key in ("offset", "length"))):
            raise ValueError("unsupported external tensor record: " + tensor.name)
        name = fields["location"]
        offset, size = int(fields["offset"]), int(fields["length"])
        if size <= 0 or offset + size > sidecar_sizes[name]:
            raise ValueError("external tensor range outside file: " + tensor.name)
        referenced.add(name)
        for entry in tensor.external_data:
            if entry.key == "location":
                entry.value = SIDECAR
            elif entry.key == "offset":
                entry.value = str(offsets[name] + offset)
    if not referenced or referenced != offsets.keys():
        raise ValueError("external files must exactly cover the graph references")
    return offsets, extent


def merge(root, graph_name, commitments, output, *, normalize_shapes=False):
    import onnx

    root = root.resolve(strict=True)
    files = {}
    for name, row in commitments.items():
        path = root / name
        if (Path(name).name != name or name in (".", "..") or path.is_symlink() or
                not path.is_file() or set(row) != {"bytes", "sha256"} or
                type(row["bytes"]) is not int or row["bytes"] <= 0 or
                re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is None or
                path.stat().st_size != row["bytes"]):
            raise ValueError("invalid model file commitment: " + name)
        files[name] = path
    if graph_name not in files or commitments[graph_name]["bytes"] > GRAPH_LIMIT:
        raise ValueError("missing or oversized ONNX graph")
    with files[graph_name].open("rb") as source:
        graph_bytes = source.read(GRAPH_LIMIT + 1)
    if (len(graph_bytes) != commitments[graph_name]["bytes"] or
            hashlib.sha256(graph_bytes).hexdigest() != commitments[graph_name]["sha256"]):
        raise ValueError("ONNX graph digest mismatch")
    model = onnx.load_model_from_string(graph_bytes)

    sidecar_sizes = {name: commitments[name]["bytes"] for name in files if name != graph_name}
    offsets, extent = rewrite_external_ranges(model, sidecar_sizes)
    if normalize_shapes:
        model, converted = normalize_graph_shapes(model)
    graph_bytes = model.SerializeToString()
    if len(graph_bytes) > GRAPH_LIMIT:
        raise ValueError("merged ONNX graph exceeds the native source limit")

    output.mkdir(mode=0o700)
    try:
        merged_hash = hashlib.sha256()
        with (output / SIDECAR).open("xb") as destination:
            for name in offsets:
                source_hash = hashlib.sha256()
                size = 0
                with files[name].open("rb") as source:
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        size += len(chunk)
                        if size > commitments[name]["bytes"]:
                            raise ValueError("external file grew: " + name)
                        source_hash.update(chunk)
                        merged_hash.update(chunk)
                        destination.write(chunk)
                if size != commitments[name]["bytes"] or source_hash.hexdigest() != commitments[name]["sha256"]:
                    raise ValueError("external file digest mismatch: " + name)
        (output / "model.onnx").write_bytes(graph_bytes)
        receipt = {
            "schema": "ndnsf-onnx-external-data-merge-v1",
            "source_graph": graph_name,
            "source_files": commitments,
            "files": {
                "model.onnx": {"bytes": len(graph_bytes), "sha256": hashlib.sha256(graph_bytes).hexdigest()},
                SIDECAR: {"bytes": extent, "sha256": merged_hash.hexdigest()},
            },
        }
        if normalize_shapes:
            receipt["shape_normalization"] = {"onnx": onnx.__version__, "constants": converted}
        with (output / ".receipt.json").open("x") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
            stream.write("\n")
        (output / ".receipt.json").rename(output / "receipt.json")
    except BaseException:
        shutil.rmtree(output)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--graph", required=True)
    parser.add_argument("--commitments", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--normalize-shapes", action="store_true",
                        help="lower Constants and infer shapes from verified graph metadata")
    args = parser.parse_args()
    merge(args.source_root, args.graph, json.loads(args.commitments.read_text()), args.output,
          normalize_shapes=args.normalize_shapes)


if __name__ == "__main__":
    main()
