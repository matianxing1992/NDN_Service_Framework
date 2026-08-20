"""Qwen adapter helpers for Spec 170 canonical layer identities."""

from __future__ import annotations

from typing import Any, Mapping

from ...app_sdk.canonical_artifacts import (
    CanonicalArtifactProfile, CanonicalLayerCatalog, CanonicalLayerManifest,
    ModelIdentity,
)


def canonical_layer_manifest(*, model_name: str, model_digest: str, profile: str,
                             graph_digest: str, role_kind: str, layer_begin: int,
                             layer_end: int, rank: int, recipe_digest: str,
                             payload: bytes, publisher: str,
                             tensor_index: tuple[Mapping[str, Any], ...] = (),
                             graph_nodes: tuple[str, ...] = (),
                             input_contracts: tuple[Mapping[str, Any], ...] = (),
                             output_contracts: tuple[Mapping[str, Any], ...] = (),
                             model_identity: ModelIdentity | None = None,
                             artifact_profile: CanonicalArtifactProfile | None = None,
                             origin_attestation: str = "",
                             transformation_attestation: str = "",
                             ) -> CanonicalLayerManifest:
    object_digest = "sha256:" + __import__("hashlib").sha256(payload).hexdigest()
    if model_identity is not None:
        if model_digest != model_identity.digest or graph_digest != model_identity.graph_digest:
            raise ValueError("Qwen layer identity does not match ModelIdentity")
    if artifact_profile is not None:
        if profile not in {artifact_profile.digest, ""}:
            raise ValueError("Qwen layer profile does not match artifact profile")
        profile = artifact_profile.digest
    if not tensor_index:
        tensor_index = ({
            "tensorName": f"opaque:{role_kind}:{layer_begin}-{layer_end}",
            "dtype": "uint8",
            "shape": [len(payload)],
            "byteOrder": "na",
            "offset": 0,
            "length": len(payload),
            "chunkDigest": object_digest,
        },)
    graph_nodes = graph_nodes or tuple(
        f"layer:{index}" for index in range(layer_begin, layer_end))
    input_contracts = input_contracts or ({
        "name": f"input:{layer_begin}", "dtype": "opaque", "shape": [],
    },)
    output_contracts = output_contracts or ({
        "name": f"output:{layer_end}", "dtype": "opaque", "shape": [],
    },)
    return CanonicalLayerManifest(
        model_name=model_name, model_digest=model_digest, profile=profile,
        graph_digest=graph_digest, role_kind=role_kind,
        layer_begin=layer_begin, layer_end=layer_end, rank=rank,
        recipe_digest=recipe_digest, object_digest=object_digest,
        object_bytes=len(payload), publisher=publisher,
        origin_attestation=(origin_attestation
                            or "qwen-origin-v1:" + model_digest),
        transformation_attestation=(transformation_attestation
                                    or "qwen-layerizer-v1:" + recipe_digest),
        tensor_index=tensor_index, graph_nodes=graph_nodes,
        input_contracts=input_contracts, output_contracts=output_contracts)


__all__ = [
    "CanonicalArtifactProfile", "CanonicalLayerCatalog",
    "CanonicalLayerManifest", "ModelIdentity", "canonical_layer_manifest",
]
