"""Python API for NDNSF-DistributedRepo RepoClient.

This package is intentionally thin. The pybind11 extension exposes the C++
manifest/protocol/placement helpers, and the Python ``RepoClient`` class adapts
them to the installed NDNSF Python ``ServiceUser`` API.
"""

from __future__ import annotations

import json
from typing import Callable, Iterable, Optional

from ndnsf import AckCandidate, ServiceUser

from ._py_repoclient import (
    PlacementPolicy,
    RepoObjectManifest,
    StorageCapability,
    decode_store_request,
    encode_inventory,
    encode_store_request,
    make_manifest,
    make_repo_service_name,
    parse_manifest_json,
    select_replicas,
    sha256_hex,
)


def manifest_to_dict(manifest: RepoObjectManifest) -> dict:
    return json.loads(manifest.to_json())


def capability_from_ack(candidate: AckCandidate) -> Optional[StorageCapability]:
    fields: dict[str, str] = {}
    for item in bytes(candidate.payload).decode(errors="replace").split(";"):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key.strip()] = value.strip()
    repo_node = fields.get("repoNode") or candidate.provider_name
    try:
        capability = StorageCapability()
        capability.repo_node = repo_node
        capability.free_bytes = int(fields.get("freeBytes", "0"))
        capability.used_bytes = int(fields.get("usedBytes", "0"))
        capability.recent_load = float(fields.get("recentLoad", fields.get("load", "0")))
        capability.availability_score = float(
            fields.get("availabilityScore", fields.get("availability", "1"))
        )
        capability.failure_domain = fields.get("failureDomain", "")
        storage_classes = fields.get("storageClasses", "")
        if storage_classes:
            capability.storage_classes = [
                value for value in storage_classes.split(",") if value
            ]
        return capability
    except ValueError:
        return None


