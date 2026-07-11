"""Canonical versioned NDNSF-DistributedRepo service names."""

from __future__ import annotations


DEFAULT_REPO_SERVICE_ROOT = "/NDNSF/DistributedRepo"


_PUBLIC_OPERATION_GROUPS = {
    "INSERT": {
        "STORE", "INSERT", "STORE_PACKETS", "STORE_PACKET",
        "STORE_PACKET_BATCH", "STORE_PACKET_PULL", "STORE_MANIFEST",
        "RESERVE_CAPACITY", "RELEASE_CAPACITY", "FINALIZE_WRITE",
        "COMMIT_PACKET_SET",
    },
    "MANIFEST": {"MANIFEST"},
    "FETCH": {"FETCH", "FETCH_PREPARE", "FETCH_PACKET_PREPARE"},
    "INVENTORY": {"INVENTORY"},
    "STATUS": {"STATUS", "CAPABILITY", "CACHE_STATUS"},
    "DELETE": {"DELETE"},
    "CATALOG_QUERY": {
        "CATALOG_STATUS", "CATALOG_SNAPSHOT", "CATALOG_LOOKUP",
        "CATALOG_QUERY",
    },
}

_INTERNAL_OPERATION_GROUPS = {
    "RESERVE_CAPACITY": {"PEER_RESERVE_CAPACITY"},
    "RELEASE_CAPACITY": {"PEER_RELEASE_CAPACITY"},
    "REPLICA_COMMIT": {"PEER_REPLICA_COMMIT"},
    "CATALOG_MERGE": {"CATALOG_MERGE", "CATALOG_MERGE_PULL"},
    "CATALOG_DIGEST": {
        "CATALOG_DELTA", "CATALOG_BUCKET_DIGEST",
        "CATALOG_BUCKET_ENTRIES",
    },
    "REPAIR": {
        "CATALOG_REPAIR", "REPAIR_SCAN", "REPAIR_CLAIM",
        "REPAIR_COMPLETE", "REPAIR_FAIL", "SCRUB",
    },
}

_PEER_OPERATION_ALIASES = {
    "PEER_RESERVE_CAPACITY": "RESERVE_CAPACITY",
    "PEER_RELEASE_CAPACITY": "RELEASE_CAPACITY",
    "PEER_REPLICA_COMMIT": "FINALIZE_WRITE",
}


def _normalize_root(root: str) -> str:
    value = str(root).strip().rstrip("/")
    if not value:
        raise ValueError("repo service root must not be empty")
    for marker in ("/Object/v1/", "/Internal/v1/"):
        if marker in value:
            value = value.split(marker, 1)[0]
    return value


def repo_service_for_operation(
    operation: str,
    root: str = DEFAULT_REPO_SERVICE_ROOT,
) -> str:
    """Return the public or peer-only versioned service for an operation."""

    normalized = str(operation).strip().upper()
    base = _normalize_root(root)
    for service, operations in _PUBLIC_OPERATION_GROUPS.items():
        if normalized in operations:
            return f"{base}/Object/v1/{service}"
    for service, operations in _INTERNAL_OPERATION_GROUPS.items():
        if normalized in operations:
            return f"{base}/Internal/v1/{service}"
    raise ValueError(f"unsupported repo operation {normalized or '<empty>'}")


def canonical_repo_operation(operation: str) -> str:
    normalized = str(operation).strip().upper()
    return _PEER_OPERATION_ALIASES.get(normalized, normalized)


def repo_versioned_services(
    root: str = DEFAULT_REPO_SERVICE_ROOT,
) -> tuple[str, ...]:
    """Return every service a Repo node must register."""

    base = _normalize_root(root)
    public = [f"{base}/Object/v1/{name}" for name in _PUBLIC_OPERATION_GROUPS]
    internal = [f"{base}/Internal/v1/{name}" for name in _INTERNAL_OPERATION_GROUPS]
    return tuple(public + internal)


def is_internal_repo_service(service_name: str) -> bool:
    return "/Internal/v1/" in str(service_name)


__all__ = [
    "DEFAULT_REPO_SERVICE_ROOT",
    "canonical_repo_operation",
    "is_internal_repo_service",
    "repo_service_for_operation",
    "repo_versioned_services",
]
