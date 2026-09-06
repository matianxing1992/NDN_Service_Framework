#!/usr/bin/env python3
"""Offline, provider-independent YOLO26n canonical export for Spec180.

The exporter may load the checkpoint and use PyTorch/Ultralytics while
constructing the canonical package.  Those packages are export-time inputs;
the resulting manifest contains only ONNX graph/initializer metadata and the
signed candidate catalogue.  A missing or mismatched signing key fails closed.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any, Mapping


EXPECTED_CHECKPOINT_SHA256 = (
    "9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef"
)
CATALOGUE_SCHEMA = "spec180-yolo-catalogue-v1"
MANIFEST_SCHEMA = "spec180-yolo26n-canonical-v1"
GRAPH_REVISION = "spec180-yolo26n-graph-v1"
FIXTURE_REVISION = "spec180-fixed-fixture-v1"
DETECTION_CONFIDENCE_THRESHOLD = 1e-3
DEFAULT_FIXTURE = Path(__file__).resolve().parents[2] / (
    "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm")

_ONNX_DTYPE_NAMES = {
    1: "float32", 2: "uint8", 3: "int8", 4: "uint16", 5: "int16",
    6: "int32", 7: "int64", 9: "bool", 10: "float16", 11: "float64",
    12: "uint32", 13: "uint64", 16: "bfloat16",
}


class ExportError(RuntimeError):
    """Fail-closed export or signing error."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def sha256_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _load_registry(path: Path) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ExportError(f"invalid trust-root registry: {path}") from exc
    if value.get("status") != "CONFIGURED":
        raise ExportError("trust-root registry is not CONFIGURED")
    entry = value.get("catalogue")
    if not isinstance(entry, Mapping):
        raise ExportError("catalogue trust-root entry is missing")
    return entry


def _registry_feature_dir(registry_path: Path) -> Path:
    """Return the directory against which a registry key path is resolved."""
    if (registry_path.parent / "spec.md").exists():
        return registry_path.parent
    if registry_path.parent.name == "contracts":
        return registry_path.parent.parent
    return registry_path.parent


def _registered_public_key_bytes(registry_path: Path,
                                 entry: Mapping[str, Any]) -> bytes:
    """Load and digest-check the public key named by a trust-root registry."""
    public_key_path = entry.get("publicKeyPath")
    if not isinstance(public_key_path, str) or not public_key_path:
        raise ExportError("catalogue trust root public key path is missing")
    public_path = _registry_feature_dir(registry_path) / public_key_path
    try:
        public_bytes = public_path.read_bytes()
    except OSError as exc:
        raise ExportError(f"cannot read registered catalogue public key: {public_path}") from exc
    expected_digest = str(entry.get("publicKeySha256", ""))
    if not expected_digest.startswith("sha256:"):
        raise ExportError("catalogue trust root public key digest is invalid")
    actual_digest = hashlib.sha256(public_bytes).hexdigest()
    if actual_digest != expected_digest[7:]:
        raise ExportError("registered catalogue public key file digest does not match")
    return public_bytes


def _load_private_key(path: Path):
    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        try:
            from cryptography.hazmat.backends import default_backend
            key = serialization.load_pem_private_key(
                path.read_bytes(), password=None, backend=default_backend())
        except ImportError:  # pragma: no cover - modern cryptography
            key = serialization.load_pem_private_key(
                path.read_bytes(), password=None)
    except (OSError, ValueError, TypeError) as exc:
        raise ExportError(f"cannot load Ed25519 signing key: {path}") from exc
    if not isinstance(key, Ed25519PrivateKey):
        raise ExportError("catalogue signing key must be Ed25519")
    return key


