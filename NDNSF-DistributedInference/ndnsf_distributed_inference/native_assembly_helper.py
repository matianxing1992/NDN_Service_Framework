"""Native Provider bridge for the certified Spec175 ONNX assembler.

The native C++ Provider owns the request, assignment, signing identity, and
cache.  This small process helper owns only the model-format operation: it
reconstructs the exact Python ``RoleAssemblySpec`` and
``CertifiedOnnxAssemblyRecipe`` objects and calls the existing certified
assembler.  It never accepts a filesystem path from the requester.

The input and output files are private, content-addressed staging files made
by the native Provider.  A canonical graph may be accompanied by the sibling
``model.onnx.data`` initializer object; both are staged by the Provider before
this helper runs.  The output model is subsequently signed and activated by
that Provider; this helper is deliberately not an alternate protocol or
recipe format.
"""

from __future__ import annotations

import argparse
from dataclasses import fields
import hashlib
import json
from pathlib import Path
import os
import tempfile

from .adapters.onnx.executor import (
    CertifiedOnnxAssemblyRecipe,
    assemble_certified_onnx_model,
)
from .sdk.placement import RoleAssemblySpec


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _canonical(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False).encode("utf-8")


def _load_input(path: Path) -> dict:
    if not path.is_file() or path.is_symlink():
        raise ValueError("native assembly input must be a regular file")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("native assembly input must be an object")
    return value


def _normalize_payload(payload: dict, target_type, aliases: dict[str, str]) -> dict:
    """Accept the C++ wire spelling and the Python dataclass spelling.

    ``NativeCanonicalOnnxAssembler`` deliberately serializes a stable JSON
    request with snake-case field names.  ``RoleAssemblySpec.to_dict`` uses
    camel-case names for the Python placement API, however.  The helper is a
    deployment boundary and must accept the former without silently dropping
    fields, while retaining the latter for direct Python callers.
    """
    allowed = {item.name for item in fields(target_type)}
    normalized: dict[str, object] = {}
    for key, value in payload.items():
        canonical = aliases.get(str(key), str(key))
        if canonical not in allowed:
            raise ValueError(f"native assembly input contains unknown field {key}")
        if canonical in normalized and normalized[canonical] != value:
            raise ValueError(f"native assembly input aliases conflict for {canonical}")
        normalized[canonical] = value
    return normalized


_ROLE_ALIASES = {
    "adapterId": "adapter_id",
    "adapterVersion": "adapter_version",
    "artifactDigest": "artifact_digest",
    "deviceSet": "device_set",
    "requiredDeviceMemoryMb": "required_device_memory_mb",
    "roleKind": "role_kind",
    "modelManifestDigest": "model_manifest_digest",
    "artifactProfileDigest": "artifact_profile_digest",
    "graphDigest": "graph_digest",
    "canonicalInitializerDigest": "canonical_initializer_digest",
    "adapterDescriptorDigest": "adapter_descriptor_digest",
    "assemblerDescriptorDigest": "assembler_descriptor_digest",
    "backendAbi": "backend_abi",
    "nodeIndices": "node_indices",
    "expectedInputs": "expected_inputs",
    "expectedOutputs": "expected_outputs",
    "layerBegin": "layer_begin",
    "layerEnd": "layer_end",
    "recipeDigest": "recipe_digest",
    "resourceEnvelope": "resource_envelope",
    "protectionEpoch": "protection_epoch",
}

_RECIPE_ALIASES = {
    "modelManifestDigest": "model_manifest_digest",
    "artifactProfileDigest": "artifact_profile_digest",
    "graphDigest": "graph_digest",
    "canonicalInitializerDigest": "canonical_initializer_digest",
    "adapterDescriptorDigest": "adapter_descriptor_digest",
    "assemblerDescriptorDigest": "assembler_descriptor_digest",
    "backendAbi": "backend_abi",
    "roleKind": "role_kind",
    "layerBegin": "layer_begin",
    "layerEnd": "layer_end",
    "nodeIndices": "node_indices",
    "inputNames": "input_names",
    "outputNames": "output_names",
    "expectedInputs": "expected_inputs",
    "expectedOutputs": "expected_outputs",
    "maxSourceBytes": "max_source_bytes",
    "maxAssembledBytes": "max_assembled_bytes",
    "maxNodes": "max_nodes",
}


def _normalize_resource_envelope(value):
    if not isinstance(value, dict):
        return value
    aliases = {
        "maxSourceBytes": "maxSourceBytes",
        "maxAssembledBytes": "maxAssembledBytes",
        "maxNodes": "maxNodes",
    }
    normalized = {}
    for key, item in value.items():
        canonical = aliases.get(str(key), str(key))
        if canonical not in aliases.values():
            raise ValueError(f"native assembly resource envelope has unknown field {key}")
        normalized[canonical] = item
    return normalized


