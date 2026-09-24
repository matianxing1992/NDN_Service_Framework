#!/usr/bin/env python3
"""Validate and collect provider-owned Spec191 tracking result products."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Callable, Iterable, Mapping


class ResultError(ValueError):
    """Raised when a result manifest or fetched product is not admissible."""


@dataclass(frozen=True)
class FetchedProduct:
    name: str
    payload: bytes
    signer: str
    signature_verified: bool


def digest(payload: bytes) -> str:
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _required(value: Mapping[str, object], key: str) -> str:
    item = value.get(key)
    if not isinstance(item, str) or not item:
        raise ResultError(f"result manifest field {key!r} is required")
    return item


def validate_manifest(manifest: Mapping[str, object], *, run_id: str,
                      session_id: str, expected_provider: str) -> None:
    if manifest.get("schema") != "spec191-uav-result/v1":
        raise ResultError("unsupported result manifest schema")
    if _required(manifest, "runId") != run_id or _required(manifest, "sessionId") != session_id:
        raise ResultError("result belongs to another run or session")
    if _required(manifest, "providerIdentity") != expected_provider:
        raise ResultError("result provider identity mismatch")
    if manifest.get("terminal") is not True:
        raise ResultError("result is not terminal")
    inputs = manifest.get("inputs")
    products = manifest.get("products")
    if not isinstance(inputs, list) or not inputs:
        raise ResultError("result must bind at least one input")
    if not isinstance(products, list) or not products:
        raise ResultError("result must list at least one product")
    for item in inputs:
        if not isinstance(item, Mapping):
            raise ResultError("invalid input lineage entry")
        _required(item, "name")
        _required(item, "digest")
    for item in products:
        if not isinstance(item, Mapping):
            raise ResultError("invalid result product entry")
        _required(item, "name")
        product_digest = _required(item, "digest")
        if not product_digest.startswith("sha256:"):
            raise ResultError("result product digest must be sha256")


def collect_result(manifest: Mapping[str, object], *, run_id: str, session_id: str,
                   expected_provider: str,
                   fetch: Callable[[str], FetchedProduct]) -> dict[str, bytes]:
    """Fetch every declared product and return only verified bytes.

    ``fetch`` is the adapter boundary for NDNSF ``fetchSignedExactData`` or a
    segmented equivalent. It must return the signer and an explicit signature
    verification bit; a path or success flag alone is not accepted.
    """
    validate_manifest(manifest, run_id=run_id, session_id=session_id,
                      expected_provider=expected_provider)
    accepted: dict[str, bytes] = {}
    for item in manifest["products"]:  # type: ignore[index]
        name = str(item["name"])
        expected_digest = str(item["digest"])
        fetched = fetch(name)
        if fetched.name != name or fetched.signer != expected_provider:
            raise ResultError(f"product signer/name mismatch: {name}")
        if not fetched.signature_verified:
            raise ResultError(f"product signature was not verified: {name}")
        if digest(fetched.payload) != expected_digest:
            raise ResultError(f"product digest mismatch: {name}")
        accepted[name] = fetched.payload
    return accepted


def load_and_collect(path: Path, **kwargs: object) -> dict[str, bytes]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, Mapping):
        raise ResultError("result manifest must be a JSON object")
    return collect_result(value, **kwargs)  # type: ignore[arg-type]