def _sign_catalogue(catalogue: Mapping[str, Any], *, key_path: Path,
                    registry_path: Path) -> dict[str, Any]:
    entry = _load_registry(registry_path)
    key = _load_private_key(key_path)
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    raw_public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    if entry.get("publicKeyAlgorithm") != "ed25519" or entry.get("signatureAlgorithm") != "ed25519":
        raise ExportError("catalogue trust root must use Ed25519")
    # The registry digest covers the exact bytes of the checked-in public-key
    # file (which may be PEM or raw), not an implicitly re-encoded key.  Compare
    # the parsed Ed25519 key as well, so an equivalent encoding is accepted but
    # a different private key can never sign for the registered authority.
    registered_bytes = _registered_public_key_bytes(registry_path, entry)
    try:
        from cryptography.hazmat.backends import default_backend
        registered_key = serialization.load_pem_public_key(
            registered_bytes, backend=default_backend())
    except ImportError:  # pragma: no cover - modern cryptography
        registered_key = serialization.load_pem_public_key(registered_bytes)
    except ValueError:
        try:
            registered_key = Ed25519PublicKey.from_public_bytes(registered_bytes)
        except (ValueError, TypeError) as exc:
            raise ExportError("registered catalogue public key is invalid") from exc
    if not isinstance(registered_key, Ed25519PublicKey):
        raise ExportError("registered catalogue public key must be Ed25519")
    registered_raw = registered_key.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    if registered_raw != raw_public:
        raise ExportError("signing key public key does not match registered key")
    body = dict(catalogue)
    body.pop("signature", None)
    signature = key.sign(canonical_bytes(body))
    body["signature"] = {
        "authorityId": entry["authorityId"],
        "keyId": entry["keyId"],
        "algorithm": "ed25519",
        "valueB64": base64.b64encode(signature).decode("ascii"),
    }
    return body


def _load_yolo(model_path: Path):
    split_dir = Path(__file__).resolve().parents[2] / (
        "examples/python/NDNSF-DistributedInference/yolo_split")
    if str(split_dir) not in sys.path:
        sys.path.insert(0, str(split_dir))
    from yolo_split_lib import YoloFull, load_yolo_model  # noqa: PLC0415
    selected, model = load_yolo_model(str(model_path))
    if Path(selected).resolve() != model_path.resolve():
        raise ExportError(f"Ultralytics selected unexpected checkpoint: {selected}")
    return YoloFull(model).eval()


def _export_onnx(model_path: Path, graph_path: Path, *, input_size: int,
                 opset: int) -> None:
    try:
        import torch
        import onnx
    except ImportError as exc:
        raise ExportError("torch and onnx are required only for offline export") from exc
    model = _load_yolo(model_path)
    graph_path.parent.mkdir(parents=True, exist_ok=True)
    sample = torch.zeros(1, 3, input_size, input_size, dtype=torch.float32)
    temporary = graph_path.with_suffix(".embedded.onnx")
    torch.onnx.export(
        model,
        sample,
        str(temporary),
        input_names=["images"],
        output_names=["predictions"],
        opset_version=opset,
        do_constant_folding=True,
    )
    loaded = onnx.load(str(temporary))
    onnx.checker.check_model(loaded)
    # Always externalize initializers so the package has an explicit weight
    # object even when this small checkpoint would fit in one graph file.
    onnx.save_model(
        loaded,
        str(graph_path),
        save_as_external_data=True,
        all_tensors_to_one_file=True,
        location=graph_path.stem + ".weights",
        size_threshold=0,
    )
    temporary.unlink(missing_ok=True)


def _fixture_tensor(path: Path, input_size: int):
    """Read the repository-owned P3 fixture and resize it deterministically."""
    try:
        import numpy as np
        import torch
        import torch.nn.functional as F
    except ImportError as exc:
        raise ExportError("torch and numpy are required for the oracle") from exc
    tokens = [item for item in path.read_text(encoding="ascii").split()
              if not item.startswith("#")]
    if len(tokens) < 4 or tokens[0] != "P3":
        raise ExportError("fixed fixture is not a P3 image")
    width, height, maximum = (int(tokens[1]), int(tokens[2]), int(tokens[3]))
    values = [int(item) for item in tokens[4:]]
    if (width <= 0 or height <= 0 or maximum != 255
            or len(values) != width * height * 3
            or any(item < 0 or item > maximum for item in values)):
        raise ExportError("fixed fixture dimensions or values are invalid")
    image = np.asarray(values, dtype=np.float32).reshape(height, width, 3)
    tensor = torch.from_numpy(image / 255.0).permute(2, 0, 1).unsqueeze(0)
    return F.interpolate(tensor, size=(int(input_size), int(input_size)),
                         mode="bilinear", align_corners=False)