class RepoClient:
    """Small synchronous repo client built on NDNSF Python ``ServiceUser``.

    The repo service is generic: all repo nodes share one service name, and the
    operation is encoded in the request payload.
    """

    def __init__(
        self,
        user: ServiceUser,
        repo_service_name: str = "/NDNSF/DistributedRepo",
        *,
        ack_timeout_ms: int = 1000,
        timeout_ms: int = 30000,
    ) -> None:
        self.user = user
        self.repo_service_name = repo_service_name
        self.ack_timeout_ms = ack_timeout_ms
        self.timeout_ms = timeout_ms

    @property
    def publisher_namespace(self) -> str:
        return (
            f"{self.user.user.rstrip('/')}"
            "/NDNSF-DISTRIBUTED-REPO/OBJECT"
        )

    def publisher_object_name(self, suffix: str) -> str:
        suffix = str(suffix).strip()
        if not suffix:
            raise ValueError("repo object suffix must not be empty")
        if suffix.startswith(self.publisher_namespace + "/"):
            return suffix
        return f"{self.publisher_namespace}/{suffix.strip('/')}"

    def _require_publisher_object_name(self, object_name: str) -> str:
        name = str(object_name).strip()
        if not name:
            raise ValueError("repo object name must not be empty")
        if not name.startswith(self.publisher_namespace + "/"):
            raise ValueError(
                "repo object data names must be under the publisher namespace: "
                f"{self.publisher_namespace}/..."
            )
        return name

    @staticmethod
    def make_manifest(
        *,
        object_name: str,
        object_type: str,
        payload: bytes,
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
    ) -> RepoObjectManifest:
        return make_manifest(
            object_name,
            object_type,
            bytes(payload),
            int(replication_factor),
            list(replica_nodes),
            policy_epoch,
        )

    def capability(self) -> list[StorageCapability]:
        response = self.user.request_service(
            self.repo_service_name,
            _request("CAPABILITY"),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        obj = _json_payload(response.payload)
        if "capabilities" in obj:
            return [_capability_from_json(item) for item in obj.get("capabilities", [])]
        if "repoNode" in obj:
            return [_capability_from_json(obj)]
        return []

    def store(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str = "artifact",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        selector: Optional[Callable[[list[AckCandidate]], list[str]]] = None,
    ) -> RepoObjectManifest:
        payload = bytes(payload)
        object_name = self._require_publisher_object_name(object_name)
        manifest = self.make_manifest(
            object_name=object_name,
            object_type=object_type,
            payload=payload,
            replication_factor=replication_factor,
            replica_nodes=replica_nodes,
            policy_epoch=policy_epoch,
        )
        if selector is None:
            selector = _capacity_selector(replication_factor, len(payload))
        response = self.user.request_service_select(
            self.repo_service_name,
            _request(
                "STORE",
                manifest=manifest_to_dict(manifest),
                payloadB64=_b64(payload),
            ),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=max(self.timeout_ms, 60000),
            request_strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        try:
            stored = parse_manifest_json(response.payload.decode())
            if stored.object_name:
                return stored
        except Exception:
            pass
        return manifest

    def put(
        self,
        object_name: str,
        payload: bytes,
        *,
        object_type: str = "object",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        selector: Optional[Callable[[list[AckCandidate]], list[str]]] = None,
    ) -> RepoObjectManifest:
        return self.store(
            object_name=object_name,
            payload=payload,
            object_type=object_type,
            replication_factor=replication_factor,
            replica_nodes=replica_nodes,
            policy_epoch=policy_epoch,
            selector=selector,
        )

    def fetch(self, object_name: str) -> bytes:
        response = self.user.request_service(
            self.repo_service_name,
            _request("FETCH", objectName=object_name),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=max(self.timeout_ms, 60000),
            strategy="first-responding",
        )
        if not response.status:
            raise RuntimeError(response.error)
        obj = _json_payload(response.payload)
        if "payloadB64" in obj:
            return _unb64(obj["payloadB64"])
        return bytes(response.payload)

    def get(self, object_name: str) -> bytes:
        return self.fetch(object_name)

    def fetch_object(self, manifest: RepoObjectManifest) -> bytes:
        """Fetch one logical object described by a repo manifest.

        The current remote repo service returns object payloads by object name.
        This helper gives callers the same object-level shape as the C++ API and
        verifies manifest size/hash after the fetch. If a future remote service
        exposes manifest-driven segmented fetch directly, this method remains
        the stable high-level entry point.
        """
        payload = self.fetch(manifest.object_name)
        _verify_manifest_payload(manifest, payload)
        return payload

    def get_object(self, manifest: RepoObjectManifest) -> bytes:
        return self.fetch_object(manifest)

    def manifest(self, object_name: str) -> RepoObjectManifest:
        response = self.user.request_service(
            self.repo_service_name,
            _request("MANIFEST", objectName=object_name),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            strategy="first-responding",
        )
        if not response.status:
            raise RuntimeError(response.error)
        return parse_manifest_json(response.payload.decode())

    def inventory(self) -> dict[str, RepoObjectManifest]:
        response = self.user.request_service(
            self.repo_service_name,
            _request("INVENTORY"),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        obj = _json_payload(response.payload)
        objects = obj.get("objects", obj)
        return {
            name: parse_manifest_json(json.dumps(value, sort_keys=True))
            for name, value in objects.items()
        }

    def list(self) -> dict[str, RepoObjectManifest]:
        return self.inventory()

    def delete(self, object_name: str) -> None:
        response = self.user.request_service(
            self.repo_service_name,
            _request("DELETE", objectName=object_name),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)

    def remove(self, object_name: str) -> None:
        self.delete(object_name)


def _capacity_selector(replication_factor: int, object_size: int):
    def selector(candidates: list[AckCandidate]) -> list[str]:
        capabilities = [
            capability
            for candidate in candidates
            if (capability := capability_from_ack(candidate)) is not None
        ]
        policy = PlacementPolicy()
        policy.replication_factor = int(replication_factor)
        selected = select_replicas(capabilities, policy, int(object_size))
        return [capability.repo_node for capability in selected]

    return selector


def _capability_from_json(obj: dict) -> StorageCapability:
    capability = StorageCapability()
    capability.repo_node = str(obj.get("repoNode", ""))
    capability.free_bytes = int(obj.get("freeBytes", 0))
    capability.used_bytes = int(obj.get("usedBytes", 0))
    capability.recent_load = float(obj.get("recentLoad", 0))
    capability.availability_score = float(obj.get("availabilityScore", 1))
    capability.failure_domain = str(obj.get("failureDomain", ""))
    capability.storage_classes = [str(value) for value in obj.get("storageClasses", [])]
    return capability


def _request(operation: str, **fields) -> bytes:
    return json.dumps(
        {
            "operation": operation,
            **fields,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()


def _json_payload(payload: bytes) -> dict:
    obj = json.loads(bytes(payload).decode())
    if not isinstance(obj, dict):
        raise ValueError("repo response payload must be a JSON object")
    return obj


def _b64(payload: bytes) -> str:
    import base64

    return base64.b64encode(payload).decode()


def _unb64(payload: str) -> bytes:
    import base64

    return base64.b64decode(payload.encode())


def _verify_manifest_payload(manifest: RepoObjectManifest, payload: bytes) -> None:
    if len(payload) != manifest.size:
        raise ValueError(
            f"repo object size mismatch for {manifest.object_name}: "
            f"expected {manifest.size}, got {len(payload)}"
        )
    digest = sha256_hex(bytes(payload))
    if manifest.sha256 and digest != manifest.sha256:
        raise ValueError(f"repo object sha256 mismatch for {manifest.object_name}")


__all__ = [
    "PlacementPolicy",
    "RepoClient",
    "RepoObjectManifest",
    "StorageCapability",
    "capability_from_ack",
    "decode_store_request",
    "encode_inventory",
    "encode_store_request",
    "make_manifest",
    "make_repo_service_name",
    "manifest_to_dict",
    "parse_manifest_json",
    "select_replicas",
    "sha256_hex",
]
