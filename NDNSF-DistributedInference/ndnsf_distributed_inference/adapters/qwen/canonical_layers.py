"""Qwen adapter helpers for Spec 170 canonical layer identities."""

from __future__ import annotations

from typing import Any, Mapping

from ...app_sdk.canonical_artifacts import (
    CanonicalLayerCatalog, CanonicalLayerManifest,
)


def canonical_layer_manifest(*, model_name: str, model_digest: str, profile: str,
                             graph_digest: str, role_kind: str, layer_begin: int,
                             layer_end: int, rank: int, recipe_digest: str,
                             payload: bytes, publisher: str,
                             tensor_index: tuple[Mapping[str, Any], ...] = ()) -> CanonicalLayerManifest:
    object_digest = "sha256:" + __import__("hashlib").sha256(payload).hexdigest()
    return CanonicalLayerManifest(
        model_name=model_name, model_digest=model_digest, profile=profile,
        graph_digest=graph_digest, role_kind=role_kind,
        layer_begin=layer_begin, layer_end=layer_end, rank=rank,
        recipe_digest=recipe_digest, object_digest=object_digest,
        object_bytes=len(payload), publisher=publisher,
        origin_attestation="qwen-origin:" + model_digest,
        transformation_attestation="qwen-layerizer:" + recipe_digest,
        tensor_index=tensor_index)


__all__ = ["CanonicalLayerCatalog", "CanonicalLayerManifest", "canonical_layer_manifest"]