def _canonicalize_detection_rows(array):
    """Normalize NMS rows for PyTorch/ONNX Runtime equivalence."""
    import numpy as np

    values = np.asarray(array)
    if values.ndim != 3 or values.shape[0] != 1 or values.shape[2] != 6:
        raise ExportError("YOLO detection oracle has an unsupported shape")
    # Rows are [x1, y1, x2, y2, confidence, class].  Very low-confidence NMS
    # rows are not stable across PyTorch and ONNX Runtime at the 300-row cap;
    # the declared application contract excludes them before canonical sort.
    values = values[:, values[0, :, 4] >= DETECTION_CONFIDENCE_THRESHOLD, :]
    order = np.lexsort((values[0, :, 3], values[0, :, 2], values[0, :, 1],
                        values[0, :, 0], values[0, :, 5], -values[0, :, 4]))
    return values[:, order, :]


def _oracle_manifest(model_path: Path, fixture_path: Path, output_path: Path,
                     *, input_size: int) -> dict[str, Any]:
    try:
        import numpy as np
    except ImportError as exc:
        raise ExportError("numpy is required for the oracle") from exc
    model = _load_yolo(model_path)
    tensor = _fixture_tensor(fixture_path, input_size)
    with __import__("torch").no_grad():
        output = model(tensor)
    if output is None:
        raise ExportError("YOLO full-model oracle returned no tensor")
    array = _canonicalize_detection_rows(
        output.detach().cpu().numpy().astype(np.float32, copy=False))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    np.save(str(output_path), array, allow_pickle=False)
    return {
        "fixtureRevision": FIXTURE_REVISION,
        "fixtureSha256": sha256_file(fixture_path),
        "inputShape": [1, 3, int(input_size), int(input_size)],
        "outputShape": [int(value) for value in array.shape],
        "outputDtype": str(array.dtype),
        "outputPath": str(output_path.name),
        "outputDigest": "sha256:" + sha256_file(output_path),
    }


def graph_metadata(graph_path: Path) -> dict[str, Any]:
    try:
        import onnx
        loaded = onnx.load(str(graph_path), load_external_data=True)
        try:
            loaded = onnx.shape_inference.infer_shapes(loaded)
        except Exception:
            # Shape inference is advisory; unknown dimensions remain explicit
            # in the semantic contract rather than being guessed.
            pass
        onnx.checker.check_model(loaded)
    except (ImportError, OSError, ValueError) as exc:
        raise ExportError(f"canonical ONNX graph is invalid: {graph_path}") from exc
    def value_info_contract(value: Any) -> dict[str, Any]:
        tensor_type = getattr(getattr(value, "type", None), "tensor_type", None)
        if tensor_type is None:
            return {"dtype": "unknown", "shape": []}
        dtype = _ONNX_DTYPE_NAMES.get(int(tensor_type.elem_type),
                                      f"onnx:{int(tensor_type.elem_type)}")
        shape = []
        if tensor_type.HasField("shape"):
            for dim in tensor_type.shape.dim:
                if dim.HasField("dim_value"):
                    shape.append(int(dim.dim_value))
                elif dim.dim_param:
                    shape.append(str(dim.dim_param))
                else:
                    shape.append("?")
        return {"dtype": dtype, "shape": shape}

    tensors: dict[str, dict[str, Any]] = {}
    for value in (list(loaded.graph.input) + list(loaded.graph.value_info)
                  + list(loaded.graph.output)):
        if value.name:
            tensors[str(value.name)] = value_info_contract(value)
    for initializer in loaded.graph.initializer:
        tensors[str(initializer.name)] = {
            "dtype": _ONNX_DTYPE_NAMES.get(int(initializer.data_type),
                                            f"onnx:{int(initializer.data_type)}"),
            "shape": [int(dim) for dim in initializer.dims],
        }
    nodes = [{
        "index": index,
        "name": str(node.name or f"{index}:{node.op_type}"),
        "opType": str(node.op_type),
        "inputs": [str(value) for value in node.input if value],
        "outputs": [str(value) for value in node.output if value],
    } for index, node in enumerate(loaded.graph.node)]
    return {
        "graphDigest": "sha256:" + sha256_file(graph_path),
        "graphBytes": graph_path.stat().st_size,
        "inputs": [item.name for item in loaded.graph.input],
        "outputs": [item.name for item in loaded.graph.output],
        "initializers": [
            {
                "name": item.name,
                "dims": [int(value) for value in item.dims],
                "external": bool(item.data_location),
            }
            for item in loaded.graph.initializer
        ],
        "nodeCount": len(loaded.graph.node),
        "nodeNames": [str(item.name or item.op_type) for item in loaded.graph.node],
        "nodes": nodes,
        "tensors": tensors,
    }


