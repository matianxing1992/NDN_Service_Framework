#!/usr/bin/env python3
"""Prepare an explicitly selected, immutable offline model package.

No inference is performed. A successful receipt proves the pinned files and
structural model contract, not native execution or numerical qualification.
"""
import argparse
import errno
import fcntl
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

from model_assets import (contained_file, file_digest, read_json, sealed_manifest,
                          validate_manifest, verify_file)

HERE = Path(__file__).resolve().parent
RECIPE_PATH = HERE / "recipes/qwen3-0.6b-int4.json"
MERGE_TOOL = "tools/ndnsf-di/merge-onnx-external-data.py"


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def tool_paths():
    # The installed copy retains the same relative layout beneath share/ndnsf.
    return (HERE.parent / "tools/ndnsf-di/normalize_onnx_constants.py",
            HERE / "TigerCluster/adapters/slurm-apptainer/scripts/stage-qwen-model.py")


def external_layout(recipe):
    """Return the frozen ONNX source and sidecar roles after checking path boundaries."""
    layout = recipe.get("externalOnnx")
    if layout is None:
        return None
    roles = layout["sidecarRoles"]
    if (set(layout) != {"sidecarRoles", "normalizeShapes"} or
            type(layout["normalizeShapes"]) is not bool or
            not isinstance(roles, list) or not roles or len(set(roles)) != len(roles) or
            "sourceModel" in roles):
        raise ValueError("ASSET_EXTERNAL_ONNX_LAYOUT_INVALID")
    paths = [Path(row["path"]) for row in recipe["files"].values()]
    if (len(set(paths)) != len(paths) or
            any(not path.parts or path.is_absolute() or ".." in path.parts for path in paths)):
        raise ValueError("ASSET_RECIPE_DESTINATION_INVALID")
    files = recipe["files"]
    source = Path(files["sourceModel"]["path"]).parent
    if (any(Path(files[role]["path"]).parent != source for role in roles) or
            not set(["sourceModel", *roles]) <= set(recipe["sourceFiles"]) or
            files["canonicalGraph"]["path"] != "canonical/model.onnx" or
            files["canonicalWeights"]["path"] != "canonical/model.onnx_data"):
        raise ValueError("ASSET_EXTERNAL_ONNX_LAYOUT_INVALID")
    return layout


def verify_tools(recipe):
    for name, version in recipe["toolVersions"].items():
        if importlib.metadata.version(name) != version:
            raise ValueError("ASSET_TOOL_VERSION_MISMATCH: {} requires {}".format(name, version))
    normalizer, downloader = tool_paths()
    transform = recipe["transforms"][0]
    normalizer_pin = transform["sha256"]
    if external_layout(recipe) is not None:
        if (len(recipe["transforms"]) != 1 or transform["tool"] != MERGE_TOOL or
                file_digest(HERE.parent / MERGE_TOOL) != transform["sha256"]):
            raise ValueError("ASSET_MERGE_TOOL_IDENTITY_MISMATCH")
        normalizer_pin = transform["normalizerSha256"]
    if file_digest(normalizer) != normalizer_pin:
        raise ValueError("ASSET_NORMALIZER_IDENTITY_MISMATCH")
    if not downloader.is_file():
        raise ValueError("ASSET_DOWNLOADER_MISSING")
    return normalizer, downloader


