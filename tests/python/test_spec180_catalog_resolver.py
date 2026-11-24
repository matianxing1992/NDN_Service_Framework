from __future__ import annotations

from dataclasses import replace
import json

import pytest

from ndnsf_distributed_inference.app_sdk.contracts import (
    PreSplitCatalogSnapshot, encode_runtime_catalog_snapshot,
)
from ndnsf_distributed_inference.sdk.placement import canonical_digest


def _snapshot() -> PreSplitCatalogSnapshot:
    digest = "sha256:" + "a" * 64
    return PreSplitCatalogSnapshot(
        alias="yolo-atomic",
        manifest_digest=digest,
        model_content_digest=digest,
        semantics_digest=digest,
        graph_digest=digest,
        candidate_digest=digest,
        backend="onnxruntime-cuda",
        precision="fp32",
        artifact_data_names={"FullModel": ("/repo/segments/a",)},
        status="ACTIVE",
        created_at_ms=1,
    )


def test_runtime_catalog_encoder_is_canonical_and_candidate_bound():
    name = "/example/controller/NDNSF/DI/catalogue/active"
    snapshot = _snapshot()
    payload = encode_runtime_catalog_snapshot(
        name, [snapshot], required_candidate_digests=(snapshot.candidate_digest,))
    envelope = json.loads(payload.decode("utf-8"))
    assert envelope["schema"] == "ndnsf-di-presplit-catalog-snapshot-v1"
    assert envelope["recordName"] == name
    assert envelope["snapshotDigest"] == canonical_digest(envelope["snapshots"])
    assert envelope["snapshots"][0]["artifactDataNames"] == {
        "FullModel": ["/repo/segments/a"]
    }
    assert payload == encode_runtime_catalog_snapshot(name, [snapshot])

    with pytest.raises(ValueError, match="coverage is incomplete"):
        encode_runtime_catalog_snapshot(name, [snapshot],
                                        required_candidate_digests=(
                                            "sha256:" + "b" * 64,))

    duplicate_candidate = replace(
        snapshot,
        alias="yolo-atomic-copy",
        manifest_digest="sha256:" + "b" * 64,
    )
    with pytest.raises(ValueError, match="duplicate snapshots"):
        encode_runtime_catalog_snapshot(name, [snapshot, duplicate_candidate])