_MODEL_NODE_RE = re.compile(r"/model/model\.(\d+)(?:/|$)")
_DETECT_SCALE_RE = re.compile(
    r"/model/model\.23/(?:one2one_)?cv[23]\.(\d+)(?:/|$)")


def _scoped_semantic_role(node_name: str) -> tuple[str, str] | None:
    """Return the role for a node carrying a registered Ultralytics scope."""
    match = _MODEL_NODE_RE.search(node_name)
    if match is None:
        return None
    module = int(match.group(1))
    if module < 23:
        return "BackboneNeck", "backbone-neck"
    if module != 23:
        raise ExportError("YOLO graph contains an unsupported model scope: " + node_name)
    scale = _DETECT_SCALE_RE.search(node_name)
    if scale is not None:
        scale_id = int(scale.group(1))
        if scale_id == 0:
            return "DetectShard0", "detect-scale-0"
        if scale_id in (1, 2):
            return "DetectShard1", f"detect-scale-{scale_id}"
        raise ExportError("YOLO Detect scope has an unsupported scale: " + node_name)
    return "Merge", "postprocess-merge"


def _semantic_role(node_name: str, *, consumers_by_tensor: Mapping[str, list[str]],
                   node_outputs: Mapping[str, list[str]]) -> tuple[str, str]:
    """Classify a known YOLO26n graph node by model semantics.

    The exporter intentionally recognizes named architecture scopes rather
    than deriving a cut from node count, topological percentage, or an index.
    Unknown scopes fail closed so a changed graph cannot silently inherit the
    old partition.
    """
    scoped = _scoped_semantic_role(node_name)
    if scoped is not None:
        return scoped
    # ONNX exporters occasionally lift a shape/split Constant out of the
    # module scope.  Classify it by its data-flow consumer, never by a graph
    # index.  Ambiguous or orphaned lifted nodes fail closed.
    pending = list(node_outputs.get(node_name, ()))
    visited = {node_name}
    found: set[tuple[str, str]] = set()
    while pending:
        tensor = pending.pop(0)
        for consumer in consumers_by_tensor.get(tensor, ()):
            if consumer in visited:
                continue
            visited.add(consumer)
            role = _scoped_semantic_role(consumer)
            if role is not None:
                found.add(role)
                continue
            pending.extend(node_outputs.get(consumer, ()))
    if len(found) != 1:
        raise ExportError("YOLO graph node has no unambiguous semantic scope: " + node_name)
    return next(iter(found))