def copy_committed(source_root, source_name, destination_root, row, *, reuse=False):
    source = contained_file(source_root, source_name)
    if source.stat().st_size != row["bytes"] or file_digest(source) != row["sha256"]:
        raise ValueError("ASSET_SOURCE_COMMITMENT_MISMATCH: " + source_name)
    target = destination_root / row["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    if reuse:
        try:
            os.link(source, target)
            return
        except OSError as error:
            if error.errno != errno.EXDEV:
                raise
    with source.open("rb") as src, target.open("xb") as dst:
        shutil.copyfileobj(src, dst, 1024 * 1024)
    verify_file(destination_root, row)


def check_model(graph, recipe):
    import onnx
    model = onnx.load(str(graph), load_external_data=False)
    contract = recipe["modelContract"]
    graph_value = model.graph
    if (len(graph_value.node) != contract["nodes"] or
            len(graph_value.input) != contract["inputs"] or
            len(graph_value.output) != contract["outputs"]):
        raise ValueError("ASSET_MODEL_IO_OR_NODE_COUNT_MISMATCH")
    quantized = [node for node in graph_value.node
                 if node.domain == "com.microsoft" and node.op_type == "MatMulNBits"]
    quantization = contract["matMulNBits"]
    if len(quantized) != quantization["count"]:
        raise ValueError("ASSET_QUANTIZATION_COUNT_MISMATCH")
    for node in quantized:
        attributes = {attr.name: attr.i for attr in node.attribute}
        if any(attributes.get(key) != quantization[key] for key in ("bits", "block_size")):
            raise ValueError("ASSET_QUANTIZATION_ATTRIBUTE_MISMATCH")
    inputs = {value.name: value for value in graph_value.input}
    outputs = {value.name: value for value in graph_value.output}
    for name in ("input_ids", "attention_mask", "position_ids"):
        tensor = inputs[name].type.tensor_type
        if tensor.elem_type != onnx.TensorProto.INT64 or len(tensor.shape.dim) != 2:
            raise ValueError("ASSET_TOKEN_INPUT_CONTRACT_MISMATCH: " + name)
    if "logits" not in outputs:
        raise ValueError("ASSET_LOGITS_MISSING")
    kv_dtype = onnx.TensorProto.DataType.Value(contract.get("kvDtype", "FLOAT"))
    if kv_dtype not in (onnx.TensorProto.FLOAT, onnx.TensorProto.FLOAT16):
        raise ValueError("ASSET_KV_DTYPE_UNSUPPORTED")
    for layer in range(contract["layers"]):
        for kind in ("key", "value"):
            for prefix, values in (("past_key_values", inputs), ("present", outputs)):
                name = "{}.{}.{}".format(prefix, layer, kind)
                tensor = values[name].type.tensor_type
                dims = tensor.shape.dim
                if (tensor.elem_type != kv_dtype or len(dims) != 4 or
                        dims[1].dim_value != contract["kvHeads"] or
                        dims[3].dim_value != contract["headDimension"]):
                    raise ValueError("ASSET_KV_IO_CONTRACT_MISMATCH: " + name)
    for tensor in graph_value.initializer:
        if tensor.data_location != onnx.TensorProto.EXTERNAL:
            continue
        fields = {item.key: item.value for item in tensor.external_data}
        if fields.get("location") != "model.onnx_data":
            raise ValueError("ASSET_EXTERNAL_DATA_LOCATION_MISMATCH")
        sidecar = graph.parent / fields["location"]
        offset, length = int(fields.get("offset", "0")), int(fields["length"])
        if offset < 0 or length < 0 or offset + length > sidecar.stat().st_size:
            raise ValueError("ASSET_EXTERNAL_DATA_RANGE_INVALID")
    # This upstream ORT export uses fused operators, including standard-domain
    # SimplifiedLayerNormalization, absent from the generic ONNX schema registry.
    # Do not rewrite the committed graph to satisfy that registry. Exact file
    # identities and the checks above qualify preparation; the native ORT CPU
    # execution gate separately proves operator support and numerical behavior.


def prepare(recipe, root, source_dir, download, normalizer_path, downloader_path):
    import onnx
    files = recipe["files"]
    layout = external_layout(recipe)
    if source_dir is not None:
        for role, name in recipe["sourceFiles"].items():
            copy_committed(source_dir, name, root, files[role], reuse=layout is not None)
        copy_committed(source_dir, "LICENSE", root, files["license"], reuse=layout is not None)
    elif download:
        downloader = load_module(downloader_path, "ndnsf_model_download")
        required = {name: {key: files[role][key] for key in ("bytes", "sha256")}
                    for role, name in recipe["sourceFiles"].items()}
        downloader.public_snapshot_download(recipe["repository"], recipe["revision"],
                                            root / "source", required)
        license_source = recipe["license"]
        downloader.public_snapshot_download(
            license_source["repository"], license_source["revision"], root / "license",
            {"LICENSE": {key: files["license"][key] for key in ("bytes", "sha256")}})
    else:
        raise ValueError("ASSET_OFFLINE_MISS: provide --source-dir or explicitly --allow-download")
    (root / "preparation-state.json").write_text('{"state":"STAGED"}\n')
    canonical = root / files["canonicalGraph"]["path"]
    if layout is not None:
        source = root / files["sourceModel"]["path"]
        pins = {Path(files[role]["path"]).name: {key: files[role][key] for key in ("bytes", "sha256")}
                for role in ("sourceModel", *layout["sidecarRoles"])}
        committed_path = root / ".external-source-pins.json"
        committed_path.write_text(json.dumps(pins))
        args = [sys.executable, str(HERE.parent / MERGE_TOOL),
                "--source-root", str(source.parent), "--graph", source.name,
                "--commitments", str(committed_path), "--output", str(canonical.parent)]
        if layout["normalizeShapes"]:
            args.append("--normalize-shapes")
        subprocess.run(args, check=True)
        committed_path.unlink()
    else:
        model = onnx.load(str(root / files["sourceModel"]["path"]), load_external_data=False)
        normalizer = load_module(normalizer_path, "ndnsf_normalize_constants")
        normalizer.normalize(model)
        intermediate = root / "normalized.onnx"
        with intermediate.open("xb") as output:
            output.write(model.SerializeToString())
        transform = recipe["transforms"][0]
        if (intermediate.stat().st_size != transform["outputBytes"] or
                file_digest(intermediate) != transform["outputSha256"]):
            raise ValueError("ASSET_NORMALIZED_MODEL_IDENTITY_MISMATCH")
        intermediate.unlink()
        canonical.parent.mkdir()
        options = {key: value for key, value in recipe["transforms"][1].items() if key != "tool"}
        onnx.save_model(model, str(canonical), **options)
        del model
    for row in files.values():
        verify_file(root, row)
    check_model(canonical, recipe)
    (root / "preparation-state.json").write_text('{"state":"VERIFIED"}\n')
    pending = root / "asset-manifest.pending.json"
    pending.write_text(json.dumps(sealed_manifest(recipe), indent=2, sort_keys=True) + "\n")
    # SEALED appears only after every file and structural contract was checked.
    os.replace(str(pending), str(root / "asset-manifest.json"))
    (root / "preparation-state.json").unlink()


def interrupted(signum, frame):
    raise KeyboardInterrupt("model preparation cancelled by signal " + str(signum))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    recipes = parser.add_mutually_exclusive_group(required=True)
    recipes.add_argument("--recipe", choices=["qwen3-0.6b-int4"])
    recipes.add_argument("--recipe-file", type=Path,
                         help="explicit immutable external-ONNX recipe")
    parser.add_argument("--asset-root", required=True, type=Path)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--source-dir", type=Path)
    source.add_argument("--allow-download", action="store_true")
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args(argv)
    if args.offline and args.allow_download:
        parser.error("--offline conflicts with --allow-download")
    root = args.asset_root.expanduser().absolute()
    owned = False
    try:
        recipe = read_json(args.recipe_file or RECIPE_PATH)
        layout = external_layout(recipe)
        if args.recipe_file is not None and layout is None:
            raise ValueError("ASSET_RECIPE_FILE_REQUIRES_EXTERNAL_ONNX_LAYOUT")
        if root.is_symlink():
            raise ValueError("ASSET_ROOT_SYMLINK_REJECTED")
        if root.exists():
            validate_manifest(root / "asset-manifest.json", recipe)
            print(str(root / "asset-manifest.json"))
            return 0
        if args.source_dir is None and not args.allow_download:
            raise ValueError("ASSET_OFFLINE_MISS: provide --source-dir or explicitly --allow-download")
        normalizer, downloader = verify_tools(recipe)
        if not root.parent.is_dir():
            raise ValueError("ASSET_PARENT_MISSING: " + str(root.parent))
        needed = 4 * 1024 ** 3
        if layout is not None:
            files = recipe["files"]
            output_bytes = files["canonicalGraph"]["bytes"] + files["canonicalWeights"]["bytes"]
            same_device = args.source_dir is not None and (
                args.source_dir.stat().st_dev == root.parent.stat().st_dev)
            copied_bytes = 0 if same_device else sum(files[role]["bytes"]
                            for role in (*recipe["sourceFiles"], "license"))
            needed = max(needed, output_bytes + copied_bytes + 2 * 1024 ** 3)
        if shutil.disk_usage(root.parent).free < needed:
            raise ValueError("ASSET_CAPACITY_INSUFFICIENT: require {} bytes free".format(needed))
        lock_path = root.parent / ("." + root.name + ".prepare.lock")
        flags = os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW
        with os.fdopen(os.open(str(lock_path), flags, 0o600), "r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            root.mkdir(mode=0o700)
            owned = True
            previous = signal.signal(signal.SIGTERM, interrupted)
            try:
                prepare(recipe, root, args.source_dir, args.allow_download, normalizer, downloader)
                validate_manifest(root / "asset-manifest.json", recipe)
            finally:
                signal.signal(signal.SIGTERM, previous)
        print(str(root / "asset-manifest.json"))
        return 0
    except (Exception, KeyboardInterrupt) as error:
        if owned:
            shutil.rmtree(root)
        print("MODEL_ASSET_PREPARATION_FAILED: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
