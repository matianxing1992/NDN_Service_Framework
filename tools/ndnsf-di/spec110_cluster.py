#!/usr/bin/env python3
"""Live iTiger cluster snapshot contract with per-fact expiry."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timedelta, timezone
import hashlib
import json
import re
from typing import Any, Mapping


MUTABLE_FACTS = ("partition", "gres", "quota", "versions", "addresses")
DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
SNAPSHOT_FIELDS = {
    "schemaVersion", "snapshotId", "observedAt", "expiresAt",
    "factObservedAt", "factExpiresAt", "account", "qos", "partition",
    "gres", "nodes", "apptainerVersions", "driverCuda", "storage", "addresses",
}


class ClusterSnapshotError(ValueError):
    """Stable fail-closed cluster discovery error."""


def _fail(code: str, detail: str = "") -> None:
    raise ClusterSnapshotError(code + (f":{detail}" if detail else ""))


def _canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def _digest(value: object) -> str:
    return "sha256:" + hashlib.sha256(_canonical_json(value)).hexdigest()


def _timestamp(value: object, field: str) -> datetime:
    if not isinstance(value, str):
        _fail("CLUSTER_TIMESTAMP_INVALID", field)
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        _fail("CLUSTER_TIMESTAMP_INVALID", field)
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        _fail("CLUSTER_TIMESTAMP_INVALID", field)
    return parsed.astimezone(timezone.utc)


def _format_timestamp(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def _nonempty_string(value: object, code: str) -> str:
    if not isinstance(value, str) or not value.strip():
        _fail(code)
    return value


def _validate_values(value: Mapping[str, object]) -> None:
    required = {
        "account", "qos", "partition", "gres", "nodes", "apptainerVersions",
        "driverCuda", "storage", "addresses",
    }
    if set(value) != required:
        _fail("CLUSTER_VALUE_FIELDS_INVALID")
    for field in ("account", "qos", "partition"):
        _nonempty_string(value.get(field), "CLUSTER_VALUE_INVALID:" + field)
    gres = value.get("gres")
    if not isinstance(gres, Mapping) or not isinstance(gres.get("nodes"), list) or not gres["nodes"]:
        _fail("CLUSTER_GRES_INVALID")
    nodes = value.get("nodes")
    if not isinstance(nodes, list) or not nodes or len(nodes) != len(set(nodes)):
        _fail("CLUSTER_NODES_INVALID")
    versions = value.get("apptainerVersions")
    if not isinstance(versions, Mapping):
        _fail("CLUSTER_VERSION_INVALID")
    for location in ("login", "compute"):
        _nonempty_string(versions.get(location), "CLUSTER_VERSION_INVALID:" + location)
    driver = value.get("driverCuda")
    if not isinstance(driver, Mapping):
        _fail("CLUSTER_DRIVER_CUDA_INVALID")
    for field in ("driver", "cuda", "observedOn"):
        _nonempty_string(driver.get(field), "CLUSTER_DRIVER_CUDA_INVALID:" + field)
    storage = value.get("storage")
    if not isinstance(storage, Mapping):
        _fail("CLUSTER_STORAGE_INVALID")
    root = _nonempty_string(storage.get("projectRoot"), "CLUSTER_PROJECT_ROOT_INVALID")
    if not root.startswith("/project/") or not root.endswith("/ndnsf-di"):
        _fail("CLUSTER_PROJECT_ROOT_INVALID")
    quota = storage.get("quota")
    if not isinstance(quota, Mapping):
        _fail("CLUSTER_QUOTA_SIGNAL_INVALID")
    command = _nonempty_string(quota.get("command"), "CLUSTER_QUOTA_SIGNAL_INVALID")
    if command.strip().split()[0] == "df":
        _fail("CLUSTER_QUOTA_SIGNAL_INVALID", "shared-df-is-not-quota")
    _nonempty_string(quota.get("status"), "CLUSTER_QUOTA_SIGNAL_INVALID")
    addresses = value.get("addresses")
    if not isinstance(addresses, list) or not addresses:
        _fail("CLUSTER_ADDRESSES_INVALID")
    for item in addresses:
        if not isinstance(item, Mapping):
            _fail("CLUSTER_ADDRESS_INVALID")
        _nonempty_string(item.get("node"), "CLUSTER_ADDRESS_INVALID")
        _nonempty_string(item.get("address"), "CLUSTER_ADDRESS_INVALID")
        if item.get("scope") != "allocation":
            _fail("CLUSTER_ADDRESS_SCOPE_INVALID")


def build_cluster_snapshot(
    values: Mapping[str, object],
    *,
    observed_at: str,
    fact_ttl_seconds: Mapping[str, int] | None = None,
) -> dict[str, Any]:
    _validate_values(values)
    observed = _timestamp(observed_at, "observedAt")
    ttls = dict(fact_ttl_seconds or {name: 900 for name in MUTABLE_FACTS})
    if set(ttls) != set(MUTABLE_FACTS):
        _fail("CLUSTER_FACT_TTL_FIELDS_INVALID")
    for name, seconds in ttls.items():
        if not isinstance(seconds, int) or isinstance(seconds, bool) or seconds <= 0:
            _fail("CLUSTER_FACT_TTL_INVALID", name)
    fact_observed = {name: _format_timestamp(observed) for name in MUTABLE_FACTS}
    fact_expires = {
        name: _format_timestamp(observed + timedelta(seconds=ttls[name]))
        for name in MUTABLE_FACTS
    }
    body: dict[str, Any] = {
        "schemaVersion": "spec110-live-cluster-v1",
        "observedAt": _format_timestamp(observed),
        "expiresAt": min(fact_expires.values(), key=lambda item: _timestamp(item, "expiresAt")),
        "factObservedAt": fact_observed,
        "factExpiresAt": fact_expires,
        **deepcopy(dict(values)),
    }
    body["snapshotId"] = "spec110-cluster-" + _digest(body)[7:27]
    return body


def validate_cluster_snapshot(
    value: Mapping[str, object], *, now: datetime | None = None
) -> dict[str, Any]:
    if not isinstance(value, Mapping) or set(value) != SNAPSHOT_FIELDS:
        _fail("CLUSTER_SNAPSHOT_FIELDS_INVALID")
    if value.get("schemaVersion") != "spec110-live-cluster-v1":
        _fail("CLUSTER_SNAPSHOT_SCHEMA_INVALID")
    core = {
        key: value[key]
        for key in (
            "account", "qos", "partition", "gres", "nodes", "apptainerVersions",
            "driverCuda", "storage", "addresses",
        )
    }
    _validate_values(core)
    observed_map = value.get("factObservedAt")
    expires_map = value.get("factExpiresAt")
    if (
        not isinstance(observed_map, Mapping) or set(observed_map) != set(MUTABLE_FACTS)
        or not isinstance(expires_map, Mapping) or set(expires_map) != set(MUTABLE_FACTS)
    ):
        _fail("CLUSTER_FACT_EXPIRY_FIELDS_INVALID")
    current = now or datetime.now(timezone.utc)
    if current.tzinfo is None or current.utcoffset() is None:
        _fail("CLUSTER_NOW_INVALID")
    current = current.astimezone(timezone.utc)
    snapshot_observed = _timestamp(value.get("observedAt"), "observedAt")
    if snapshot_observed > current:
        _fail("CLUSTER_SNAPSHOT_FROM_FUTURE")
    for fact in MUTABLE_FACTS:
        fact_observed = _timestamp(observed_map[fact], "factObservedAt:" + fact)
        fact_expires = _timestamp(expires_map[fact], "factExpiresAt:" + fact)
        if fact_observed > current or fact_expires <= fact_observed:
            _fail("CLUSTER_FACT_TIME_INVALID", fact)
        if current >= fact_expires:
            _fail("CLUSTER_FACT_STALE", fact)
    _timestamp(value.get("expiresAt"), "expiresAt")
    body = dict(value)
    actual_id = body.pop("snapshotId")
    expected_id = "spec110-cluster-" + _digest(body)[7:27]
    if actual_id != expected_id:
        _fail("CLUSTER_SNAPSHOT_DIGEST_MISMATCH")
    return deepcopy(dict(value))


__all__ = [
    "ClusterSnapshotError", "MUTABLE_FACTS", "build_cluster_snapshot",
    "validate_cluster_snapshot",
]