def semantic_partition(metadata: Mapping[str, Any]) -> dict[str, Any]:
    """Build a signed, graph-bound semantic partition certificate."""
    nodes = metadata.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise ExportError("YOLO graph node metadata is missing")
    role_node_sets: dict[str, list[str]] = {
        "BackboneNeck": [], "DetectShard0": [], "DetectShard1": [], "Merge": [],
    }
    branch_ownership: dict[str, str] = {}
    node_role: dict[str, str] = {}
    node_branch: dict[str, str] = {}
    consumers_by_tensor: dict[str, list[str]] = {}
    node_outputs: dict[str, list[str]] = {}
    for node in nodes:
        name = str(node.get("name", ""))
        inputs = [str(value) for value in node.get("inputs", [])]
        outputs = [str(value) for value in node.get("outputs", [])]
        node_outputs[name] = outputs
        for tensor in inputs:
            consumers_by_tensor.setdefault(tensor, []).append(name)
    for node in nodes:
        if not isinstance(node, Mapping):
            raise ExportError("YOLO graph node metadata is invalid")
        name = str(node.get("name", ""))
        role, branch = _semantic_role(
            name, consumers_by_tensor=consumers_by_tensor,
            node_outputs=node_outputs)
        if name in node_role:
            raise ExportError("YOLO graph node names are duplicated")
        node_role[name] = role
        node_branch[name] = branch
        role_node_sets[role].append(name)
        branch_ownership[branch] = role
    if any(not values for values in role_node_sets.values()):
        raise ExportError("YOLO semantic partition does not cover all roles")

    producers: dict[str, str] = {}
    consumers: dict[str, list[str]] = {}
    for node in nodes:
        name = str(node["name"])
        for tensor in node.get("outputs", []):
            tensor = str(tensor)
            if tensor in producers:
                raise ExportError("YOLO graph tensor has multiple producers: " + tensor)
            producers[tensor] = name
        for tensor in node.get("inputs", []):
            tensor = str(tensor)
            consumers.setdefault(tensor, []).append(name)
    tensor_meta = metadata.get("tensors")
    if not isinstance(tensor_meta, Mapping):
        tensor_meta = {}
    interfaces: list[dict[str, Any]] = []
    dependencies: dict[tuple[str, str], set[str]] = {}
    for tensor, producer in sorted(producers.items()):
        producer_role = node_role[producer]
        all_consumer_names = tuple(sorted(set(consumers.get(tensor, ()))))
        consumer_names = tuple(
            name for name in all_consumer_names if node_role[name] != producer_role)
        consumer_roles = tuple(sorted({node_role[name] for name in consumer_names}))
        if not consumer_roles:
            continue
        info = tensor_meta.get(tensor, {})
        interface = {
            "edgeId": tensor,
            "producerNode": producer,
            "producerRole": producer_role,
            "consumerNodes": list(consumer_names),
            "consumerRoles": list(consumer_roles),
            "dtype": str(info.get("dtype", "unknown")),
            "shape": list(info.get("shape", [])),
        }
        interfaces.append(interface)
        for consumer_role in consumer_roles:
            dependencies.setdefault((producer_role, consumer_role), set()).add(tensor)
    if not interfaces:
        raise ExportError("YOLO semantic partition has no tensor interfaces")
    role_inputs: dict[str, set[str]] = {role: set() for role in role_node_sets}
    role_outputs: dict[str, set[str]] = {role: set() for role in role_node_sets}
    for interface in interfaces:
        producer_role = str(interface["producerRole"])
        role_outputs[producer_role].add(str(interface["edgeId"]))
        for consumer_role in interface["consumerRoles"]:
            role_inputs[str(consumer_role)].add(str(interface["edgeId"]))
    # Bind the application boundary as well as inter-role tensors.  This keeps
    # the ingress/egress contract explicit even though graph inputs/outputs do
    # not have an ONNX node producer/consumer pair.
    input_names = {str(value) for value in metadata.get("inputs", ())}
    output_names = {str(value) for value in metadata.get("outputs", ())}
    for node in nodes:
        name = str(node["name"])
        role = node_role[name]
        role_inputs[role].update(
            str(value) for value in node.get("inputs", ()) if str(value) in input_names)
        role_outputs[role].update(
            str(value) for value in node.get("outputs", ()) if str(value) in output_names)

    def contract(name: str) -> dict[str, Any]:
        info = tensor_meta.get(name, {})
        return {
            "name": name,
            "dtype": str(info.get("dtype", "unknown")),
            "shape": list(info.get("shape", [])),
        }

    role_interfaces = {
        role: {
            "inputs": [contract(name) for name in sorted(role_inputs[role])],
            "outputs": [contract(name) for name in sorted(role_outputs[role])],
        }
        for role in sorted(role_node_sets)
    }
    if any(not item["inputs"] or not item["outputs"]
           for item in role_interfaces.values()):
        raise ExportError("YOLO semantic role interface is incomplete")
    safe_cuts = []
    for (from_role, to_role), tensors in sorted(dependencies.items()):
        rows = [item for item in interfaces
                if item["producerRole"] == from_role and to_role in item["consumerRoles"]]
        safe_cuts.append({
            "cutId": f"{from_role}-to-{to_role}",
            "fromRole": from_role,
            "toRole": to_role,
            "boundaryTensors": sorted(tensors),
            "producerNodes": sorted({item["producerNode"] for item in rows}),
            "consumerNodes": sorted({name for item in rows for name in item["consumerNodes"]
                                      if node_role[name] == to_role}),
        })
    return {
        "schema": "spec180-yolo-semantic-partition-v1",
        "roleNodeSets": {role: sorted(values) for role, values in role_node_sets.items()},
        "branchOwnership": dict(sorted(branch_ownership.items())),
        "nodeBranch": dict(sorted(node_branch.items())),
        "tensorInterfaces": interfaces,
        "roleInterfaces": role_interfaces,
        "dependencyEdges": [
            {"fromRole": from_role, "toRole": to_role, "tensorEdges": sorted(tensors)}
            for (from_role, to_role), tensors in sorted(dependencies.items())
        ],
        "safeCuts": safe_cuts,
        "equivalence": {
            "oraclePath": "oracle/full-model-output.npy",
            "method": "pytorch-reference-compared-with-cpu-onnxruntime",
            "atol": 1e-3,
            "rtol": 1e-4,
        },
    }


