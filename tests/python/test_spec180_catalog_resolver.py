from __future__ import annotations

from dataclasses import replace
import hashlib
import json

import pytest

from ndnsf_distributed_inference.app_sdk.contracts import (
    PreSplitCatalogSnapshot,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    NetworkCatalogSnapshotResolver, encode_runtime_catalog_snapshot,
    canonical_digest,
)


class _Result:
    def __init__(self, *, success=True, data_name="", payload=b"", error=""):
        self.success = success
        self.data_name = data_name
        self.payload = payload
        self.error = error


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


def _payload(name: str, snapshots: list[PreSplitCatalogSnapshot]) -> bytes:
    values = [item.to_dict() for item in sorted(
        snapshots, key=lambda item: (item.alias, item.manifest_digest))]
    return json.dumps({
        "schema": NetworkCatalogSnapshotResolver.SCHEMA,
        "recordName": name,
        "snapshotDigest": canonical_digest(values),
        "snapshots": values,
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")


def test_network_catalog_resolver_fetches_signed_exact_record():
    name = "/catalogue/spec180/current"
    signer = "/authority/catalogue"
    calls = []
    snapshot = _snapshot()

    def fetch(record_name, expected_signer, *, timeout_ms):
        calls.append((record_name, expected_signer, timeout_ms))
        return _Result(data_name=name, payload=_payload(name, [snapshot]))

    resolved = NetworkCatalogSnapshotResolver(
        fetch, data_name=name, expected_signer=signer, timeout_ms=1234)()
    assert resolved == (snapshot,)
    assert calls == [(name, signer, 1234)]


def test_runtime_catalog_encoder_is_canonical_and_candidate_bound():
    name = "/example/controller/NDNSF/DI/catalogue/active"
    snapshot = _snapshot()
    payload = encode_runtime_catalog_snapshot(
        name, [snapshot], required_candidate_digests=(snapshot.candidate_digest,))
    envelope = json.loads(payload.decode("utf-8"))
    assert envelope["schema"] == NetworkCatalogSnapshotResolver.SCHEMA
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


def test_network_catalog_resolver_rejects_digest_or_duplicate_mutation():
    name = "/catalogue/spec180/current"
    signer = "/authority/catalogue"
    snapshot = _snapshot()

    def fetch(*_args, **_kwargs):
        envelope = json.loads(_payload(name, [snapshot]))
        envelope["snapshotDigest"] = "sha256:" + "0" * 64
        return _Result(data_name=name, payload=json.dumps(envelope).encode())

    with pytest.raises(ValueError, match="digest mismatch"):
        NetworkCatalogSnapshotResolver(
            fetch, data_name=name, expected_signer=signer)()

    duplicate_payload = _payload(name, [snapshot, snapshot])

    def fetch_duplicate(*_args, **_kwargs):
        return _Result(data_name=name, payload=duplicate_payload)

    with pytest.raises(ValueError, match="duplicates"):
        NetworkCatalogSnapshotResolver(
            fetch_duplicate, data_name=name, expected_signer=signer)()

    same_candidate = replace(
        snapshot,
        alias="yolo-atomic-copy",
        manifest_digest="sha256:" + "b" * 64,
    )
    duplicate_candidate_payload = _payload(name, [snapshot, same_candidate])

    def fetch_duplicate_candidate(*_args, **_kwargs):
        return _Result(data_name=name, payload=duplicate_candidate_payload)

    with pytest.raises(ValueError, match="duplicates"):
        NetworkCatalogSnapshotResolver(
            fetch_duplicate_candidate,
            data_name=name,
            expected_signer=signer,
        )()

    retired = replace(snapshot, alias="retired", status="RETIRED")
    retired_payload = _payload(name, [retired])

    def fetch_retired(*_args, **_kwargs):
        return _Result(data_name=name, payload=retired_payload)

    with pytest.raises(ValueError, match="ACTIVE snapshots"):
        NetworkCatalogSnapshotResolver(
            fetch_retired,
            data_name=name,
            expected_signer=signer,
        )()


def test_network_catalog_resolver_rejects_failed_or_wrong_name_fetch():
    with pytest.raises(LookupError, match="unavailable"):
        NetworkCatalogSnapshotResolver(
            lambda *_args, **_kwargs: _Result(success=False, error="timeout"),
            data_name="/catalogue/current",
            expected_signer="/authority/catalogue",
        )()

    with pytest.raises(ValueError, match="exact-name"):
        NetworkCatalogSnapshotResolver(
            lambda *_args, **_kwargs: _Result(data_name="/catalogue/other"),
            data_name="/catalogue/current",
            expected_signer="/authority/catalogue",
        )()