def _run(request_path: Path, output_dir: Path) -> dict:
    request = _load_input(request_path)
    source_candidate = Path(str(request.get("canonical_model_path", "")))
    if source_candidate.is_symlink() or not source_candidate.is_file():
        raise ValueError("canonical ONNX source is not a regular file")
    source_path = source_candidate.resolve()
    source = source_path.read_bytes()
    initializer_source = None
    initializer_path_value = request.get("canonical_initializer_path", "")
    if initializer_path_value:
        initializer_candidate = Path(str(initializer_path_value))
        if (initializer_candidate.is_symlink()
                or not initializer_candidate.is_file()):
            raise ValueError(
                "canonical ONNX initializer must be a sibling model.onnx.data file")
        initializer_path = initializer_candidate.resolve()
        if (initializer_path.name != "model.onnx.data"
                or initializer_path.parent != source_path.parent):
            raise ValueError(
                "canonical ONNX initializer must be a sibling model.onnx.data file")
        initializer_source = initializer_path.read_bytes()
    role_payload = request.get("role_spec")
    recipe_payload = request.get("recipe")
    if not isinstance(role_payload, dict) or not isinstance(recipe_payload, dict):
        raise ValueError("native assembly input is missing role_spec or recipe")

    role_payload = _normalize_payload(role_payload, RoleAssemblySpec, _ROLE_ALIASES)
    role_payload["resource_envelope"] = _normalize_resource_envelope(
        role_payload.get("resource_envelope", {}))
    recipe_payload = _normalize_payload(
        recipe_payload, CertifiedOnnxAssemblyRecipe, _RECIPE_ALIASES)
    role = RoleAssemblySpec(**role_payload)
    recipe = CertifiedOnnxAssemblyRecipe(**recipe_payload)
    assembled = assemble_certified_onnx_model(
        source, canonical_initializer=initializer_source,
        role_spec=role, recipe=recipe)

    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    model_path = output_dir / "model.onnx"
    manifest_path = output_dir / "manifest.json"
    if model_path.exists() or manifest_path.exists():
        raise ValueError("native assembly output directory is not empty")

    manifest = {
        "schema": "ndnsf-di-assembled-onnx-v1",
        "modelName": str(request.get("model_name", "")),
        "modelDigest": str(request.get("model_digest", "")),
        "modelManifestDigest": recipe.model_manifest_digest,
        "artifactProfileDigest": recipe.artifact_profile_digest,
        "graphDigest": recipe.graph_digest,
        "role": role.role,
        "roleKind": role.role_kind,
        "rank": role.rank,
        "layerBegin": role.layer_begin,
        "layerEnd": role.layer_end,
        "recipeDigest": recipe.digest,
        "adapterDescriptorDigest": recipe.adapter_descriptor_digest,
        "assemblerDescriptorDigest": recipe.assembler_descriptor_digest,
        "backendAbi": recipe.backend_abi,
        "precision": recipe.precision,
        "quantization": recipe.quantization,
        "layout": recipe.layout,
        "padding": recipe.padding,
        "inputNames": list(assembled.input_names),
        "outputNames": list(assembled.output_names),
        "nodeCount": assembled.node_count,
        "entryDigests": {"model.onnx": assembled.model_digest},
        "entryLengths": {"model.onnx": len(assembled.model_bytes)},
        "onnxChecker": "PASS",
        "onnxRuntimeLoad": "PENDING_NATIVE_PROVIDER",
        "signer": str(request.get("provider", "")),
        "layoutMode": "INLINE_ONNX",
    }
    manifest_bytes = _canonical(manifest)
    model_tmp = model_path.with_name("model.onnx.tmp")
    manifest_tmp = manifest_path.with_name("manifest.json.tmp")
    try:
        model_tmp.write_bytes(assembled.model_bytes)
        manifest_tmp.write_bytes(manifest_bytes)
        os.replace(model_tmp, model_path)
        os.replace(manifest_tmp, manifest_path)
    except Exception:
        for item in (model_tmp, manifest_tmp, model_path, manifest_path):
            try:
                item.unlink()
            except FileNotFoundError:
                pass
        raise

    return {
        "schema": "ndnsf-di-native-assembly-result-v1",
        "model_path": str(model_path),
        "manifest_path": str(manifest_path),
        "manifest_digest": _digest(manifest_bytes),
        "model_digest": assembled.model_digest,
        "model_bytes": len(assembled.model_bytes),
        "node_count": assembled.node_count,
        "input_names": list(assembled.input_names),
        "output_names": list(assembled.output_names),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    result = _run(args.input, args.output_dir)
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