def candidate_catalogue(*, graph_digest: str, model_digest: str,
                        safe_cuts: list[dict[str, Any]],
                        semantic_partition: Mapping[str, Any] | None = None) -> dict[str, Any]:
    """Return the two provider-independent candidate families.

    The emitted order is deterministic for reproducible manifests, but the
    order is not a selection authority; the signed per-candidate priority is
    applied only after ACK-owned feasibility checks.
    """
    if semantic_partition is None:
        raise ExportError("signed semantic partition is required for YOLO candidates")
    candidates = [
        {
            "candidateId": "atomic-v1",
            # Shared placement is preferred only after all four roles are
            # feasible; atomic remains the lower-priority fallback.
            "selectionPriority": 10,
            "roles": [{"role": "FullModel", "kind": "COMPONENT_SET"}],
            "inputIngressRole": "FullModel",
            "resultEgressRole": "FullModel",
            "mergeKind": "NATIVE_POSTPROCESS",
            "safeCuts": [],
            "semanticPartition": {},
        },
        {
            "candidateId": "shared-backbone-two-shard-v1",
            "selectionPriority": 20,
            "roles": [
                {"role": "BackboneNeck", "kind": "COMPONENT_SET"},
                {"role": "DetectShard0", "kind": "COMPONENT_SET"},
                {"role": "DetectShard1", "kind": "COMPONENT_SET"},
                {"role": "Merge", "kind": "COMPONENT_SET"},
            ],
            "inputIngressRole": "BackboneNeck",
            "resultEgressRole": "Merge",
            "mergeKind": "NATIVE_POSTPROCESS",
            "safeCuts": safe_cuts,
            "semanticPartition": dict(semantic_partition),
        },
    ]
    for candidate in candidates:
        candidate["candidateDigest"] = sha256_digest(canonical_bytes(candidate))
    return {
        "schema": CATALOGUE_SCHEMA,
        "revision": "spec180-yolo26n-catalogue-v1",
        "modelFamily": "YOLO26n",
        "modelDigest": model_digest,
        "graphDigest": graph_digest,
        "candidates": candidates,
    }


def build_package(model_path: Path, output_dir: Path, *, input_size: int = 640,
                  opset: int = 17, signing_key: Path | None = None,
                  registry_path: Path | None = None,
                  fixture_path: Path | None = None) -> dict[str, Any]:
    if not model_path.is_file():
        raise ExportError(f"checkpoint is missing: {model_path}")
    actual = sha256_file(model_path)
    if actual != EXPECTED_CHECKPOINT_SHA256:
        raise ExportError("checkpoint digest does not match the registered YOLO26n source")
    output_dir.mkdir(parents=True, exist_ok=True)
    graph_path = output_dir / "canonical" / "yolo26n.onnx"
    _export_onnx(model_path, graph_path, input_size=input_size, opset=opset)
    metadata = graph_metadata(graph_path)
    weights_path = graph_path.with_name(graph_path.stem + ".weights")
    if not weights_path.is_file():
        raise ExportError("external ONNX initializer file was not produced")
    partition = semantic_partition(metadata)
    safe_cuts = list(partition["safeCuts"])
    catalogue = candidate_catalogue(
        graph_digest=metadata["graphDigest"],
        model_digest="sha256:" + actual,
        safe_cuts=safe_cuts,
        semantic_partition=partition,
    )
    if signing_key is not None:
        if registry_path is None:
            raise ExportError("registry_path is required when signing")
        catalogue = _sign_catalogue(
            catalogue, key_path=signing_key, registry_path=registry_path)
    elif registry_path is not None:
        raise ExportError("a signing key is required for a signed catalogue")
    fixture = Path(fixture_path or DEFAULT_FIXTURE)
    if not fixture.is_file():
        raise ExportError(f"fixed YOLO fixture is missing: {fixture}")
    oracle = _oracle_manifest(
        model_path, fixture, output_dir / "oracle" / "full-model-output.npy",
        input_size=input_size)
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "modelFamily": "YOLO26n",
        "source": {
            "checkpointName": model_path.name,
            "checkpointSha256": actual,
            "checkpointBytes": model_path.stat().st_size,
        },
        "export": {
            "opset": int(opset),
            "dtype": "float32",
            "batch": 1,
            "inputShape": [1, 3, int(input_size), int(input_size)],
            "options": "do_constant_folding=true;external_initializers=true",
        },
        "graph": metadata,
        "graphRevision": GRAPH_REVISION,
        "preprocessing": {
            "identity": "float32-NCHW-RGB-0-to-1",
            "inputName": "images",
            "shape": [1, 3, int(input_size), int(input_size)],
        },
        "postprocessing": {
            "identity": "YOLO26n-canonical-detection-rows",
            "outputName": "predictions",
            "confidenceThreshold": DETECTION_CONFIDENCE_THRESHOLD,
            "sort": "confidence-desc,class-asc,xyxy-asc",
        },
        "fixture": {
            "revision": FIXTURE_REVISION,
            # Keep the package portable: the fixture is identified by its
            # content digest, not by an absolute path on the exporter host.
            "path": "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm",
            "sha256": sha256_file(fixture),
        },
        "oracle": oracle,
        "weights": {
            "path": str(weights_path.relative_to(output_dir)),
            "digest": "sha256:" + sha256_file(weights_path),
            "bytes": weights_path.stat().st_size,
        },
        "safeCuts": safe_cuts,
        "catalogue": catalogue,
        "providerIndependent": True,
        "deploymentDependencies": ["onnx", "onnxruntime"],
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=Path("yolo26n.pt"))
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--input-size", type=int, default=640)
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--signing-key", type=Path)
    parser.add_argument("--registry", type=Path)
    args = parser.parse_args(argv)
    manifest = build_package(
        args.model, args.out_dir, input_size=args.input_size, opset=args.opset,
        signing_key=args.signing_key, registry_path=args.registry)
    print("SPEC180_YOLO_EXPORT", json.dumps({
        "manifest": str(args.out_dir / "manifest.json"),
        "modelDigest": manifest["source"]["checkpointSha256"],
        "graphDigest": manifest["graph"]["graphDigest"],
        "weightsDigest": manifest["weights"]["digest"],
        "signed": "signature" in manifest["catalogue"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
