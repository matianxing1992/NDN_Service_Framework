"""NDNSF-DistributedRepo operational orchestration.

The C++ Repo contract remains canonical. This module owns Python NDNSF network,
persistence, placement, catalog and repair orchestration. Applications may use
the public client and reference helpers but do not own these policies.
"""

from __future__ import annotations

import base64
from collections import OrderedDict
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import nullcontext
from dataclasses import dataclass, field, replace
from enum import Enum
from functools import wraps
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import threading
import time
import uuid
from typing import Iterable, Optional

from ndnsf import (
    AckCandidate,
    AckDecision,
    DataPacket,
    DataProductReference,
    GenericProviderRuntimeHint,
    ProviderCapabilityHint,
    SegmentedObjectProducer,
    ServiceProvider,
    ServiceOperationState,
    ServiceOperationStatus,
    ServiceResponse,
    ServiceUser,
    SegmentHintRange,
    StoredDataProducer,
    decode_provider_capability_ack,
    decode_data_packet,
    encode_ack_metadata,
    encode_provider_capability_ack,
    fetch_exact_data_packet,
    fetch_segmented_data_packets,
    fetch_segmented_object,
    fetch_segmented_object_with_segment_hints,
    fetch_known_segmented_object_with_segment_hints,
    make_segmented_data_packets,
    to_plain,
)
from py_repoclient import RepoDataPlaneProducer
from py_repoclient.service_names import (
    canonical_repo_operation,
    is_internal_repo_service,
    repo_service_for_operation,
    repo_versioned_services,
)


def _pull_fetch_timeout_ms(segment_count: int) -> int:
    """Return a conservative timeout for repo-side pull of segmented objects."""
    if segment_count <= 0:
        return 60000
    return max(60000, min(600000, segment_count * 150))


def _large_data_interest_lifetime_ms() -> int:
    """InterestLifetime for repo and DI large-object fetches.

    Large-object consumers often issue Interests before the producer has
    finished publishing all segments. A longer default keeps those Interests
    pending in NFD and avoids repeated 1s re-expressions during cold
    provisioning and activation prefetch.
    """

    return max(50, int(os.environ.get("NDNSF_LARGE_DATA_INTEREST_LIFETIME_MS", "10000")))


def _boolish(value: object, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "on", "enabled"}:
        return True
    if text in {"0", "false", "no", "off", "disabled"}:
        return False
    return default


class WriteConsistency(str, Enum):
    ONE = "ONE"
    QUORUM = "QUORUM"
    ALL = "ALL"


REPO_OPERATION_STATES = frozenset({
    "RECEIVED",
    "RUNNING",
    "COMMITTED",
    "INCOMPLETE",
    "FAILED",
    "CANCELLED",
    "EXPIRED",
})

REPO_REASON_OPERATION_CONFLICT = "repo-operation-conflict"
REPO_REASON_GENERATION_CONFLICT = "repo-generation-conflict"
REPO_REASON_WRITE_INCOMPLETE = "repo-write-incomplete"
REPO_REASON_OVERLOADED = "repo-overloaded"
REPO_REASON_CAPACITY_RESERVED = "repo-capacity-reserved"
REPO_REASON_CAPACITY_REJECTED = "repo-capacity-rejected"
REPO_REASON_INTEGRITY_FAILURE = "repo-integrity-failure"
REPO_REASON_REPAIR_UNAVAILABLE = "repo-repair-unavailable"
CATALOG_MERGE_MAX_PULL_BYTES = 16 * 1024 * 1024

REPO_REJECTION_REASONS = frozenset({
    REPO_REASON_OPERATION_CONFLICT,
    REPO_REASON_GENERATION_CONFLICT,
    REPO_REASON_WRITE_INCOMPLETE,
    REPO_REASON_OVERLOADED,
    REPO_REASON_CAPACITY_RESERVED,
    REPO_REASON_CAPACITY_REJECTED,
    REPO_REASON_INTEGRITY_FAILURE,
    REPO_REASON_REPAIR_UNAVAILABLE,
})


def normalize_write_consistency(value: str | WriteConsistency) -> str:
    normalized = str(value.value if isinstance(value, WriteConsistency) else value).upper()
    if normalized not in {item.value for item in WriteConsistency}:
        raise ValueError(f"unsupported repo write consistency: {value}")
    return normalized


def required_write_acks(replication_factor: int,
                        consistency: str | WriteConsistency) -> int:
    replicas = int(replication_factor)
    if replicas < 1:
        raise ValueError("repo replication factor must be >= 1")
    normalized = normalize_write_consistency(consistency)
    if normalized == WriteConsistency.ONE.value:
        return 1
    if normalized == WriteConsistency.QUORUM.value:
        return replicas // 2 + 1
    return replicas


def normalize_repo_operation_state(value: str) -> str:
    normalized = str(value).upper()
    if normalized not in REPO_OPERATION_STATES:
        raise ValueError(f"unsupported repo operation state: {value}")
    return normalized


@dataclass(frozen=True)
class RepoWriteIntent:
    operation_id: str
    object_name: str
    generation: int
    digest: str
    replication_factor: int
    required_acks: int = 0
    consistency: str = WriteConsistency.ALL.value
    expected_generation: int = -1
    selected_replicas: tuple[str, ...] = ()
    state: str = "RECEIVED"
    created_at_ms: int = 0
    updated_at_ms: int = 0

    def __post_init__(self) -> None:
        if not self.operation_id or not self.object_name or not self.digest:
            raise ValueError("repo write intent requires operationId, objectName, and digest")
        if self.generation < 0:
            raise ValueError("repo write generation must be >= 0")
        consistency = normalize_write_consistency(self.consistency)
        object.__setattr__(self, "consistency", consistency)
        acknowledgements = self.required_acks or required_write_acks(
            self.replication_factor, consistency)
        if acknowledgements < 1 or acknowledgements > self.replication_factor:
            raise ValueError("repo required write acknowledgements must be within replication factor")
        object.__setattr__(self, "required_acks", acknowledgements)
        object.__setattr__(self, "state", normalize_repo_operation_state(self.state))

    def to_dict(self) -> dict[str, object]:
        return {
            "operationId": self.operation_id,
            "objectName": self.object_name,
            "generation": self.generation,
            "expectedGeneration": self.expected_generation,
            "digest": self.digest,
            "replicationFactor": self.replication_factor,
            "requiredWriteAcks": self.required_acks,
            "writeConsistency": self.consistency,
            "selectedReplicas": list(self.selected_replicas),
            "state": self.state,
            "createdAtMs": self.created_at_ms,
            "updatedAtMs": self.updated_at_ms,
        }

    @staticmethod
    def from_dict(obj: dict) -> "RepoWriteIntent":
        return RepoWriteIntent(
            operation_id=str(obj.get("operationId", "")),
            object_name=str(obj.get("objectName", "")),
            generation=int(obj.get("generation", 0)),
            expected_generation=int(obj.get("expectedGeneration", -1)),
            digest=str(obj.get("digest", obj.get("sha256", ""))),
            replication_factor=int(obj.get("replicationFactor", 1)),
            required_acks=int(obj.get("requiredWriteAcks", 0)),
            consistency=str(obj.get("writeConsistency", WriteConsistency.ALL.value)),
            selected_replicas=tuple(str(value) for value in obj.get("selectedReplicas", [])),
            state=str(obj.get("state", "RECEIVED")),
            created_at_ms=int(obj.get("createdAtMs", 0)),
            updated_at_ms=int(obj.get("updatedAtMs", 0)),
        )


@dataclass(frozen=True)
class RepoWriteReceipt:
    operation_id: str
    repo_node: str
    object_name: str
    generation: int
    digest: str
    persisted_bytes: int
    state: str = "COMMITTED"
    completed_at_ms: int = 0

    def __post_init__(self) -> None:
        if not self.operation_id or not self.repo_node or not self.object_name or not self.digest:
            raise ValueError("repo write receipt requires operation, repo, object, and digest")
        if self.generation < 0 or self.persisted_bytes < 0:
            raise ValueError("repo receipt generation and persisted bytes must be non-negative")
        object.__setattr__(self, "state", normalize_repo_operation_state(self.state))

    def to_dict(self) -> dict[str, object]:
        return {
            "operationId": self.operation_id,
            "repoNode": self.repo_node,
            "objectName": self.object_name,
            "generation": self.generation,
            "digest": self.digest,
            "persistedBytes": self.persisted_bytes,
            "state": self.state,
            "completedAtMs": self.completed_at_ms,
        }

    @staticmethod
    def from_dict(obj: dict) -> "RepoWriteReceipt":
        return RepoWriteReceipt(
            operation_id=str(obj.get("operationId", "")),
            repo_node=str(obj.get("repoNode", "")),
            object_name=str(obj.get("objectName", "")),
            generation=int(obj.get("generation", 0)),
            digest=str(obj.get("digest", obj.get("sha256", ""))),
            persisted_bytes=int(obj.get("persistedBytes", 0)),
            state=str(obj.get("state", "COMMITTED")),
            completed_at_ms=int(obj.get("completedAtMs", 0)),
        )


@dataclass(frozen=True)
class RepoCapacityReservation:
    reservation_id: str
    operation_id: str
    repo_node: str
    reserved_bytes: int
    state: str
    expires_at_ms: int

    def to_dict(self) -> dict[str, object]:
        return {
            "reservationId": self.reservation_id,
            "operationId": self.operation_id,
            "repoNode": self.repo_node,
            "reservedBytes": self.reserved_bytes,
            "state": self.state,
            "expiresAtMs": self.expires_at_ms,
        }


class RepoIncompleteWriteError(RuntimeError):
    def __init__(self, intent: RepoWriteIntent,
                 receipts: Iterable[RepoWriteReceipt],
                 failures: Optional[dict[str, str]] = None) -> None:
        self.intent = intent
        self.receipts = tuple(receipts)
        self.confirmed_replicas = tuple(receipt.repo_node for receipt in self.receipts)
        self.failures = dict(failures or {})
        super().__init__(
            f"{REPO_REASON_WRITE_INCOMPLETE}: operation={intent.operation_id} "
            f"confirmed={len(self.receipts)} required={intent.required_acks} "
            f"failures={self.failures}"
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "reason": REPO_REASON_WRITE_INCOMPLETE,
            "intent": self.intent.to_dict(),
            "receipts": [receipt.to_dict() for receipt in self.receipts],
            "confirmedReplicas": list(self.confirmed_replicas),
            "failures": dict(self.failures),
        }


def validate_write_receipts(
    intent: RepoWriteIntent,
    receipts: Iterable[RepoWriteReceipt],
    *,
    failures: Optional[dict[str, str]] = None,
) -> tuple[RepoWriteReceipt, ...]:
    unique: dict[str, RepoWriteReceipt] = {}
    selected = set(intent.selected_replicas)
    for receipt in receipts:
        if (receipt.operation_id != intent.operation_id or
                receipt.object_name != intent.object_name or
                receipt.generation != intent.generation or
                receipt.digest != intent.digest or
                receipt.state != "COMMITTED"):
            raise ValueError(f"{REPO_REASON_INTEGRITY_FAILURE}: write receipt tuple mismatch")
        if selected and receipt.repo_node not in selected:
            raise ValueError(f"{REPO_REASON_INTEGRITY_FAILURE}: receipt from unselected repo")
        existing = unique.get(receipt.repo_node)
        if existing is not None and existing != receipt:
            raise ValueError(f"{REPO_REASON_INTEGRITY_FAILURE}: conflicting duplicate receipt")
        unique[receipt.repo_node] = receipt
    validated = tuple(unique.values())
    if len(validated) < intent.required_acks:
        raise RepoIncompleteWriteError(intent, validated, failures)
    return validated


def _serialize_repo_storage(method):
    """Serialize writers and coordinate same-object cache transitions."""

    @wraps(method)
    def wrapped(self, *args, **kwargs):
        object_name = ""
        if args:
            first = args[0]
            object_name = str(getattr(first, "object_name", first if isinstance(first, str) else ""))
        object_name = str(kwargs.get("object_name", object_name))
        stripe = (
            self._object_lock(object_name)
            if object_name and hasattr(self, "_object_locks") else nullcontext()
        )
        with stripe:
            with self._db_lock:
                return method(self, *args, **kwargs)

    return wrapped


def _coordinate_repo_object(method):
    """Keep one object's read-through and cache admission coherent."""

    @wraps(method)
    def wrapped(self, *args, **kwargs):
        object_name = ""
        if args:
            first = args[0]
            object_name = str(
                getattr(first, "object_name", first if isinstance(first, str) else ""))
        object_name = str(kwargs.get("object_name", object_name))
        stripe = (
            self._object_lock(object_name)
            if object_name and hasattr(self, "_object_locks") else nullcontext()
        )
        with stripe:
            return method(self, *args, **kwargs)

    return wrapped


def _packet_manifest_versioned_data_name(packet_manifest: dict) -> str:
    packets = list(packet_manifest.get("packets", []))
    if not packets:
        raise ValueError("packet manifest contains no packets")
    first_name = str(packets[0].get("name", ""))
    if not first_name:
        raise ValueError("packet manifest first packet has no Data name")
    if "/seg=" not in first_name:
        return first_name
    return first_name.rsplit("/seg=", 1)[0]


def _packet_set_versioned_data_name(packets: Iterable[DataPacket]) -> str:
    packet_list = list(packets)
    if not packet_list:
        raise ValueError("packet set contains no Data packets")
    parents = {
        packet.name.rsplit("/seg=", 1)[0]
        if "/seg=" in packet.name else packet.name
        for packet in packet_list
    }
    if len(parents) != 1:
        raise ValueError("packet set mixes Data prefixes or versions")
    return next(iter(parents))


REPO_OBJECT_CLASS_DEFAULTS: dict[str, dict[str, object]] = {
    "temporary-activation": {
        "minReplicationFactor": 1,
        "maxReplicationFactor": 1,
        "ttlMs": 10 * 60 * 1000,
        "repairAllowed": False,
    },
    "model-artifact": {
        "minReplicationFactor": 2,
        "maxReplicationFactor": 3,
        "ttlMs": 0,
        "repairAllowed": True,
    },
    "uav-recording": {
        "minReplicationFactor": 2,
        "maxReplicationFactor": 3,
        "ttlMs": 7 * 24 * 60 * 60 * 1000,
        "repairAllowed": True,
    },
    "telemetry-log": {
        "minReplicationFactor": 1,
        "maxReplicationFactor": 2,
        "ttlMs": 7 * 24 * 60 * 60 * 1000,
        "repairAllowed": True,
    },
    "mission-log": {
        "minReplicationFactor": 2,
        "maxReplicationFactor": 3,
        "ttlMs": 30 * 24 * 60 * 60 * 1000,
        "repairAllowed": True,
    },
}
REPO_OBJECT_CLASS_POLICIES: dict[str, dict[str, object]] = {
    name: dict(policy)
    for name, policy in REPO_OBJECT_CLASS_DEFAULTS.items()
}


def _normalize_object_class_policy(name: str, raw_policy: dict) -> dict[str, object]:
    policy = dict(raw_policy)
    if "minReplica" in policy and "minReplicationFactor" not in policy:
        policy["minReplicationFactor"] = policy["minReplica"]
    if "minReplicas" in policy and "minReplicationFactor" not in policy:
        policy["minReplicationFactor"] = policy["minReplicas"]
    if "maxReplica" in policy and "maxReplicationFactor" not in policy:
        policy["maxReplicationFactor"] = policy["maxReplica"]
    if "maxReplicas" in policy and "maxReplicationFactor" not in policy:
        policy["maxReplicationFactor"] = policy["maxReplicas"]
    if "ttl" in policy and "ttlMs" not in policy:
        policy["ttlMs"] = policy["ttl"]
    if "repair" in policy and "repairAllowed" not in policy:
        policy["repairAllowed"] = policy["repair"]
    if "repair_allowed" in policy and "repairAllowed" not in policy:
        policy["repairAllowed"] = policy["repair_allowed"]
    if "auto_delete" in policy and "autoDelete" not in policy:
        policy["autoDelete"] = policy["auto_delete"]
    return {
        "objectClass": str(policy.get("objectClass", name)),
        "minReplicationFactor": int(policy.get("minReplicationFactor", 1) or 1),
        "maxReplicationFactor": int(policy.get("maxReplicationFactor", 0) or 0),
        "ttlMs": int(policy.get("ttlMs", 0) or 0),
        "repairAllowed": _boolish(policy.get("repairAllowed", True), True),
        "autoDelete": _boolish(policy.get("autoDelete", False), False),
        "deletePolicy": str(policy.get("deletePolicy", "")),
        "priority": int(policy.get("priority", 0) or 0),
    }


def configure_repo_object_class_policies(config: dict) -> None:
    """Install deployment-specific object class policies for this process."""

    control = config.get("repo_control_plane", {})
    if not isinstance(control, dict):
        return
    configured = control.get("object_classes", control.get("objectClasses", {}))
    if not configured:
        return
    policies = {
        name: dict(policy)
        for name, policy in REPO_OBJECT_CLASS_DEFAULTS.items()
    }
    if isinstance(configured, dict):
        items = configured.items()
    elif isinstance(configured, list):
        items = [
            (str(item.get("name", item.get("objectClass", ""))), item)
            for item in configured
            if isinstance(item, dict)
        ]
    else:
        raise ValueError("repo_control_plane.object_classes must be a mapping or list")
    for name, raw_policy in items:
        class_name = str(name).strip()
        if not class_name:
            continue
        if not isinstance(raw_policy, dict):
            raise ValueError(f"object class policy must be a mapping: {class_name}")
        base = dict(policies.get(class_name, {}))
        base.update(raw_policy)
        policies[class_name] = _normalize_object_class_policy(class_name, base)
    REPO_OBJECT_CLASS_POLICIES.clear()
    REPO_OBJECT_CLASS_POLICIES.update(policies)


def repo_object_class_policy(object_type: str, object_class: str = "") -> dict[str, object]:
    """Return default lifecycle/replication metadata for a repo object class."""

    normalized_class = object_class.strip() if object_class else ""
    normalized_type = object_type.strip().lower()
    if not normalized_class:
        if "activation" in normalized_type:
            normalized_class = "temporary-activation"
        elif "model" in normalized_type or "artifact" in normalized_type:
            normalized_class = "model-artifact"
        elif "recording" in normalized_type or "video" in normalized_type:
            normalized_class = "uav-recording"
        elif "telemetry" in normalized_type:
            normalized_class = "telemetry-log"
        elif "mission" in normalized_type:
            normalized_class = "mission-log"
        else:
            normalized_class = normalized_type or "generic"
    defaults = dict(REPO_OBJECT_CLASS_POLICIES.get(normalized_class, {}))
    defaults.setdefault("minReplicationFactor", 1)
    defaults.setdefault("maxReplicationFactor", 0)
    defaults.setdefault("ttlMs", 0)
    defaults.setdefault("repairAllowed", True)
    defaults.setdefault("autoDelete", False)
    defaults.setdefault("deletePolicy", "")
    defaults.setdefault("priority", 0)
    defaults["objectClass"] = normalized_class
    return defaults


@dataclass(frozen=True)
class RepoObjectManifest:
    object_name: str
    object_type: str
    sha256: str
    size: int
    segment_count: int = 1
    replication_factor: int = 1
    # Zero means "unspecified": default the repair floor to the requested
    # replication factor. Callers may explicitly choose a lower floor.
    min_replication_factor: int = 0
    max_replication_factor: int = 0
    replica_nodes: tuple[str, ...] = ()
    replica_data_names: tuple[str, ...] = ()
    packet_names: tuple[str, ...] = ()
    segment_locations: tuple[dict, ...] = ()
    policy_epoch: str = ""
    object_class: str = ""
    ttl_ms: int = 0
    repair_allowed: bool = True
    auto_delete: bool = False
    delete_policy: str = ""
    priority: int = 0
    metadata: dict = field(default_factory=dict)
    generation: int = 0
    parent_generation: int = -1
    write_consistency: str = WriteConsistency.ALL.value
    required_write_acks: int = 0
    confirmed_replica_nodes: tuple[str, ...] = ()
    operation_id: str = ""
    lifecycle_state: str = "COMMITTED"

    def __post_init__(self) -> None:
        if self.generation < 0:
            raise ValueError("repo manifest generation must be >= 0")
        if self.parent_generation >= self.generation and self.parent_generation >= 0:
            raise ValueError("repo parent generation must be less than generation")
        consistency = normalize_write_consistency(self.write_consistency)
        object.__setattr__(self, "write_consistency", consistency)
        acknowledgements = self.required_write_acks or required_write_acks(
            self.replication_factor, consistency)
        if acknowledgements < 1 or acknowledgements > self.replication_factor:
            raise ValueError("repo manifest required acknowledgements exceed replication factor")
        object.__setattr__(self, "required_write_acks", acknowledgements)
        lifecycle = normalize_repo_operation_state(self.lifecycle_state)
        object.__setattr__(self, "lifecycle_state", lifecycle)
        if (not self.confirmed_replica_nodes and self.replica_nodes and
                lifecycle == "COMMITTED"):
            object.__setattr__(self, "confirmed_replica_nodes", self.replica_nodes)

    def to_dict(self) -> dict:
        class_policy = repo_object_class_policy(self.object_type, self.object_class)
        object_class = str(class_policy.get("objectClass", self.object_class))
        class_min_replication_factor = int(
            class_policy.get("minReplicationFactor", 1) or 1)
        min_replication_factor = (
            max(self.min_replication_factor, class_min_replication_factor)
            if self.min_replication_factor > 0 else
            max(self.replication_factor, class_min_replication_factor)
        )
        max_replication_factor = (
            self.max_replication_factor
            if self.max_replication_factor > 0 else
            int(class_policy.get("maxReplicationFactor", 0) or 0) or
            self.replication_factor
        )
        ttl_ms = (
            self.ttl_ms
            if self.ttl_ms > 0 else
            int(class_policy.get("ttlMs", 0) or 0)
        )
        auto_delete = (
            self.auto_delete or
            _boolish(class_policy.get("autoDelete", False), False)
        )
        delete_policy = self.delete_policy or str(class_policy.get("deletePolicy", ""))
        priority = self.priority or int(class_policy.get("priority", 0) or 0)
        metadata = dict(self.metadata or {})
        query_tags = [
            str(value) for value in
            metadata.get("queryTags", metadata.get("tags", []))
            if str(value)
        ] if isinstance(metadata.get("queryTags", metadata.get("tags", [])), list) else []
        return {
            "objectName": self.object_name,
            "objectType": self.object_type,
            "objectClass": object_class,
            "sha256": self.sha256,
            "size": self.size,
            "segmentCount": self.segment_count,
            "replicationFactor": self.replication_factor,
            "minReplicationFactor": min_replication_factor,
            "maxReplicationFactor": max_replication_factor,
            "ttlMs": ttl_ms,
            "repairAllowed": bool(self.repair_allowed and class_policy.get("repairAllowed", True)),
            "autoDelete": auto_delete,
            "deletePolicy": delete_policy,
            "priority": priority,
            "replicaNodes": list(self.replica_nodes),
            "replicaDataNames": list(self.replica_data_names),
            "packetNames": list(self.packet_names),
            "segmentLocations": list(self.segment_locations),
            "policyEpoch": self.policy_epoch,
            "metadata": metadata,
            "queryTags": query_tags,
            "generation": self.generation,
            "parentGeneration": self.parent_generation,
            "writeConsistency": self.write_consistency,
            "requiredWriteAcks": self.required_write_acks,
            "confirmedReplicaNodes": list(self.confirmed_replica_nodes),
            "operationId": self.operation_id,
            "lifecycleState": self.lifecycle_state,
        }

    def to_bytes(self) -> bytes:
        return json.dumps(self.to_dict(), sort_keys=True).encode()

    @staticmethod
    def from_dict(obj: dict) -> "RepoObjectManifest":
        return RepoObjectManifest(
            object_name=str(obj["objectName"]),
            object_type=str(obj.get("objectType", "artifact")),
            sha256=str(obj["sha256"]),
            size=int(obj["size"]),
            segment_count=int(obj.get("segmentCount", 1)),
            replication_factor=int(obj.get("replicationFactor", 1)),
            min_replication_factor=int(obj.get("minReplicationFactor", 0)),
            max_replication_factor=int(obj.get(
                "maxReplicationFactor",
                obj.get("replicationFactor", 1),
            )),
            replica_nodes=tuple(str(value) for value in obj.get("replicaNodes", [])),
            replica_data_names=tuple(str(value) for value in obj.get("replicaDataNames", [])),
            packet_names=tuple(str(value) for value in obj.get("packetNames", [])),
            segment_locations=tuple(dict(value) for value in obj.get("segmentLocations", [])),
            policy_epoch=str(obj.get("policyEpoch", "")),
            object_class=str(obj.get("objectClass", "")),
            ttl_ms=int(obj.get("ttlMs", 0) or 0),
            repair_allowed=_boolish(obj.get("repairAllowed", True), True),
            auto_delete=_boolish(obj.get("autoDelete", False), False),
            delete_policy=str(obj.get("deletePolicy", "")),
            priority=int(obj.get("priority", 0) or 0),
            metadata=dict(obj.get("metadata", {}))
            if isinstance(obj.get("metadata", {}), dict) else {},
            generation=int(obj.get("generation", 0)),
            parent_generation=int(obj.get("parentGeneration", -1)),
            write_consistency=str(obj.get("writeConsistency", WriteConsistency.ALL.value)),
            required_write_acks=int(obj.get(
                "requiredWriteAcks",
                required_write_acks(
                    int(obj.get("replicationFactor", 1)),
                    str(obj.get("writeConsistency", WriteConsistency.ALL.value)),
                ),
            )),
            confirmed_replica_nodes=tuple(str(value) for value in obj.get(
                "confirmedReplicaNodes", obj.get("replicaNodes", []))),
            operation_id=str(obj.get("operationId", "")),
            lifecycle_state=str(obj.get("lifecycleState", "COMMITTED")),
        )


@dataclass(frozen=True)
class RepoRepairAction:
    """Validated catalog repair action.

    The wire/catalog shape remains a JSON object, but this class gives the
    control plane a typed schema boundary before a sidecar executes repair.
    """

    object_name: str
    object_sha256: str
    manifest_sha256: str
    source_repo: str
    target_repo: str
    min_replication_factor: int
    max_replication_factor: int
    reason: str = "under-replicated"
    action_type: str = "copy-replica"
    schema_version: int = 1

    def to_dict(self) -> dict:
        return {
            "schemaVersion": self.schema_version,
            "actionType": self.action_type,
            "objectName": self.object_name,
            "objectSha256": self.object_sha256,
            "manifestSha256": self.manifest_sha256,
            "minReplicationFactor": self.min_replication_factor,
            "maxReplicationFactor": self.max_replication_factor,
            "sourceRepo": self.source_repo,
            "targetRepo": self.target_repo,
            "reason": self.reason,
        }

    @staticmethod
    def from_dict(obj: dict, *, target_repo_node: str = "") -> "RepoRepairAction":
        if not isinstance(obj, dict):
            raise ValueError("catalog repair action must be a mapping")
        action_type = str(obj.get("actionType", "copy-replica"))
        if action_type != "copy-replica":
            raise ValueError(f"unsupported catalog repair actionType: {action_type}")
        schema_version = int(obj.get("schemaVersion", 1) or 1)
        if schema_version != 1:
            raise ValueError(f"unsupported catalog repair schemaVersion: {schema_version}")
        object_name = str(obj.get("objectName", ""))
        object_sha256 = str(obj.get("objectSha256", ""))
        manifest_sha256 = str(obj.get("manifestSha256", ""))
        source_repo = str(obj.get("sourceRepo", ""))
        target_repo = str(obj.get("targetRepo", target_repo_node))
        if not object_name:
            raise ValueError("catalog repair action missing objectName")
        if not object_sha256:
            raise ValueError("catalog repair action missing objectSha256")
        if not manifest_sha256:
            raise ValueError("catalog repair action missing manifestSha256")
        if not source_repo:
            raise ValueError("catalog repair action missing sourceRepo")
        if not target_repo:
            raise ValueError("catalog repair action missing targetRepo")
        if target_repo_node and target_repo != target_repo_node:
            raise ValueError(
                f"catalog repair target mismatch: action={target_repo} "
                f"request={target_repo_node}"
            )
        if source_repo == target_repo:
            raise ValueError("catalog repair sourceRepo and targetRepo must differ")
        min_replication_factor = int(obj.get("minReplicationFactor", 1) or 1)
        max_replication_factor = int(
            obj.get("maxReplicationFactor", min_replication_factor) or
            min_replication_factor
        )
        if min_replication_factor < 1:
            raise ValueError("catalog repair minReplicationFactor must be >= 1")
        if max_replication_factor < min_replication_factor:
            raise ValueError(
                "catalog repair maxReplicationFactor must be >= minReplicationFactor"
            )
        return RepoRepairAction(
            object_name=object_name,
            object_sha256=object_sha256,
            manifest_sha256=manifest_sha256,
            source_repo=source_repo,
            target_repo=target_repo,
            min_replication_factor=min_replication_factor,
            max_replication_factor=max_replication_factor,
            reason=str(obj.get("reason", "under-replicated")),
            action_type=action_type,
            schema_version=schema_version,
        )


def large_data_reference_from_repo_manifest(
    manifest: RepoObjectManifest | dict,
    *,
    object_type: str = "",
    object_id: str = "",
) -> dict:
    """Return the generic large-object reference metadata for a repo manifest."""

    manifest_dict = manifest.to_dict() if isinstance(manifest, RepoObjectManifest) else dict(manifest)
    return {
        "source": "repo-manifest",
        "dataName": str(manifest_dict.get("objectName", "")),
        "objectType": object_type or str(manifest_dict.get("objectType", "")),
        "objectId": object_id or str(manifest_dict.get("objectName", "")),
        "plaintextSize": int(manifest_dict.get("size", 0)),
        "encrypted": _boolish(manifest_dict.get("encrypted", False), False),
        "digest": "sha256:" + str(manifest_dict.get("sha256", "")),
    }


def repo_artifact_reference(
    manifest: RepoObjectManifest | dict,
    *,
    object_type: str = "",
    object_id: str = "",
) -> dict:
    """Wrap a repo manifest with explicit large-data reference metadata."""

    manifest_dict = manifest.to_dict() if isinstance(manifest, RepoObjectManifest) else dict(manifest)
    return {
        "repoManifest": manifest_dict,
        "largeDataReference": large_data_reference_from_repo_manifest(
            manifest_dict,
            object_type=object_type,
            object_id=object_id,
        ),
    }


def repo_manifest_from_artifact_reference(entry: dict) -> dict:
    """Extract the repo manifest from a new or legacy artifact manifest entry."""

    if not isinstance(entry, dict):
        raise ValueError("repo artifact entry must be a mapping")
    if "largeDataReference" in entry:
        reference = entry.get("largeDataReference", {})
        if not isinstance(reference, dict):
            raise ValueError("largeDataReference must be a mapping")
        source = str(reference.get("source", ""))
        if source and source != "repo-manifest":
            raise ValueError(
                "unsupported artifact largeDataReference source: "
                f"{source}"
            )
        manifest = dict(entry.get("repoManifest", entry.get("repo_manifest", {})))
        if not manifest:
            raise ValueError(
                "repo-backed artifact largeDataReference missing repoManifest"
            )
        digest = str(reference.get("digest", ""))
        if digest.startswith("sha256:"):
            expected = digest.split(":", 1)[1]
            actual = str(manifest.get("sha256", ""))
            if expected and actual and expected != actual:
                raise ValueError(
                    "largeDataReference digest does not match repoManifest: "
                    f"reference={expected} manifest={actual}"
                )
        return manifest
    if "repoManifest" in entry:
        return dict(entry["repoManifest"])
    if "repo_manifest" in entry:
        return dict(entry["repo_manifest"])
    return dict(entry)


def repo_manifest_from_large_data_reference(entry: dict) -> dict:
    """Resolve a repo-backed artifact through the large-data reference layer.

    New planner/executor code should call this helper instead of directly
    reading ``repoManifest``. The implementation still accepts legacy manifest
    shapes so older generated policies keep working during migration.
    """

    return repo_manifest_from_artifact_reference(entry)


@dataclass(frozen=True)
class StorageCapability:
    repo_node: str
    free_bytes: int
    used_bytes: int = 0
    recent_load: float = 0.0
    availability_score: float = 1.0
    failure_domain: str = ""
    storage_classes: tuple[str, ...] = ("model", "intermediate")
    repo_mode: str = "persistent"
    accepts_backup_replica: bool = True
    queue_depth: int = 0
    inflight_operations: int = 0
    storage_latency_ms: float = 0.0
    network_rtt_ms: float = 0.0
    network_bandwidth_mbps: float = 0.0


@dataclass(frozen=True)
class PlacementPolicy:
    replication_factor: int = 1
    avoid_same_failure_domain: bool = True
    prefer_low_load: bool = True
    prefer_high_availability: bool = True


@dataclass(frozen=True)
class RepoPlacement:
    object_name: str
    replicas: tuple[StorageCapability, ...]

    @property
    def replica_names(self) -> tuple[str, ...]:
        return tuple(replica.repo_node for replica in self.replicas)


@dataclass
class _RepoNodeState:
    capability: StorageCapability
    objects: dict[str, bytes]
    manifests: dict[str, RepoObjectManifest]
    available: bool = True

    @property
    def free_bytes(self) -> int:
        used = sum(len(payload) for payload in self.objects.values())
        return max(0, self.capability.free_bytes - used)

    def effective_capability(self) -> StorageCapability:
        used = sum(len(payload) for payload in self.objects.values())
        return StorageCapability(
            repo_node=self.capability.repo_node,
            free_bytes=max(0, self.capability.free_bytes - used),
            used_bytes=self.capability.used_bytes + used,
            recent_load=self.capability.recent_load,
            availability_score=(self.capability.availability_score
                                if self.available else 0.0),
            failure_domain=self.capability.failure_domain,
            storage_classes=self.capability.storage_classes,
            repo_mode=self.capability.repo_mode,
            accepts_backup_replica=self.capability.accepts_backup_replica,
            queue_depth=self.capability.queue_depth,
            inflight_operations=self.capability.inflight_operations,
            storage_latency_ms=self.capability.storage_latency_ms,
            network_rtt_ms=self.capability.network_rtt_ms,
            network_bandwidth_mbps=self.capability.network_bandwidth_mbps,
        )


class LocalDistributedRepo:
    """Deterministic local repo-cluster planner used by examples and smoke tests.

    The C++ DistributedRepo subproject owns the long-term repo-node service
    implementation. This Python class mirrors its manifest and placement rules
    so NDNSF-DI examples can already carry repo object references in plans and
    validate store/fetch behavior before running a full NDNSF repo cluster.
    """

    def __init__(self, capabilities: Iterable[StorageCapability]):
        self._nodes = {
            capability.repo_node: _RepoNodeState(capability, {}, {})
            for capability in capabilities
        }

    @property
    def capabilities(self) -> tuple[StorageCapability, ...]:
        return tuple(node.effective_capability() for node in self._nodes.values())

    @property
    def objects(self) -> dict[str, tuple[RepoObjectManifest, bytes]]:
        merged: dict[str, tuple[RepoObjectManifest, bytes]] = {}
        for node in self._nodes.values():
            for object_name, payload in node.objects.items():
                merged.setdefault(object_name, (node.manifests[object_name], payload))
        return merged

    def put(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str = "artifact",
        policy: PlacementPolicy = PlacementPolicy(),
        policy_epoch: str = "",
    ) -> RepoObjectManifest:
        replicas = select_replicas(self.capabilities, policy, len(payload))
        if len(replicas) < policy.replication_factor:
            raise RuntimeError(
                f"not enough repo nodes for {object_name}: "
                f"need {policy.replication_factor}, got {len(replicas)}")
        manifest = RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
            segment_count=1,
            replication_factor=policy.replication_factor,
            replica_nodes=tuple(replica.repo_node for replica in replicas),
            policy_epoch=policy_epoch,
        )
        for replica in replicas:
            node = self._nodes[replica.repo_node]
            node.objects[object_name] = bytes(payload)
            node.manifests[object_name] = manifest
        return manifest

    def fetch(self, object_name: str) -> bytes:
        manifest = self.manifest(object_name)
        replica_names = manifest.replica_nodes or tuple(self._nodes)
        candidates = [
            self._nodes[name] for name in replica_names
            if name in self._nodes and self._nodes[name].available and
            object_name in self._nodes[name].objects
        ]
        if not candidates:
            raise KeyError(f"no available repo replica for {object_name}")
        candidates.sort(key=lambda node: _score(node.effective_capability()),
                        reverse=True)
        payload = candidates[0].objects[object_name]
        if hashlib.sha256(payload).hexdigest() != manifest.sha256:
            raise ValueError(f"repo object hash mismatch: {object_name}")
        return payload

    def get(self, object_name: str) -> bytes:
        return self.fetch(object_name)

    def fetch_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
        """Fetch one logical object and verify it against its manifest."""

        manifest = manifest or self.manifest(object_name)
        payload = self.fetch(manifest.object_name)
        if len(payload) != manifest.size:
            raise ValueError(f"repo object size mismatch: {manifest.object_name}")
        if hashlib.sha256(payload).hexdigest() != manifest.sha256:
            raise ValueError(f"repo object hash mismatch: {manifest.object_name}")
        return payload

    def get_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
        return self.fetch_object(object_name, manifest)

    def put_manifest(self, manifest: RepoObjectManifest) -> None:
        replicas = manifest.replica_nodes or tuple(self._nodes)
        for repo_node in replicas:
            if repo_node in self._nodes:
                self._nodes[repo_node].manifests[manifest.object_name] = manifest

    def erase(self, object_name: str) -> bool:
        removed = False
        for node in self._nodes.values():
            if object_name in node.objects:
                node.objects.pop(object_name, None)
                removed = True
            if object_name in node.manifests:
                node.manifests.pop(object_name, None)
                removed = True
        return removed

    def manifest(self, object_name: str) -> RepoObjectManifest:
        for node in self._nodes.values():
            if object_name in node.manifests:
                return node.manifests[object_name]
        raise KeyError(object_name)

    def inventory(self, repo_node: str | None = None) -> dict[str, RepoObjectManifest]:
        if repo_node is not None:
            return dict(self._nodes[repo_node].manifests)
        merged: dict[str, RepoObjectManifest] = {}
        for node in self._nodes.values():
            merged.update(node.manifests)
        return merged

    def set_available(self, repo_node: str, available: bool) -> None:
        self._nodes[repo_node].available = available


def encode_repo_request(operation: str, **fields) -> bytes:
    return json.dumps({
        "operation": operation,
        **fields,
    }, sort_keys=True, separators=(",", ":")).encode()


def decode_repo_request(payload: bytes) -> dict:
    decoded = json.loads(payload.decode())
    if not isinstance(decoded, dict) or "operation" not in decoded:
        raise ValueError("repo request must be a JSON object with operation")
    return decoded


@dataclass
class _RepoHotCacheEntry:
    manifest: RepoObjectManifest
    value: object
    charge_bytes: int


class _BoundedRepoHotCache:
    """One logical-byte-bounded LRU shared by object and packet entries."""

    def __init__(self, budget_bytes: int) -> None:
        self.budget_bytes = max(0, int(budget_bytes))
        self._entries: OrderedDict[tuple[str, str], _RepoHotCacheEntry] = OrderedDict()
        self._used_bytes = 0
        self._lock = threading.RLock()
        self._hits = 0
        self._misses = 0
        self._admissions = 0
        self._evictions = 0
        self._invalidations = 0
        self._oversized_bypasses = 0
        self._backing_reads = 0
        self._backing_writes = 0

    @staticmethod
    def _manifest_charge(manifest: RepoObjectManifest) -> int:
        return len(manifest.object_name.encode()) + len(manifest.to_bytes())

    @classmethod
    def _object_charge(cls, manifest: RepoObjectManifest, payload: bytes) -> int:
        return cls._manifest_charge(manifest) + len(payload)

    @classmethod
    def _packet_charge(cls, manifest: RepoObjectManifest,
                       packets: list[DataPacket]) -> int:
        return cls._manifest_charge(manifest) + sum(
            len(packet.name.encode()) + len(packet.wire) for packet in packets
        )

    @property
    def used_bytes(self) -> int:
        with self._lock:
            return self._used_bytes

    def _get(self, key: tuple[str, str]) -> Optional[_RepoHotCacheEntry]:
        with self._lock:
            entry = self._entries.get(key)
            if entry is None:
                self._misses += 1
                return None
            self._entries.move_to_end(key)
            self._hits += 1
            return entry

    def get_object(self, object_name: str) -> Optional[tuple[RepoObjectManifest, bytes]]:
        entry = self._get(("object", object_name))
        if entry is None:
            return None
        return entry.manifest, bytes(entry.value)

    def get_packets(self, object_name: str) -> Optional[tuple[RepoObjectManifest, list[DataPacket]]]:
        entry = self._get(("packets", object_name))
        if entry is None:
            return None
        return entry.manifest, list(entry.value)

    def get_packet(self, data_name: str) -> Optional[DataPacket]:
        entry = self._get(("packet", data_name))
        if entry is None:
            return None
        packet = entry.value
        return DataPacket(packet.name, packet.segment, bytes(packet.wire))

    def get_any(self, object_name: str) -> Optional[tuple[str, RepoObjectManifest, object]]:
        with self._lock:
            for kind in ("object", "packets"):
                key = (kind, object_name)
                entry = self._entries.get(key)
                if entry is None:
                    continue
                self._entries.move_to_end(key)
                self._hits += 1
                value = bytes(entry.value) if kind == "object" else list(entry.value)
                return kind, entry.manifest, value
            self._misses += 1
            return None

    def _put(self, key: tuple[str, str], entry: _RepoHotCacheEntry) -> None:
        with self._lock:
            old = self._entries.pop(key, None)
            if old is not None:
                self._used_bytes -= old.charge_bytes
                self._invalidations += 1
            if self.budget_bytes <= 0:
                return
            if entry.charge_bytes > self.budget_bytes:
                self._oversized_bypasses += 1
                return
            while (self._entries and
                   self._used_bytes > self.budget_bytes - entry.charge_bytes):
                _, victim = self._entries.popitem(last=False)
                self._used_bytes -= victim.charge_bytes
                self._evictions += 1
            try:
                self._entries[key] = entry
            except MemoryError:
                return
            self._used_bytes += entry.charge_bytes
            self._admissions += 1

    def put_object(self, manifest: RepoObjectManifest, payload: bytes) -> None:
        self.invalidate(manifest.object_name)
        self._put(
            ("object", manifest.object_name),
            _RepoHotCacheEntry(
                manifest=manifest,
                value=bytes(payload),
                charge_bytes=self._object_charge(manifest, payload),
            ),
        )

    def put_packets(self, manifest: RepoObjectManifest,
                    packets: list[DataPacket]) -> None:
        copied = list(packets)
        self.invalidate(manifest.object_name)
        self._put(
            ("packets", manifest.object_name),
            _RepoHotCacheEntry(
                manifest=manifest,
                value=copied,
                charge_bytes=self._packet_charge(manifest, copied),
            ),
        )

    def put_packet(self, packet: DataPacket) -> None:
        self._put(
            ("packet", packet.name),
            _RepoHotCacheEntry(
                manifest=RepoObjectManifest(
                    object_name=packet.name,
                    object_type="ndn-data-wire",
                    sha256=hashlib.sha256(packet.wire).hexdigest(),
                    size=len(packet.wire),
                    packet_names=(packet.name,),
                ),
                value=DataPacket(packet.name, packet.segment, bytes(packet.wire)),
                charge_bytes=len(packet.name.encode()) + len(packet.wire),
            ),
        )

    def invalidate_packet(self, data_name: str) -> None:
        with self._lock:
            old = self._entries.pop(("packet", data_name), None)
            if old is not None:
                self._used_bytes -= old.charge_bytes
                self._invalidations += 1

    def invalidate(self, object_name: str) -> None:
        with self._lock:
            for key in (("object", object_name), ("packets", object_name)):
                old = self._entries.pop(key, None)
                if old is not None:
                    self._used_bytes -= old.charge_bytes
                    self._invalidations += 1

    def record_backing_read(self) -> None:
        with self._lock:
            self._backing_reads += 1

    def record_backing_write(self) -> None:
        with self._lock:
            self._backing_writes += 1

    def status(self, *, storage_backend: str,
               authoritative_backend: str) -> dict[str, object]:
        with self._lock:
            return {
                "storageBackend": storage_backend,
                "authoritativeBackend": authoritative_backend,
                "cachePolicy": "lru" if self.budget_bytes > 0 else "disabled",
                "budgetBytes": self.budget_bytes,
                "usedBytes": self._used_bytes,
                "entryCount": len(self._entries),
                "hits": self._hits,
                "misses": self._misses,
                "admissions": self._admissions,
                "evictions": self._evictions,
                "invalidations": self._invalidations,
                "oversizedBypasses": self._oversized_bypasses,
                "backingReads": self._backing_reads,
                "backingWrites": self._backing_writes,
            }


class RepoNodeApp:
    """Real NDNSF repo node using versioned public and peer-only services."""

    def __init__(
        self,
        *,
        repo_node: str,
        service_name: str = "/NDNSF/DistributedRepo",
        provider_id: str = "",
        group: str = "/NDNSF-DistributeInference/example/group",
        controller: str = "/NDNSF-DistributeInference/example/controller",
        provider_prefix: str = "/NDNSF-DistributeInference/example/provider",
        trust_schema: str = "examples/trust-schema.conf",
        free_bytes: int = 4_000_000_000,
        failure_domain: str = "",
        storage_classes: tuple[str, ...] = ("model", "intermediate"),
        storage_dir: str | Path | None = None,
        memory_cache_bytes: int = 64 * 1024 * 1024,
        preallocate_bytes: int = 0,
        advertise_stored_prefixes: bool = False,
        advertise_command: str = "nlsrc",
        repo_mode: str = "persistent",
        accepts_backup_replica: bool = True,
        peer_repo_nodes: tuple[str, ...] = (),
        peer_provider_identities: tuple[str, ...] = (),
        catalog_sync_interval_s: float = 10.0,
        handler_threads: int = 4,
        ack_threads: int = 2,
        serve_certificates: bool = True,
        producer_retention_s: float = 120.0,
        exact_data_validation_policy: str = "wire-name-and-request-digest",
    ) -> None:
        self.repo_node = repo_node
        self.service_name = service_name
        self.group = group
        self.controller = controller
        self.trust_schema = trust_schema
        self.provider_prefix = provider_prefix.rstrip("/")
        self.provider_name = (
            f"{self.provider_prefix}/{provider_id.strip('/')}"
            if provider_id else self.provider_prefix
        )
        self.peer_provider_identities = frozenset(
            str(identity).rstrip("/")
            for identity in peer_provider_identities if str(identity).strip()
        )
        if exact_data_validation_policy not in {
                "wire-name-and-request-digest", "wire-name-only"}:
            raise ValueError("unsupported exact Data validation policy")
        self.exact_data_validation_policy = exact_data_validation_policy
        self.capability = StorageCapability(
            repo_node=repo_node,
            free_bytes=free_bytes,
            failure_domain=failure_domain,
            storage_classes=storage_classes,
            repo_mode=repo_mode,
            accepts_backup_replica=accepts_backup_replica,
        )
        self.provider = ServiceProvider(
            provider_id=provider_id,
            group=group,
            controller=controller,
            provider_prefix=provider_prefix,
            trust_schema=trust_schema,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            serve_certificates=serve_certificates,
        )
        default_storage_root = Path(os.environ.get(
            "NDNSF_REPO_STORAGE_ROOT", "/tmp/ndnsf-distributed-repo"))
        default_storage_name = hashlib.sha256(repo_node.encode()).hexdigest()[:16]
        self.storage_dir = (Path(storage_dir) if storage_dir else
                            default_storage_root / default_storage_name)
        self.storage_dir.mkdir(parents=True, exist_ok=True)
        self.capacity_bytes = free_bytes
        self.memory_cache_bytes = max(0, memory_cache_bytes)
        self._hot_cache = _BoundedRepoHotCache(self.memory_cache_bytes)
        self._cache_bytes = 0
        self._db_lock = threading.RLock()
        self._db: Optional[sqlite3.Connection] = sqlite3.connect(
            self.storage_dir / "repo.sqlite3",
            check_same_thread=False,
        )
        self._init_sqlite()
        if preallocate_bytes > 0:
            reserve = self.storage_dir / "repo.reserve"
            with reserve.open("ab") as file:
                file.truncate(preallocate_bytes)
        # Kept as an accepted constructor option for callers from the former
        # per-request producer path. The always-on data plane has no retention
        # timer and serves every persisted prefix until shutdown.
        _ = producer_retention_s
        self.advertise_stored_prefixes = advertise_stored_prefixes
        self.advertise_command = advertise_command
        self._advertised_prefixes: set[str] = set()
        self._data_plane = RepoDataPlaneProducer(
            self._lookup_data_plane_wire,
            signing_identity=self.provider_name,
            forwarding_route_prefixes=[self._serving_forwarding_hint("")],
        )
        self._restore_serving_prefixes()
        self.peer_repo_nodes = tuple(
            peer for peer in peer_repo_nodes
            if peer and peer.rstrip("/") != self.repo_node.rstrip("/")
        )
        self.catalog_sync_interval_s = max(0.5, float(catalog_sync_interval_s))
        self._catalog_lock = threading.RLock()
        self._catalog_epoch = 0
        self._catalog_changes: list[dict] = []
        self._global_catalog: dict[str, dict[str, dict]] = {}
        self._repo_status: dict[str, dict] = {}
        self._peer_catalog_epochs: dict[str, int] = {}
        self._catalog_stale_after_ms = 30_000
        self._catalog_boot_id = uuid.uuid4().hex
        self._catalog_sequence = 0
        self._catalog_history_limit = 10_000
        self._catalog_stop = threading.Event()
        self._catalog_thread: Optional[threading.Thread] = None
        self._restore_catalog_state()

    def _init_sqlite(self) -> None:
        assert self._db is not None
        if not hasattr(self, "_read_local"):
            self._read_local = threading.local()
        if not hasattr(self, "_object_locks"):
            self._object_locks = tuple(threading.RLock() for _ in range(64))
        if not hasattr(self, "_runtime_lock"):
            self._runtime_lock = threading.RLock()
            self._runtime = {
                "queueDepth": 0,
                "inflightReads": 0,
                "inflightWrites": 0,
                "inflightRepair": 0,
                "rejected": 0,
                "storageReadLatencyMs": 0.0,
                "storageWriteLatencyMs": 0.0,
                "metricsTimestampMs": self._now_ms(),
            }
        if not hasattr(self, "_read_semaphore"):
            self._read_semaphore = threading.BoundedSemaphore(32)
        if not hasattr(self, "_write_semaphore"):
            self._write_semaphore = threading.BoundedSemaphore(4)
        with self._db_lock:
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.execute("PRAGMA synchronous=NORMAL")
            self._db.execute("PRAGMA foreign_keys=ON")
            self._db.execute("PRAGMA busy_timeout=5000")
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS objects (
                    object_name TEXT PRIMARY KEY,
                    manifest_json TEXT NOT NULL,
                    payload BLOB,
                    payload_size INTEGER NOT NULL DEFAULT 0,
                    sha256 TEXT NOT NULL,
                    object_type TEXT NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS data_segments (
                    object_name TEXT NOT NULL,
                    segment_no INTEGER NOT NULL,
                    data_name TEXT NOT NULL,
                    wire BLOB NOT NULL,
                    wire_size INTEGER NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (object_name, segment_no)
                )
            """)
            self._db.execute("""
                CREATE INDEX IF NOT EXISTS idx_data_segments_data_name
                ON data_segments(data_name)
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS data_packets (
                    data_name TEXT PRIMARY KEY,
                    wire BLOB NOT NULL,
                    wire_sha256 TEXT NOT NULL,
                    wire_size INTEGER NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS object_packet_refs (
                    object_name TEXT NOT NULL,
                    ordinal INTEGER NOT NULL,
                    segment_no INTEGER NOT NULL,
                    data_name TEXT NOT NULL,
                    PRIMARY KEY (object_name, ordinal),
                    UNIQUE (object_name, data_name),
                    FOREIGN KEY (data_name) REFERENCES data_packets(data_name)
                )
            """)
            self._db.execute("""
                CREATE INDEX IF NOT EXISTS idx_object_packet_refs_data_name
                ON object_packet_refs(data_name)
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS repo_meta (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS write_operations (
                    operation_id TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL,
                    expected_generation INTEGER NOT NULL DEFAULT -1,
                    digest TEXT NOT NULL,
                    replication_factor INTEGER NOT NULL,
                    required_acks INTEGER NOT NULL,
                    consistency TEXT NOT NULL,
                    selected_replicas_json TEXT NOT NULL DEFAULT '[]',
                    state TEXT NOT NULL,
                    error TEXT NOT NULL DEFAULT '',
                    created_at_ms INTEGER NOT NULL,
                    updated_at_ms INTEGER NOT NULL
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS write_receipts (
                    operation_id TEXT NOT NULL,
                    repo_node TEXT NOT NULL,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL,
                    digest TEXT NOT NULL,
                    state TEXT NOT NULL,
                    persisted_bytes INTEGER NOT NULL,
                    completed_at_ms INTEGER NOT NULL,
                    PRIMARY KEY (operation_id, repo_node),
                    FOREIGN KEY (operation_id) REFERENCES write_operations(operation_id)
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS serving_prefixes (
                    prefix TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    active INTEGER NOT NULL DEFAULT 1
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS serving_packets (
                    data_name TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    wire BLOB NOT NULL,
                    wire_sha256 TEXT NOT NULL,
                    wire_size INTEGER NOT NULL
                )
            """)
            self._db.execute("""
                CREATE INDEX IF NOT EXISTS idx_serving_packets_object
                ON serving_packets(object_name, generation)
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS catalog_journal (
                    source_repo TEXT NOT NULL,
                    source_boot_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    state TEXT NOT NULL,
                    digest TEXT NOT NULL,
                    entry_json TEXT NOT NULL,
                    PRIMARY KEY (source_repo, source_boot_id, source_sequence)
                )
            """)
            self._db.execute("""
                CREATE INDEX IF NOT EXISTS idx_catalog_journal_object
                ON catalog_journal(object_name, generation)
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS catalog_tombstones (
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL,
                    source_repo TEXT NOT NULL,
                    source_boot_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    entry_json TEXT NOT NULL,
                    PRIMARY KEY (object_name, generation)
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS peer_watermarks (
                    peer_repo TEXT PRIMARY KEY,
                    peer_boot_id TEXT NOT NULL DEFAULT '',
                    source_sequence INTEGER NOT NULL DEFAULT 0,
                    updated_at_ms INTEGER NOT NULL
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS repo_membership (
                    repo_node TEXT PRIMARY KEY,
                    boot_id TEXT NOT NULL,
                    last_sequence INTEGER NOT NULL DEFAULT 0,
                    last_seen_ms INTEGER NOT NULL,
                    status_json TEXT NOT NULL
                )
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS repair_jobs (
                    repair_id TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    source_repo TEXT NOT NULL,
                    target_repo TEXT NOT NULL,
                    state TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    next_attempt_ms INTEGER NOT NULL DEFAULT 0,
                    lease_owner TEXT NOT NULL DEFAULT '',
                    lease_deadline_ms INTEGER NOT NULL DEFAULT 0,
                    result_json TEXT NOT NULL DEFAULT '{}',
                    available_replicas INTEGER NOT NULL DEFAULT 0,
                    missing_replicas INTEGER NOT NULL DEFAULT 1,
                    object_priority INTEGER NOT NULL DEFAULT 0,
                    object_updated_at_ms INTEGER NOT NULL DEFAULT 0
                )
            """)
            repair_columns = {
                str(row[1]) for row in
                self._db.execute("PRAGMA table_info(repair_jobs)").fetchall()
            }
            repair_migrations = {
                "available_replicas": "INTEGER NOT NULL DEFAULT 0",
                "missing_replicas": "INTEGER NOT NULL DEFAULT 1",
                "object_priority": "INTEGER NOT NULL DEFAULT 0",
                "object_updated_at_ms": "INTEGER NOT NULL DEFAULT 0",
            }
            for column, declaration in repair_migrations.items():
                if column not in repair_columns:
                    self._db.execute(
                        f"ALTER TABLE repair_jobs ADD COLUMN {column} {declaration}")
            self._db.execute("""
                CREATE INDEX IF NOT EXISTS idx_repair_jobs_ready
                ON repair_jobs(state, next_attempt_ms)
            """)
            self._db.execute("""
                CREATE INDEX IF NOT EXISTS idx_repair_jobs_schedule
                ON repair_jobs(
                  target_repo, state, next_attempt_ms, available_replicas,
                  object_priority, object_updated_at_ms)
            """)
            self._db.execute("""
                CREATE TABLE IF NOT EXISTS capacity_reservations (
                    reservation_id TEXT PRIMARY KEY,
                    operation_id TEXT NOT NULL,
                    reserved_bytes INTEGER NOT NULL,
                    state TEXT NOT NULL,
                    expires_at_ms INTEGER NOT NULL
                )
            """)
            self._db.execute(
                "INSERT INTO repo_meta(key, value) VALUES('schema_version', '8') "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value"
            )
            # One-time compatibility migration. New writes use only exact-name
            # packet authority plus manifest references.
            legacy_rows = self._db.execute("""
                SELECT object_name, segment_no, data_name, wire, updated_at, hit_count
                FROM data_segments
                ORDER BY object_name, segment_no
            """).fetchall()
            for object_name, segment_no, data_name, wire, updated_at, hit_count in legacy_rows:
                wire_bytes = bytes(wire)
                existing = self._db.execute(
                    "SELECT wire FROM data_packets WHERE data_name=?",
                    (str(data_name),),
                ).fetchone()
                if existing is not None and bytes(existing[0]) != wire_bytes:
                    raise ValueError(
                        f"legacy repo packet conflict for exact Data name {data_name}")
                self._db.execute("""
                    INSERT OR IGNORE INTO data_packets
                      (data_name, wire, wire_sha256, wire_size, updated_at, hit_count)
                    VALUES (?, ?, ?, ?, ?, ?)
                """, (
                    str(data_name), sqlite3.Binary(wire_bytes),
                    hashlib.sha256(wire_bytes).hexdigest(), len(wire_bytes),
                    float(updated_at), int(hit_count),
                ))
                self._db.execute("""
                    INSERT OR IGNORE INTO object_packet_refs
                      (object_name, ordinal, segment_no, data_name)
                    VALUES (?, ?, ?, ?)
                """, (str(object_name), int(segment_no), int(segment_no), str(data_name)))
            if legacy_rows:
                self._db.execute("DELETE FROM data_segments")
            path_row = self._db.execute("PRAGMA database_list").fetchone()
            self._db_path = str(path_row[2]) if path_row and path_row[2] else ""
            used_row = self._db.execute(
                "SELECT value FROM repo_meta WHERE key='used_bytes'"
            ).fetchone()
            if used_row is None:
                self._used_bytes = self._calculate_used_bytes_locked()
                self._db.execute(
                    "INSERT INTO repo_meta(key, value) VALUES('used_bytes', ?)",
                    (str(self._used_bytes),),
                )
            else:
                self._used_bytes = int(used_row[0])
            self._db.commit()

    def _object_lock(self, object_name: str) -> threading.RLock:
        digest = hashlib.sha256(str(object_name).encode()).digest()
        return self._object_locks[int.from_bytes(digest[:2], "big") % len(self._object_locks)]

    def _reader_db(self) -> sqlite3.Connection:
        if not self._db_path:
            return self._db
        connection = getattr(self._read_local, "connection", None)
        if connection is None:
            connection = sqlite3.connect(
                self._db_path,
                check_same_thread=False,
                isolation_level=None,
            )
            connection.execute("PRAGMA query_only=ON")
            connection.execute("PRAGMA busy_timeout=5000")
            self._read_local.connection = connection
        return connection

    def _calculate_used_bytes_locked(self) -> int:
        assert self._db is not None
        row = self._db.execute("""
            SELECT
              (SELECT COALESCE(SUM(payload_size), 0) FROM objects) +
              (SELECT COALESCE(SUM(wire_size), 0) FROM data_packets) +
              (SELECT COALESCE(SUM(wire_size), 0) FROM serving_packets)
        """).fetchone()
        return int(row[0] if row else 0)

    def _refresh_used_bytes_locked(self) -> int:
        self._used_bytes = self._calculate_used_bytes_locked()
        self._db.execute("""
            INSERT INTO repo_meta(key, value) VALUES('used_bytes', ?)
            ON CONFLICT(key) DO UPDATE SET value=excluded.value
        """, (str(self._used_bytes),))
        return self._used_bytes

    def _admit_operation(self, operation: str) -> tuple[str, float]:
        write_operations = {
            "STORE", "INSERT", "STORE_PACKETS", "STORE_PACKET",
            "STORE_PACKET_BATCH", "STORE_PACKET_PULL", "STORE_MANIFEST",
            "COMMIT_PACKET_SET", "DELETE", "CATALOG_MERGE",
            "CATALOG_MERGE_PULL",
            "RESERVE_CAPACITY", "RELEASE_CAPACITY",
        }
        repair_operations = {
            "CATALOG_REPAIR", "REPAIR_SCAN", "REPAIR_CLAIM",
            "REPAIR_COMPLETE", "REPAIR_FAIL", "SCRUB",
        }
        kind = "repair" if operation in repair_operations else (
            "write" if operation in write_operations else "read")
        semaphore = self._write_semaphore if kind in {"write", "repair"} else self._read_semaphore
        with self._runtime_lock:
            self._runtime["queueDepth"] += 1
        acquired = semaphore.acquire(timeout=0.05)
        with self._runtime_lock:
            self._runtime["queueDepth"] -= 1
            if not acquired:
                self._runtime["rejected"] += 1
                self._runtime["metricsTimestampMs"] = self._now_ms()
                raise RuntimeError(REPO_REASON_OVERLOADED)
            key = "inflightRepair" if kind == "repair" else (
                "inflightWrites" if kind == "write" else "inflightReads")
            self._runtime[key] += 1
        return kind, time.monotonic()

    def _release_operation(self, admission: tuple[str, float]) -> None:
        kind, started = admission
        elapsed_ms = (time.monotonic() - started) * 1000.0
        semaphore = self._write_semaphore if kind in {"write", "repair"} else self._read_semaphore
        with self._runtime_lock:
            key = "inflightRepair" if kind == "repair" else (
                "inflightWrites" if kind == "write" else "inflightReads")
            self._runtime[key] = max(0, int(self._runtime[key]) - 1)
            latency_key = "storageWriteLatencyMs" if kind in {"write", "repair"} else "storageReadLatencyMs"
            previous = float(self._runtime[latency_key])
            self._runtime[latency_key] = elapsed_ms if previous == 0 else 0.8 * previous + 0.2 * elapsed_ms
            self._runtime["metricsTimestampMs"] = self._now_ms()
        semaphore.release()

    def _runtime_snapshot(self) -> dict[str, object]:
        with self._runtime_lock:
            return dict(self._runtime)

    def _cache_get(self, object_name: str) -> Optional[tuple[RepoObjectManifest, bytes]]:
        item = self._hot_cache.get_object(object_name)
        self._cache_bytes = self._hot_cache.used_bytes
        return item

    def _cache_put(self, manifest: RepoObjectManifest, payload: bytes) -> None:
        self._hot_cache.put_object(manifest, payload)
        self._cache_bytes = self._hot_cache.used_bytes

    def _cache_put_best_effort(self, manifest: RepoObjectManifest,
                               payload: bytes) -> None:
        try:
            self._cache_put(manifest, payload)
        except MemoryError:
            self._cache_invalidate(manifest.object_name)

    def _packet_cache_get(self, object_name: str) -> Optional[tuple[RepoObjectManifest, list[DataPacket]]]:
        item = self._hot_cache.get_packets(object_name)
        self._cache_bytes = self._hot_cache.used_bytes
        return item

    def _packet_cache_put(self, manifest: RepoObjectManifest, packets: list[DataPacket]) -> None:
        self._hot_cache.put_packets(manifest, packets)
        self._cache_bytes = self._hot_cache.used_bytes

    def _packet_cache_put_best_effort(self, manifest: RepoObjectManifest,
                                      packets: list[DataPacket]) -> None:
        try:
            self._packet_cache_put(manifest, packets)
        except MemoryError:
            self._cache_invalidate(manifest.object_name)

    def _cache_invalidate(self, object_name: str) -> None:
        self._hot_cache.invalidate(object_name)
        self._cache_bytes = self._hot_cache.used_bytes

    def _cache_status(self) -> dict[str, object]:
        if not hasattr(self, "_hot_cache"):
            backend = "sqlite" if self._db is not None else "memory"
            return {
                "storageBackend": backend,
                "authoritativeBackend": backend,
                "cachePolicy": "disabled",
                "budgetBytes": int(getattr(self, "memory_cache_bytes", 0)),
                "usedBytes": int(getattr(self, "_cache_bytes", 0)),
                "entryCount": 0,
                "hits": 0,
                "misses": 0,
                "admissions": 0,
                "evictions": 0,
                "invalidations": 0,
                "oversizedBypasses": 0,
                "backingReads": 0,
                "backingWrites": 0,
            }
        persistent = self._db is not None
        return self._hot_cache.status(
            storage_backend=("tiered" if persistent and self.memory_cache_bytes > 0
                             else "sqlite" if persistent else "memory"),
            authoritative_backend="sqlite" if persistent else "memory",
        )

    def _sqlite_used_bytes(self) -> int:
        if self._db is None:
            # Some focused unit tests construct RepoNodeApp with __new__ and a
            # tiny fake store. Normal RepoNodeApp construction always opens
            # SQLite and never reaches this test-double path.
            store = getattr(self, "_store", None)
            objects = getattr(store, "objects", {})
            return sum(len(payload) for _, payload in objects.values())
        return int(self._used_bytes)

    def _active_reserved_bytes_locked(self) -> int:
        assert self._db is not None
        now_ms = self._now_ms()
        self._db.execute("""
            UPDATE capacity_reservations SET state='EXPIRED'
            WHERE state='RESERVED' AND expires_at_ms<=?
        """, (now_ms,))
        row = self._db.execute("""
            SELECT COALESCE(SUM(reserved_bytes), 0)
            FROM capacity_reservations WHERE state='RESERVED'
        """).fetchone()
        return int(row[0] if row else 0)

    def _reserve_capacity(self, reservation_id: str, operation_id: str,
                          reserved_bytes: int, ttl_ms: int = 30_000
                          ) -> RepoCapacityReservation:
        if not reservation_id or not operation_id or reserved_bytes <= 0:
            raise ValueError("capacity reservation requires ids and positive bytes")
        with self._db_lock:
            existing = self._db.execute("""
                SELECT operation_id, reserved_bytes, state, expires_at_ms
                FROM capacity_reservations WHERE reservation_id=?
            """, (reservation_id,)).fetchone()
            if existing is not None:
                if (str(existing[0]), int(existing[1])) != (
                        operation_id, int(reserved_bytes)):
                    raise ValueError(REPO_REASON_OPERATION_CONFLICT)
                if str(existing[2]) in {"RESERVED", "CONSUMED"}:
                    return RepoCapacityReservation(
                        reservation_id, operation_id, self.repo_node,
                        int(existing[1]), str(existing[2]), int(existing[3]))
            active_reserved = self._active_reserved_bytes_locked()
            if self._used_bytes + active_reserved + reserved_bytes > self.capacity_bytes:
                self._db.commit()
                raise RuntimeError(REPO_REASON_CAPACITY_REJECTED)
            expires_at_ms = self._now_ms() + max(1000, int(ttl_ms))
            self._db.execute("""
                INSERT INTO capacity_reservations
                  (reservation_id, operation_id, reserved_bytes, state, expires_at_ms)
                VALUES (?, ?, ?, 'RESERVED', ?)
                ON CONFLICT(reservation_id) DO UPDATE SET
                  state='RESERVED', expires_at_ms=excluded.expires_at_ms
            """, (reservation_id, operation_id, int(reserved_bytes), expires_at_ms))
            self._db.commit()
        return RepoCapacityReservation(
            reservation_id, operation_id, self.repo_node, int(reserved_bytes),
            "RESERVED", expires_at_ms)

    def _release_capacity(self, *, reservation_id: str = "",
                          operation_id: str = "", state: str = "RELEASED") -> int:
        if not reservation_id and not operation_id:
            raise ValueError("reservationId or operationId is required")
        with self._db_lock:
            column = "reservation_id" if reservation_id else "operation_id"
            value = reservation_id or operation_id
            cursor = self._db.execute(
                f"UPDATE capacity_reservations SET state=? "
                f"WHERE {column}=? AND state='RESERVED'",
                (state, value),
            )
            self._db.commit()
            return int(cursor.rowcount)

    def _capability(self) -> StorageCapability:
        used = self._sqlite_used_bytes()
        with self._db_lock:
            reserved = self._active_reserved_bytes_locked()
            self._db.commit()
        return StorageCapability(
            repo_node=self.capability.repo_node,
            free_bytes=max(0, self.capacity_bytes - used - reserved),
            used_bytes=used,
            recent_load=self.capability.recent_load,
            availability_score=self.capability.availability_score,
            failure_domain=self.capability.failure_domain,
            storage_classes=self.capability.storage_classes,
            repo_mode=self.capability.repo_mode,
            accepts_backup_replica=self.capability.accepts_backup_replica,
        )

    def _load_manifest(self, object_name: str) -> RepoObjectManifest:
        assert self._db is not None
        with self._object_lock(object_name):
            row = self._reader_db().execute(
                "SELECT manifest_json FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
        if row is None:
            raise KeyError(object_name)
        return RepoObjectManifest.from_dict(json.loads(str(row[0])))

    def _write_intent_from_request(self, request: dict,
                                   manifest: RepoObjectManifest) -> RepoWriteIntent:
        intent_obj = request.get("writeIntent")
        if isinstance(intent_obj, dict):
            intent = RepoWriteIntent.from_dict(intent_obj)
        else:
            operation_id = str(
                request.get("operationId", manifest.operation_id) or uuid.uuid4())
            intent = RepoWriteIntent(
                operation_id=operation_id,
                object_name=manifest.object_name,
                generation=manifest.generation,
                expected_generation=manifest.parent_generation,
                digest=manifest.sha256,
                replication_factor=manifest.replication_factor,
                required_acks=manifest.required_write_acks,
                consistency=manifest.write_consistency,
                selected_replicas=manifest.replica_nodes,
            )
        if (intent.object_name != manifest.object_name or
                intent.generation != manifest.generation or
                intent.digest != manifest.sha256):
            raise ValueError(
                f"{REPO_REASON_INTEGRITY_FAILURE}: write intent/manifest mismatch")
        return intent

    def _manifest_for_write_intent(self, manifest: RepoObjectManifest,
                                   intent: RepoWriteIntent) -> RepoObjectManifest:
        quorum_finalized = intent.required_acks <= 1
        metadata = dict(manifest.metadata or {})
        metadata["quorumFinalized"] = quorum_finalized
        return replace(
            manifest,
            generation=intent.generation,
            parent_generation=intent.expected_generation,
            write_consistency=intent.consistency,
            required_write_acks=intent.required_acks,
            operation_id=intent.operation_id,
            lifecycle_state="COMMITTED" if quorum_finalized else "RUNNING",
            confirmed_replica_nodes=(self.repo_node,),
            metadata=metadata,
        )

    @staticmethod
    def _catalog_state_for_manifest(manifest: RepoObjectManifest) -> str:
        return (
            "AVAILABLE"
            if (manifest.lifecycle_state == "COMMITTED" and
                _boolish((manifest.metadata or {}).get("quorumFinalized", True), True))
            else "STAGED"
        )

    @classmethod
    def _require_finalized_manifest(
            cls, manifest: RepoObjectManifest) -> RepoObjectManifest:
        if cls._catalog_state_for_manifest(manifest) != "AVAILABLE":
            raise ValueError(
                f"{REPO_REASON_WRITE_INCOMPLETE}: object is not quorum finalized: "
                f"{manifest.object_name}")
        return manifest

    def _require_finalized_packet_owner(self, data_name: str) -> RepoObjectManifest:
        assert self._db is not None
        with self._db_lock:
            rows = self._db.execute("""
                SELECT o.manifest_json
                FROM object_packet_refs AS r
                JOIN objects AS o ON o.object_name=r.object_name
                WHERE r.data_name=?
            """, (data_name,)).fetchall()
        for row in rows:
            manifest = RepoObjectManifest.from_dict(json.loads(str(row[0])))
            if self._catalog_state_for_manifest(manifest) == "AVAILABLE":
                return manifest
        raise ValueError(
            f"{REPO_REASON_WRITE_INCOMPLETE}: packet has no quorum-finalized owner: "
            f"{data_name}")

    def _activate_finalized_manifest(self, manifest: RepoObjectManifest) -> None:
        self._require_finalized_manifest(manifest)
        try:
            stored_kind, _, stored_value = self._load_persisted_for_fetch(
                manifest.object_name)
        except KeyError:
            return
        if stored_kind == "packets":
            self._serve_packets(list(stored_value))
            return
        self._serve_object(
            self.data_name(self.repo_node, manifest.object_name),
            bytes(stored_value),
            manifest.object_name,
        )

    @_serialize_repo_storage
    def _finalize_write(
        self,
        manifest: RepoObjectManifest,
        intent: RepoWriteIntent,
        receipts: Iterable[RepoWriteReceipt],
    ) -> RepoObjectManifest:
        validated = validate_write_receipts(intent, receipts)
        confirmed = tuple(receipt.repo_node for receipt in validated)
        if self.repo_node not in confirmed:
            raise ValueError(
                f"{REPO_REASON_INTEGRITY_FAILURE}: finalize excludes local receipt")
        with self._db_lock:
            row = self._db.execute(
                "SELECT manifest_json FROM objects WHERE object_name=?",
                (intent.object_name,),
            ).fetchone()
            if row is None:
                raise KeyError(intent.object_name)
            stored = RepoObjectManifest.from_dict(json.loads(str(row[0])))
            if (stored.operation_id != intent.operation_id or
                    stored.generation != intent.generation or
                    stored.sha256 != intent.digest):
                raise ValueError(
                    f"{REPO_REASON_INTEGRITY_FAILURE}: finalize tuple mismatch")
            metadata = dict(stored.metadata or {})
            metadata.update(dict(manifest.metadata or {}))
            metadata["quorumFinalized"] = True
            replica_data_names = tuple(manifest.replica_data_names)
            if replica_data_names and len(replica_data_names) != len(confirmed):
                replica_data_names = tuple(
                    replica_data_names[0] for _ in confirmed)
            finalized = replace(
                stored,
                replication_factor=intent.replication_factor,
                replica_nodes=confirmed,
                replica_data_names=replica_data_names,
                confirmed_replica_nodes=confirmed,
                write_consistency=intent.consistency,
                required_write_acks=intent.required_acks,
                operation_id=intent.operation_id,
                lifecycle_state="COMMITTED",
                metadata=metadata,
            )
            self._db.execute("""
                UPDATE objects SET manifest_json=?, updated_at=?
                WHERE object_name=?
            """, (
                json.dumps(finalized.to_dict(), sort_keys=True),
                time.time(), finalized.object_name,
            ))
            self._db.execute("""
                UPDATE write_operations SET state='COMMITTED',
                  updated_at_ms=?, error=''
                WHERE operation_id=?
            """, (self._now_ms(), intent.operation_id))
            self._db.commit()
        self._cache_invalidate(finalized.object_name)
        self._remember_catalog_change(finalized, "AVAILABLE")
        self._activate_finalized_manifest(finalized)
        return finalized

    def _load_write_receipt_locked(self, operation_id: str) -> Optional[RepoWriteReceipt]:
        assert self._db is not None
        row = self._db.execute("""
            SELECT operation_id, repo_node, object_name, generation, digest,
                   persisted_bytes, state, completed_at_ms
            FROM write_receipts
            WHERE operation_id=? AND repo_node=?
        """, (operation_id, self.repo_node)).fetchone()
        if row is None:
            return None
        return RepoWriteReceipt(
            operation_id=str(row[0]),
            repo_node=str(row[1]),
            object_name=str(row[2]),
            generation=int(row[3]),
            digest=str(row[4]),
            persisted_bytes=int(row[5]),
            state=str(row[6]),
            completed_at_ms=int(row[7]),
        )

    def _ensure_write_intent_locked(self, intent: RepoWriteIntent) -> Optional[RepoWriteReceipt]:
        assert self._db is not None
        row = self._db.execute("""
            SELECT object_name, generation, digest, replication_factor,
                   required_acks, consistency
            FROM write_operations WHERE operation_id=?
        """, (intent.operation_id,)).fetchone()
        if row is not None:
            expected = (
                intent.object_name, intent.generation, intent.digest,
                intent.replication_factor, intent.required_acks, intent.consistency,
            )
            actual = (str(row[0]), int(row[1]), str(row[2]), int(row[3]),
                      int(row[4]), str(row[5]))
            if actual != expected:
                raise ValueError(
                    f"{REPO_REASON_OPERATION_CONFLICT}: operation ID reused with different content")
            return self._load_write_receipt_locked(intent.operation_id)

        now_ms = self._now_ms()
        self._db.execute("""
            INSERT INTO write_operations
              (operation_id, object_name, generation, expected_generation,
               digest, replication_factor, required_acks, consistency,
               selected_replicas_json, state, error, created_at_ms, updated_at_ms)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '', ?, ?)
        """, (
            intent.operation_id, intent.object_name, intent.generation,
            intent.expected_generation, intent.digest, intent.replication_factor,
            intent.required_acks, intent.consistency,
            json.dumps(list(intent.selected_replicas), sort_keys=True),
            "RUNNING", now_ms, now_ms,
        ))
        return None

    def _validate_write_generation_locked(self, intent: RepoWriteIntent) -> None:
        assert self._db is not None
        if intent.expected_generation < 0:
            return
        row = self._db.execute(
            "SELECT manifest_json FROM objects WHERE object_name=?",
            (intent.object_name,),
        ).fetchone()
        current_generation = -1
        if row is not None:
            current_generation = RepoObjectManifest.from_dict(
                json.loads(str(row[0]))).generation
        if (current_generation != intent.expected_generation or
                intent.generation != intent.expected_generation + 1):
            raise ValueError(
                f"{REPO_REASON_GENERATION_CONFLICT}: "
                f"expected={intent.expected_generation} current={current_generation} "
                f"proposed={intent.generation}")

    def _commit_write_receipt_locked(self, intent: RepoWriteIntent,
                                     persisted_bytes: int) -> RepoWriteReceipt:
        assert self._db is not None
        receipt = RepoWriteReceipt(
            operation_id=intent.operation_id,
            repo_node=self.repo_node,
            object_name=intent.object_name,
            generation=intent.generation,
            digest=intent.digest,
            persisted_bytes=persisted_bytes,
            state="COMMITTED",
            completed_at_ms=self._now_ms(),
        )
        self._db.execute("""
            INSERT INTO write_receipts
              (operation_id, repo_node, object_name, generation, digest,
               state, persisted_bytes, completed_at_ms)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(operation_id, repo_node) DO NOTHING
        """, (
            receipt.operation_id, receipt.repo_node, receipt.object_name,
            receipt.generation, receipt.digest, receipt.state,
            receipt.persisted_bytes, receipt.completed_at_ms,
        ))
        self._db.execute("""
            UPDATE write_operations
            SET state='COMMITTED', updated_at_ms=?, error=''
            WHERE operation_id=?
        """, (receipt.completed_at_ms, intent.operation_id))
        self._db.execute("""
            UPDATE capacity_reservations SET state='CONSUMED'
            WHERE operation_id=? AND state='RESERVED'
        """, (intent.operation_id,))
        self._cleanup_operation_status_locked()
        return receipt

    def _cleanup_operation_status_locked(self, max_entries: int = 4096) -> None:
        """Bound durable terminal-operation evidence without deleting live work."""

        assert self._db is not None
        terminal_rows = self._db.execute("""
            SELECT operation_id FROM write_operations
            WHERE state IN ('COMMITTED', 'FAILED', 'EXPIRED')
            ORDER BY updated_at_ms DESC
            LIMIT -1 OFFSET ?
        """, (max(1, int(max_entries)),)).fetchall()
        stale_ids = [str(row[0]) for row in terminal_rows]
        if not stale_ids:
            return
        placeholders = ",".join("?" for _ in stale_ids)
        self._db.execute(
            f"DELETE FROM write_receipts WHERE operation_id IN ({placeholders})",
            stale_ids,
        )
        self._db.execute(
            f"DELETE FROM write_operations WHERE operation_id IN ({placeholders})",
            stale_ids,
        )

    def _record_write_failure_locked(self, intent: RepoWriteIntent,
                                     error: str) -> None:
        assert self._db is not None
        now_ms = self._now_ms()
        row = self._db.execute("""
            SELECT object_name, generation, digest
            FROM write_operations WHERE operation_id=?
        """, (intent.operation_id,)).fetchone()
        if row is not None and (
                str(row[0]), int(row[1]), str(row[2])) != (
                    intent.object_name, intent.generation, intent.digest):
            return
        self._db.execute("""
            INSERT INTO write_operations
              (operation_id, object_name, generation, expected_generation,
               digest, replication_factor, required_acks, consistency,
               selected_replicas_json, state, error, created_at_ms, updated_at_ms)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'FAILED', ?, ?, ?)
            ON CONFLICT(operation_id) DO UPDATE SET
              state='FAILED', error=excluded.error, updated_at_ms=excluded.updated_at_ms
        """, (
            intent.operation_id, intent.object_name, intent.generation,
            intent.expected_generation, intent.digest, intent.replication_factor,
            intent.required_acks, intent.consistency,
            json.dumps(list(intent.selected_replicas), sort_keys=True),
            error, now_ms, now_ms,
        ))
        self._db.execute("""
            UPDATE capacity_reservations SET state='RELEASED'
            WHERE operation_id=? AND state='RESERVED'
        """, (intent.operation_id,))
        self._cleanup_operation_status_locked()
        self._db.commit()

    @_serialize_repo_storage
    def _persist_object(self, manifest: RepoObjectManifest, payload: bytes,
                        *, intent: Optional[RepoWriteIntent] = None) -> Optional[RepoWriteReceipt]:
        assert self._db is not None
        old_size = 0
        old_packet_names: list[str] = []
        with self._db_lock:
            try:
                if intent is not None:
                    existing_receipt = self._ensure_write_intent_locked(intent)
                    if existing_receipt is not None:
                        return existing_receipt
                    self._validate_write_generation_locked(intent)
                old_packet_names = [
                    str(row[0]) for row in self._db.execute(
                        "SELECT data_name FROM object_packet_refs WHERE object_name=?",
                        (manifest.object_name,),
                    ).fetchall()
                ]
                row = self._db.execute(
                    "SELECT payload_size FROM objects WHERE object_name=?",
                    (manifest.object_name,),
                ).fetchone()
                if row is not None:
                    old_size = int(row[0])
                exclusive = self._db.execute("""
                    SELECT COALESCE(SUM(p.wire_size), 0)
                    FROM object_packet_refs AS r
                    JOIN data_packets AS p ON p.data_name=r.data_name
                    WHERE r.object_name=? AND (
                      SELECT COUNT(*) FROM object_packet_refs
                      WHERE data_name=r.data_name
                    )=1
                """, (manifest.object_name,)).fetchone()
                old_size += int(exclusive[0] if exclusive else 0)
                if len(payload) > self.capacity_bytes - self._sqlite_used_bytes() + old_size:
                    raise RuntimeError(
                        f"repo node {self.repo_node} has insufficient free space "
                        f"for {manifest.object_name}")
                self._db.execute(
                    """
                    INSERT INTO objects
                      (object_name, manifest_json, payload, payload_size, sha256,
                       object_type, updated_at, hit_count)
                    VALUES (?, ?, ?, ?, ?, ?, ?, 0)
                    ON CONFLICT(object_name) DO UPDATE SET
                      manifest_json=excluded.manifest_json,
                      payload=excluded.payload,
                      payload_size=excluded.payload_size,
                      sha256=excluded.sha256,
                      object_type=excluded.object_type,
                      updated_at=excluded.updated_at
                    """,
                    (
                        manifest.object_name,
                        json.dumps(manifest.to_dict(), sort_keys=True),
                        sqlite3.Binary(payload),
                        len(payload),
                        manifest.sha256,
                        manifest.object_type,
                        time.time(),
                    ),
                )
                self._db.execute(
                    "DELETE FROM data_segments WHERE object_name=?",
                    (manifest.object_name,),
                )
                self._db.execute(
                    "DELETE FROM object_packet_refs WHERE object_name=?",
                    (manifest.object_name,),
                )
                self._db.execute("""
                    DELETE FROM data_packets
                    WHERE NOT EXISTS (
                      SELECT 1 FROM object_packet_refs
                      WHERE object_packet_refs.data_name=data_packets.data_name
                    )
                """)
                receipt = (
                    self._commit_write_receipt_locked(intent, len(payload))
                    if intent is not None else None
                )
                self._refresh_used_bytes_locked()
                self._db.commit()
            except Exception as exc:
                self._db.rollback()
                if intent is not None:
                    self._record_write_failure_locked(intent, str(exc))
                raise
        self._hot_cache.record_backing_write()
        self._cache_put_best_effort(manifest, payload)
        for data_name in old_packet_names:
            self._hot_cache.invalidate_packet(data_name)
        self._remember_catalog_change(
            manifest, self._catalog_state_for_manifest(manifest))
        return receipt

    @_serialize_repo_storage
    def _persist_manifest(self, manifest: RepoObjectManifest,
                          *, intent: Optional[RepoWriteIntent] = None) -> Optional[RepoWriteReceipt]:
        assert self._db is not None
        old_packet_names: list[str] = []
        with self._db_lock:
            try:
                if intent is not None:
                    existing_receipt = self._ensure_write_intent_locked(intent)
                    if existing_receipt is not None:
                        return existing_receipt
                    self._validate_write_generation_locked(intent)
                old_packet_names = [
                    str(row[0]) for row in self._db.execute(
                        "SELECT data_name FROM object_packet_refs WHERE object_name=?",
                        (manifest.object_name,),
                    ).fetchall()
                ]
                self._db.execute(
                    """
                    INSERT INTO objects
                      (object_name, manifest_json, payload, payload_size, sha256,
                       object_type, updated_at, hit_count)
                    VALUES (?, ?, NULL, 0, ?, ?, ?, 0)
                    ON CONFLICT(object_name) DO UPDATE SET
                      manifest_json=excluded.manifest_json,
                      payload=NULL,
                      payload_size=0,
                      sha256=excluded.sha256,
                      object_type=excluded.object_type,
                      updated_at=excluded.updated_at
                    """,
                    (
                        manifest.object_name,
                        json.dumps(manifest.to_dict(), sort_keys=True),
                        manifest.sha256,
                        manifest.object_type,
                        time.time(),
                    ),
                )
                self._db.execute(
                    "DELETE FROM data_segments WHERE object_name=?",
                    (manifest.object_name,),
                )
                self._db.execute(
                    "DELETE FROM object_packet_refs WHERE object_name=?",
                    (manifest.object_name,),
                )
                self._db.execute("""
                    DELETE FROM data_packets
                    WHERE NOT EXISTS (
                      SELECT 1 FROM object_packet_refs
                      WHERE object_packet_refs.data_name=data_packets.data_name
                    )
                """)
                receipt = (
                    self._commit_write_receipt_locked(intent, 0)
                    if intent is not None else None
                )
                self._refresh_used_bytes_locked()
                self._db.commit()
            except Exception as exc:
                self._db.rollback()
                if intent is not None:
                    self._record_write_failure_locked(intent, str(exc))
                raise
        self._hot_cache.record_backing_write()
        self._cache_invalidate(manifest.object_name)
        for data_name in old_packet_names:
            self._hot_cache.invalidate_packet(data_name)
        self._remember_catalog_change(
            manifest, self._catalog_state_for_manifest(manifest))
        return receipt

    @_serialize_repo_storage
    def _persist_packets(self, manifest: RepoObjectManifest, packets: list[DataPacket],
                         *, intent: Optional[RepoWriteIntent] = None) -> Optional[RepoWriteReceipt]:
        assert self._db is not None
        if not packets:
            raise ValueError(f"repo packet set is empty: {manifest.object_name}")
        validated: list[DataPacket] = []
        seen_names: set[str] = set()
        seen_segments: set[int] = set()
        versioned_parent = ""
        for declared in packets:
            try:
                decoded = decode_data_packet(declared.wire)
            except Exception as exc:
                raise ValueError(f"repo-invalid-data-wire: {exc}") from exc
            if decoded.name != declared.name:
                raise ValueError(
                    "repo-data-name-mismatch: repo Data name/wire mismatch: "
                    f"declared={declared.name} encoded={decoded.name}")
            if decoded.segment != declared.segment:
                raise ValueError(
                    "repo-packet-set-invalid: repo Data segment/wire mismatch: "
                    f"declared={declared.segment} encoded={decoded.segment}")
            if decoded.name in seen_names or decoded.segment in seen_segments:
                raise ValueError(
                    f"repo-packet-set-invalid: duplicate packet: {decoded.name}")
            parent = decoded.name.rsplit("/", 1)[0] if "/" in decoded.name else decoded.name
            if versioned_parent and parent != versioned_parent:
                raise ValueError(
                    "repo-packet-set-invalid: packet set mixes Data versions or prefixes: "
                    f"{versioned_parent} vs {parent}")
            versioned_parent = parent
            seen_names.add(decoded.name)
            seen_segments.add(decoded.segment)
            validated.append(decoded)
        packets = sorted(validated, key=lambda packet: packet.segment)
        manifest = replace(
            manifest,
            segment_count=len(packets),
            packet_names=tuple(packet.name for packet in packets),
        )
        orphaned_names: set[str] = set()
        with self._db_lock:
            try:
                if intent is not None:
                    existing_receipt = self._ensure_write_intent_locked(intent)
                    if existing_receipt is not None:
                        return existing_receipt
                    self._validate_write_generation_locked(intent)
                old_object = self._db.execute(
                    "SELECT payload_size FROM objects WHERE object_name=?",
                    (manifest.object_name,),
                ).fetchone()
                old_payload_size = int(old_object[0]) if old_object else 0
                old_refs = {
                    str(row[0]) for row in self._db.execute(
                        "SELECT data_name FROM object_packet_refs WHERE object_name=?",
                        (manifest.object_name,),
                    ).fetchall()
                }
                existing_packets: dict[str, tuple[bytes, int]] = {}
                for packet in packets:
                    row = self._db.execute(
                        "SELECT wire, wire_size FROM data_packets WHERE data_name=?",
                        (packet.name,),
                    ).fetchone()
                    if row is not None:
                        stored_wire = bytes(row[0])
                        if stored_wire != packet.wire:
                            raise ValueError(
                                "repo-data-wire-conflict: immutable NDN Data name "
                                f"conflict: {packet.name}")
                        existing_packets[packet.name] = (stored_wire, int(row[1]))
                additional_bytes = sum(
                    len(packet.wire) for packet in packets
                    if packet.name not in existing_packets
                )
                reclaimed_bytes = 0
                new_names = {packet.name for packet in packets}
                for old_name in old_refs - new_names:
                    ref_count = int(self._db.execute(
                        "SELECT COUNT(*) FROM object_packet_refs WHERE data_name=?",
                        (old_name,),
                    ).fetchone()[0])
                    if ref_count == 1:
                        orphaned_names.add(old_name)
                        size_row = self._db.execute(
                            "SELECT wire_size FROM data_packets WHERE data_name=?",
                            (old_name,),
                        ).fetchone()
                        reclaimed_bytes += int(size_row[0]) if size_row else 0
                projected = (
                    self._sqlite_used_bytes() - old_payload_size +
                    additional_bytes - reclaimed_bytes
                )
                if projected > self.capacity_bytes:
                    raise RuntimeError(
                        f"repo node {self.repo_node} has insufficient free space "
                        f"for {manifest.object_name}")
                self._db.execute(
                    """
                    INSERT INTO objects
                      (object_name, manifest_json, payload, payload_size, sha256,
                       object_type, updated_at, hit_count)
                    VALUES (?, ?, NULL, 0, ?, ?, ?, 0)
                    ON CONFLICT(object_name) DO UPDATE SET
                      manifest_json=excluded.manifest_json,
                      payload=NULL,
                      payload_size=0,
                      sha256=excluded.sha256,
                      object_type=excluded.object_type,
                      updated_at=excluded.updated_at
                    """,
                    (
                        manifest.object_name,
                        json.dumps(manifest.to_dict(), sort_keys=True),
                        manifest.sha256,
                        manifest.object_type,
                        time.time(),
                    ),
                )
                self._db.execute(
                    "DELETE FROM data_segments WHERE object_name=?",
                    (manifest.object_name,),
                )
                self._db.execute(
                    "DELETE FROM object_packet_refs WHERE object_name=?",
                    (manifest.object_name,),
                )
                self._db.executemany(
                    """
                    INSERT INTO data_packets
                      (data_name, wire, wire_sha256, wire_size, updated_at, hit_count)
                    VALUES (?, ?, ?, ?, ?, 0)
                    ON CONFLICT(data_name) DO NOTHING
                    """,
                    [
                        (
                            packet.name,
                            sqlite3.Binary(packet.wire),
                            hashlib.sha256(packet.wire).hexdigest(),
                            len(packet.wire),
                            time.time(),
                        )
                        for packet in packets
                    ],
                )
                self._db.executemany(
                    """
                    INSERT INTO object_packet_refs
                      (object_name, ordinal, segment_no, data_name)
                    VALUES (?, ?, ?, ?)
                    """,
                    [
                        (manifest.object_name, ordinal, packet.segment, packet.name)
                        for ordinal, packet in enumerate(packets)
                    ],
                )
                self._db.execute("""
                    DELETE FROM data_packets
                    WHERE NOT EXISTS (
                      SELECT 1 FROM object_packet_refs
                      WHERE object_packet_refs.data_name=data_packets.data_name
                    )
                """)
                receipt = (
                    self._commit_write_receipt_locked(
                        intent, sum(len(packet.wire) for packet in packets))
                    if intent is not None else None
                )
                self._refresh_used_bytes_locked()
                self._db.commit()
            except Exception as exc:
                self._db.rollback()
                if intent is not None:
                    self._record_write_failure_locked(intent, str(exc))
                raise
        self._hot_cache.record_backing_write()
        for data_name in orphaned_names:
            self._hot_cache.invalidate_packet(data_name)
        self._packet_cache_put_best_effort(manifest, packets)
        self._remember_catalog_change(
            manifest, self._catalog_state_for_manifest(manifest))
        return receipt

    @_serialize_repo_storage
    def _persist_packet(self, manifest: RepoObjectManifest, packet: DataPacket) -> None:
        assert self._db is not None
        with self._db_lock:
            rows = self._db.execute("""
                SELECT r.segment_no, p.data_name, p.wire
                FROM object_packet_refs AS r
                JOIN data_packets AS p ON p.data_name=r.data_name
                WHERE r.object_name=?
                ORDER BY r.ordinal
            """, (manifest.object_name,)).fetchall()
        by_segment = {
            int(segment_no): DataPacket(str(data_name), int(segment_no), bytes(wire))
            for segment_no, data_name, wire in rows
        }
        by_segment[packet.segment] = packet
        self._persist_packets(manifest, list(by_segment.values()))

    @_serialize_repo_storage
    def _commit_existing_packet_set(
        self,
        manifest: RepoObjectManifest,
        intent: RepoWriteIntent,
    ) -> RepoWriteReceipt:
        assert self._db is not None
        with self._db_lock:
            try:
                existing_receipt = self._ensure_write_intent_locked(intent)
                if existing_receipt is not None:
                    return existing_receipt
                self._validate_write_generation_locked(intent)
                rows = self._db.execute("""
                    SELECT r.ordinal, p.data_name, p.wire_size
                    FROM object_packet_refs AS r
                    JOIN data_packets AS p ON p.data_name=r.data_name
                    WHERE r.object_name=?
                    ORDER BY r.ordinal
                """, (manifest.object_name,)).fetchall()
                if len(rows) != manifest.segment_count:
                    raise ValueError(
                        f"repo-packet-set-incomplete: object={manifest.object_name} "
                        f"expected={manifest.segment_count} actual={len(rows)}")
                stored_names = tuple(str(row[1]) for row in rows)
                if manifest.packet_names and stored_names != manifest.packet_names:
                    raise ValueError(
                        f"{REPO_REASON_INTEGRITY_FAILURE}: packet index mismatch")
                quorum_finalized = intent.required_acks <= 1
                metadata = dict(manifest.metadata or {})
                metadata["quorumFinalized"] = quorum_finalized
                local_manifest = replace(
                    manifest,
                    lifecycle_state=(
                        "COMMITTED" if quorum_finalized else "RUNNING"),
                    confirmed_replica_nodes=(self.repo_node,),
                    metadata=metadata,
                )
                self._db.execute("""
                    UPDATE objects
                    SET manifest_json=?, sha256=?, object_type=?, updated_at=?
                    WHERE object_name=?
                """, (
                    json.dumps(local_manifest.to_dict(), sort_keys=True),
                    local_manifest.sha256,
                    local_manifest.object_type,
                    time.time(),
                    local_manifest.object_name,
                ))
                receipt = self._commit_write_receipt_locked(
                    intent, sum(int(row[2]) for row in rows))
                self._refresh_used_bytes_locked()
                self._db.commit()
            except Exception as exc:
                self._db.rollback()
                self._record_write_failure_locked(intent, str(exc))
                raise
        self._cache_invalidate(manifest.object_name)
        self._remember_catalog_change(
            local_manifest, self._catalog_state_for_manifest(local_manifest))
        return receipt

    @_coordinate_repo_object
    def _load_persisted_packets(self, object_name: str) -> tuple[RepoObjectManifest, list[DataPacket]]:
        cached = self._packet_cache_get(object_name)
        if cached is not None:
            return cached
        assert self._db is not None
        self._hot_cache.record_backing_read()
        with self._object_lock(object_name):
            db = self._reader_db()
            manifest_row = db.execute(
                "SELECT manifest_json FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
            rows = db.execute(
                """
                SELECT r.segment_no, p.data_name, p.wire
                FROM object_packet_refs AS r
                JOIN data_packets AS p ON p.data_name=r.data_name
                WHERE r.object_name=?
                ORDER BY r.ordinal ASC
                """,
                (object_name,),
            ).fetchall()
            if manifest_row is None or not rows:
                raise KeyError(object_name)
        manifest = RepoObjectManifest.from_dict(json.loads(str(manifest_row[0])))
        packets = [
            DataPacket(name=str(data_name), segment=int(segment_no), wire=bytes(wire))
            for segment_no, data_name, wire in rows
        ]
        if len(packets) >= manifest.segment_count:
            self._packet_cache_put_best_effort(manifest, packets)
        return manifest, packets

    @_coordinate_repo_object
    def _load_persisted_packet(self, data_name: str) -> DataPacket:
        cached = self._hot_cache.get_packet(data_name)
        self._cache_bytes = self._hot_cache.used_bytes
        if cached is not None:
            return cached
        assert self._db is not None
        self._hot_cache.record_backing_read()
        with self._object_lock(data_name):
            row = self._reader_db().execute(
                "SELECT wire, wire_sha256 FROM data_packets WHERE data_name=?",
                (data_name,),
            ).fetchone()
            if row is None:
                raise KeyError(data_name)
            wire = bytes(row[0])
            if hashlib.sha256(wire).hexdigest() != str(row[1]):
                raise ValueError(f"persisted repo Data wire hash mismatch: {data_name}")
        packet = decode_data_packet(wire)
        if packet.name != data_name:
            raise ValueError(
                f"persisted repo Data name/wire mismatch: key={data_name} "
                f"encoded={packet.name}")
        self._hot_cache.put_packet(packet)
        self._cache_bytes = self._hot_cache.used_bytes
        return packet

    @_coordinate_repo_object
    def _load_persisted_packet_prefix(self, data_name: str) -> list[DataPacket]:
        """Load the complete stored packet set for one exact versioned name."""

        versioned_prefix = (
            data_name.rsplit("/seg=", 1)[0]
            if "/seg=" in data_name else data_name
        )
        escaped = (versioned_prefix.replace("\\", "\\\\")
                   .replace("%", "\\%")
                   .replace("_", "\\_"))
        assert self._db is not None
        with self._object_lock(versioned_prefix):
            rows = self._reader_db().execute(
                """
                SELECT data_name, wire FROM data_packets
                WHERE data_name LIKE ? ESCAPE '\\'
                """,
                (escaped + "/%",),
            ).fetchall()
        packets = [decode_data_packet(bytes(wire)) for _, wire in rows]
        packets = [
            packet for packet in packets
            if packet.name == versioned_prefix or
            packet.name.startswith(versioned_prefix + "/")
        ]
        packets.sort(key=lambda packet: packet.segment)
        if not packets or not any(packet.name == data_name for packet in packets):
            raise KeyError(data_name)
        return packets

    @_coordinate_repo_object
    def _load_persisted_object(self, object_name: str) -> tuple[RepoObjectManifest, bytes]:
        cached = self._cache_get(object_name)
        if cached is not None:
            return cached
        assert self._db is not None
        self._hot_cache.record_backing_read()
        with self._object_lock(object_name):
            row = self._reader_db().execute(
                """
                SELECT manifest_json, payload
                FROM objects
                WHERE object_name=? AND payload IS NOT NULL
                """,
                (object_name,),
            ).fetchone()
            if row is None:
                raise KeyError(object_name)
        manifest = RepoObjectManifest.from_dict(json.loads(str(row[0])))
        payload = bytes(row[1])
        if hashlib.sha256(payload).hexdigest() != manifest.sha256:
            raise ValueError(f"persisted repo object hash mismatch: {object_name}")
        self._cache_put_best_effort(manifest, payload)
        return manifest, payload

    @_coordinate_repo_object
    def _load_persisted_for_fetch(
            self, object_name: str) -> tuple[str, RepoObjectManifest, object]:
        cached = self._hot_cache.get_any(object_name)
        self._cache_bytes = self._hot_cache.used_bytes
        if cached is not None:
            return cached

        assert self._db is not None
        self._hot_cache.record_backing_read()
        with self._object_lock(object_name):
            db = self._reader_db()
            row = db.execute(
                "SELECT manifest_json, payload FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
            packet_rows = db.execute(
                """
                SELECT r.segment_no, p.data_name, p.wire
                FROM object_packet_refs AS r
                JOIN data_packets AS p ON p.data_name=r.data_name
                WHERE r.object_name=?
                ORDER BY r.ordinal ASC
                """,
                (object_name,),
            ).fetchall()
            if row is None:
                raise KeyError(object_name)

        manifest = RepoObjectManifest.from_dict(json.loads(str(row[0])))
        if row[1] is not None:
            payload = bytes(row[1])
            if hashlib.sha256(payload).hexdigest() != manifest.sha256:
                raise ValueError(f"persisted repo object hash mismatch: {object_name}")
            self._cache_put_best_effort(manifest, payload)
            return "object", manifest, payload

        packets = [
            DataPacket(name=str(data_name), segment=int(segment_no), wire=bytes(wire))
            for segment_no, data_name, wire in packet_rows
        ]
        if not packets or len(packets) < manifest.segment_count:
            raise KeyError(object_name)
        self._packet_cache_put_best_effort(manifest, packets)
        return "packets", manifest, packets

    def _sqlite_has_manifest(self, object_name: str) -> bool:
        if self._db is None:
            return False
        with self._object_lock(object_name):
            row = self._reader_db().execute(
                "SELECT 1 FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
        return row is not None

    def _sqlite_has_object(self, object_name: str) -> bool:
        if self._db is None:
            return False
        with self._object_lock(object_name):
            row = self._reader_db().execute(
                """
                SELECT 1 FROM objects
                WHERE object_name=? AND
                  (payload IS NOT NULL OR EXISTS (
                    SELECT 1 FROM object_packet_refs
                    WHERE object_name=objects.object_name
                  ))
                """,
                (object_name,),
            ).fetchone()
        return row is not None

    def _sqlite_has_packet(self, data_name: str) -> bool:
        if self._db is None:
            return False
        with self._object_lock(data_name):
            row = self._reader_db().execute(
                "SELECT 1 FROM data_packets WHERE data_name=?",
                (data_name,),
            ).fetchone()
        return row is not None

    def _sqlite_inventory(self) -> dict[str, RepoObjectManifest]:
        if self._db is None:
            return {}
        with self._runtime_lock:
            rows = self._reader_db().execute(
                "SELECT object_name, manifest_json FROM objects"
            ).fetchall()
        return {
            str(name): RepoObjectManifest.from_dict(json.loads(str(manifest_json)))
            for name, manifest_json in rows
        }

    @staticmethod
    def _now_ms() -> int:
        return int(time.time() * 1000)

    @staticmethod
    def _manifest_sha256(manifest: RepoObjectManifest) -> str:
        return hashlib.sha256(manifest.to_bytes()).hexdigest()

    @staticmethod
    def _publisher_from_object_name(object_name: str) -> str:
        marker = "/NDNSF-DISTRIBUTED-REPO/"
        if marker in object_name:
            return object_name.split(marker, 1)[0]
        parts = object_name.strip("/").split("/")
        return "/" + "/".join(parts[:3]) if len(parts) >= 3 else object_name

    def _enforce_request_ownership(self, operation: str, request: dict,
                                   requester_identity: str) -> None:
        if not requester_identity:
            return
        protected_operations = {
            "STORE", "INSERT", "STORE_PACKETS", "STORE_PACKET",
            "STORE_PACKET_BATCH", "STORE_PACKET_PULL", "STORE_MANIFEST",
            "COMMIT_PACKET_SET", "FINALIZE_WRITE", "DELETE",
        }
        if operation not in protected_operations:
            return
        manifest = request.get("manifest", {})
        object_name = str(request.get("objectName", ""))
        if not object_name and isinstance(manifest, dict):
            object_name = str(manifest.get("objectName", ""))
        marker = "/NDNSF-DISTRIBUTED-REPO/"
        if marker not in object_name:
            return
        publisher = object_name.split(marker, 1)[0].rstrip("/")
        if publisher != requester_identity.rstrip("/"):
            authorization = request.get("repairAuthorization", {})
            manifest_sha256 = str(manifest.get("sha256", "")) if isinstance(
                manifest, dict) else ""
            if (operation == "STORE_PACKET_PULL" and
                    isinstance(authorization, dict) and
                    requester_identity.rstrip("/") == self.repo_node.rstrip("/") and
                    str(authorization.get("targetRepo", "")).rstrip("/") ==
                    self.repo_node.rstrip("/") and
                    str(authorization.get("objectName", "")) == object_name and
                    str(authorization.get("objectSha256", "")) == manifest_sha256):
                with self._db_lock:
                    row = self._db.execute("""
                        SELECT result_json FROM repair_jobs
                        WHERE object_name=? AND source_repo=? AND target_repo=?
                          AND state='RUNNING'
                        ORDER BY rowid DESC LIMIT 1
                    """, (
                        object_name,
                        str(authorization.get("sourceRepo", "")),
                        self.repo_node,
                    )).fetchone()
                if row is not None:
                    persisted = json.loads(str(row[0]) or "{}")
                    persisted_action = persisted.get("action", {})
                    if (isinstance(persisted_action, dict) and
                            str(persisted_action.get("objectSha256", "")) ==
                            manifest_sha256):
                        return
            raise PermissionError(
                "repo-publisher-ownership-mismatch: "
                f"requester={requester_identity} publisher={publisher}")

    @staticmethod
    def _catalog_manifest_summary(manifest: RepoObjectManifest) -> dict:
        # Catalog entries keep object/repo control-plane semantics here. Payload
        # transport sizing is handled by NDNSF core large-response references.
        manifest_dict = manifest.to_dict()
        return {
            "objectName": manifest.object_name,
            "objectType": manifest.object_type,
            "objectClass": manifest_dict.get("objectClass", ""),
            "publisher": RepoNodeApp._publisher_from_object_name(manifest.object_name),
            "sha256": manifest.sha256,
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "minReplicationFactor": manifest_dict.get("minReplicationFactor", 1),
            "maxReplicationFactor": manifest_dict.get("maxReplicationFactor", 1),
            "ttlMs": manifest_dict.get("ttlMs", 0),
            "repairAllowed": manifest_dict.get("repairAllowed", True),
            "autoDelete": manifest_dict.get("autoDelete", False),
            "deletePolicy": manifest_dict.get("deletePolicy", ""),
            "priority": manifest_dict.get("priority", 0),
            "replicationFactor": manifest.replication_factor,
            "replicaNodes": list(manifest.replica_nodes),
            "replicaDataNames": list(manifest.replica_data_names),
            "segmentLocations": list(manifest.segment_locations),
            "policyEpoch": manifest.policy_epoch,
            "metadata": dict(manifest_dict.get("metadata", {})),
            "queryTags": list(manifest_dict.get("queryTags", [])),
        }

    def _catalog_entry(self, manifest: RepoObjectManifest, state: str) -> dict:
        now_ms = self._now_ms()
        manifest_dict = manifest.to_dict()
        return {
            "objectName": manifest.object_name,
            "objectSha256": manifest.sha256,
            "manifestSha256": self._manifest_sha256(manifest),
            "objectType": manifest.object_type,
            "objectClass": manifest_dict.get("objectClass", ""),
            "publisher": self._publisher_from_object_name(manifest.object_name),
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "sourceRepo": self.repo_node,
            "repoMode": self.capability.repo_mode,
            "state": state,
            "catalogEpoch": self._catalog_epoch,
            "sourceBootId": self._catalog_boot_id,
            "sourceSequence": self._catalog_sequence,
            "generation": manifest.generation,
            "lastSeenMs": now_ms,
            "updatedAtMs": now_ms,
            "minReplicationFactor": manifest_dict.get("minReplicationFactor", 1),
            "maxReplicationFactor": manifest_dict.get("maxReplicationFactor", 1),
            "desiredReplicationFactor": manifest_dict.get("minReplicationFactor", 1),
            "ttlMs": manifest_dict.get("ttlMs", 0),
            "repairAllowed": manifest_dict.get("repairAllowed", True),
            "autoDelete": manifest_dict.get("autoDelete", False),
            "deletePolicy": manifest_dict.get("deletePolicy", ""),
            "priority": manifest_dict.get("priority", 0),
            "replicaNodes": list(manifest.replica_nodes),
            "metadata": dict(manifest_dict.get("metadata", {})),
            "queryTags": list(manifest_dict.get("queryTags", [])),
            "manifest": self._catalog_manifest_summary(manifest),
        }

    def _catalog_status_entry(self) -> dict:
        capability = self._capability()
        return {
            "repoNode": self.repo_node,
            "repoMode": capability.repo_mode,
            "catalogEpoch": self._catalog_epoch,
            "bootId": self._catalog_boot_id,
            "sourceSequence": self._catalog_sequence,
            "lastSeenMs": self._now_ms(),
            "acceptsBackupReplica": capability.accepts_backup_replica,
            "freeBytes": capability.free_bytes,
            "usedBytes": capability.used_bytes,
            "failureDomain": capability.failure_domain,
        }

    def _merge_repo_status(self, status: dict) -> None:
        repo_node = str(status.get("repoNode", ""))
        if not repo_node:
            return
        merged = dict(status)
        merged["lastSeenMs"] = self._now_ms()
        self._repo_status[repo_node] = merged
        if self._db is not None:
            with self._db_lock:
                self._db.execute("""
                    INSERT INTO repo_membership
                      (repo_node, boot_id, last_sequence, last_seen_ms, status_json)
                    VALUES (?, ?, ?, ?, ?)
                    ON CONFLICT(repo_node) DO UPDATE SET
                      boot_id=excluded.boot_id,
                      last_sequence=MAX(repo_membership.last_sequence,
                                        excluded.last_sequence),
                      last_seen_ms=excluded.last_seen_ms,
                      status_json=excluded.status_json
                """, (
                    repo_node, str(merged.get("bootId", "")),
                    int(merged.get("sourceSequence", merged.get("catalogEpoch", 0)) or 0),
                    int(merged["lastSeenMs"]),
                    json.dumps(merged, sort_keys=True),
                ))
                self._db.commit()

    @staticmethod
    def _catalog_order_key(entry: dict) -> tuple[int, str, int]:
        return (
            int(entry.get("generation", 0) or 0),
            str(entry.get("sourceBootId", "")),
            int(entry.get("sourceSequence", entry.get("catalogEpoch", 0)) or 0),
        )

    def _upsert_catalog_entry(self, entry: dict) -> None:
        object_name = str(entry.get("objectName", ""))
        source_repo = str(entry.get("sourceRepo", ""))
        if not object_name or not source_repo:
            return
        normalized = dict(entry)
        normalized.setdefault("sourceBootId", "legacy")
        normalized.setdefault(
            "sourceSequence", int(normalized.get("catalogEpoch", 0) or 0))
        normalized.setdefault("generation", int(
            normalized.get("manifest", {}).get("generation", 0)
            if isinstance(normalized.get("manifest", {}), dict) else 0))
        normalized.setdefault("objectSha256", normalized.get("manifestSha256", ""))
        if "manifestSha256" not in normalized and "manifest" in normalized:
            try:
                normalized["manifestSha256"] = hashlib.sha256(
                    json.dumps(normalized["manifest"], sort_keys=True).encode()
                ).hexdigest()
            except Exception:
                normalized["manifestSha256"] = ""
        normalized["lastSeenMs"] = self._now_ms()
        by_source = self._global_catalog.setdefault(object_name, {})
        current = by_source.get(source_repo)
        if current is not None:
            current_epoch = int(current.get("sourceSequence", current.get("catalogEpoch", 0)))
            entry_epoch = int(normalized.get("sourceSequence", normalized.get("catalogEpoch", 0)))
            current_updated_ms = int(current.get("updatedAtMs", 0) or 0)
            entry_updated_ms = int(normalized.get("updatedAtMs", 0) or 0)
            incoming_tombstone_overrides_available = (
                str(current.get("state", "")) == "AVAILABLE" and
                str(normalized.get("state", "")) == "DELETED" and
                entry_updated_ms >= current_updated_ms
            )
            if (str(current.get("state", "")) == "DELETED" and
                    str(normalized.get("state", "")) == "AVAILABLE" and
                    current_updated_ms >= entry_updated_ms):
                return
            same_boot = str(current.get("sourceBootId", "legacy")) == str(
                normalized.get("sourceBootId", "legacy"))
            if same_boot and current_epoch > entry_epoch and not incoming_tombstone_overrides_available:
                return
            if (same_boot and current_epoch == entry_epoch and
                    current_updated_ms > entry_updated_ms and
                    not incoming_tombstone_overrides_available):
                return
        by_source[source_repo] = normalized
        self._merge_repo_status({
            "repoNode": source_repo,
            "repoMode": normalized.get("repoMode", ""),
            "catalogEpoch": normalized.get("catalogEpoch", 0),
            "bootId": normalized.get("sourceBootId", ""),
            "sourceSequence": normalized.get("sourceSequence", 0),
        })

    def _persist_catalog_entry_locked(self, entry: dict) -> None:
        assert self._db is not None
        source_repo = str(entry.get("sourceRepo", ""))
        boot_id = str(entry.get("sourceBootId", "legacy"))
        sequence = int(entry.get("sourceSequence", entry.get("catalogEpoch", 0)) or 0)
        self._db.execute("""
            INSERT OR IGNORE INTO catalog_journal
              (source_repo, source_boot_id, source_sequence, object_name,
               generation, state, digest, entry_json)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            source_repo, boot_id, sequence, str(entry.get("objectName", "")),
            int(entry.get("generation", 0) or 0), str(entry.get("state", "")),
            str(entry.get("objectSha256", "")), json.dumps(entry, sort_keys=True),
        ))
        if str(entry.get("state", "")) == "DELETED":
            self._db.execute("""
                INSERT INTO catalog_tombstones
                  (object_name, generation, source_repo, source_boot_id,
                   source_sequence, entry_json)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(object_name, generation) DO UPDATE SET
                  source_repo=excluded.source_repo,
                  source_boot_id=excluded.source_boot_id,
                  source_sequence=excluded.source_sequence,
                  entry_json=excluded.entry_json
                WHERE excluded.source_sequence >= catalog_tombstones.source_sequence
            """, (
                str(entry.get("objectName", "")), int(entry.get("generation", 0) or 0),
                source_repo, boot_id, sequence, json.dumps(entry, sort_keys=True),
            ))

    def _compact_catalog_locked(self) -> None:
        assert self._db is not None
        self._db.execute("""
            DELETE FROM catalog_journal WHERE rowid IN (
              SELECT rowid FROM catalog_journal
              ORDER BY source_sequence DESC LIMIT -1 OFFSET ?
            )
        """, (self._catalog_history_limit,))

    def _restore_catalog_state(self) -> None:
        assert self._db is not None
        with self._db_lock:
            sequence_row = self._db.execute(
                "SELECT value FROM repo_meta WHERE key='catalog_sequence'"
            ).fetchone()
            self._catalog_sequence = int(sequence_row[0]) if sequence_row else 0
            self._catalog_epoch = self._catalog_sequence
            self._db.execute("""
                INSERT INTO repo_meta(key, value) VALUES('catalog_boot_id', ?)
                ON CONFLICT(key) DO UPDATE SET value=excluded.value
            """, (self._catalog_boot_id,))
            journal_rows = self._db.execute("""
                SELECT entry_json FROM catalog_journal
                ORDER BY source_sequence
            """).fetchall()
            membership_rows = self._db.execute(
                "SELECT status_json FROM repo_membership"
            ).fetchall()
            watermark_rows = self._db.execute(
                "SELECT peer_repo, source_sequence FROM peer_watermarks"
            ).fetchall()
            self._db.commit()
        with self._catalog_lock:
            for row in journal_rows:
                entry = json.loads(str(row[0]))
                self._upsert_catalog_entry(entry)
                if str(entry.get("sourceRepo", "")) == self.repo_node:
                    self._catalog_changes.append(entry)
            for row in membership_rows:
                status = json.loads(str(row[0]))
                self._repo_status[str(status.get("repoNode", ""))] = status
            self._peer_catalog_epochs.update({
                str(peer): int(sequence) for peer, sequence in watermark_rows
            })

    def _flatten_catalog_entries(self) -> list[dict]:
        entries: list[dict] = []
        for by_source in self._global_catalog.values():
            entries.extend(dict(entry) for entry in by_source.values())
        return entries

    def _refresh_local_catalog_liveness_locked(self) -> None:
        """Refresh this repo's liveness without changing object semantics.

        Catalog metadata such as updatedAtMs, ttlMs, repairAllowed, and
        replication factors describes the object/control-plane policy.  A repo
        heartbeat should only prove that this source is still live; it must not
        silently extend object TTLs or overwrite policy imported from catalog
        gossip.
        """
        now_ms = self._now_ms()
        for by_source in self._global_catalog.values():
            entry = by_source.get(self.repo_node)
            if entry is not None:
                entry["lastSeenMs"] = now_ms
        self._merge_repo_status(self._catalog_status_entry())

    def _is_entry_stale(self, entry: dict) -> bool:
        source_repo = str(entry.get("sourceRepo", ""))
        status = self._repo_status.get(source_repo, {})
        last_seen = int(status.get("lastSeenMs", entry.get("lastSeenMs", 0)) or 0)
        return bool(last_seen and self._now_ms() - last_seen > self._catalog_stale_after_ms)

    def _is_repo_status_stale(self, status: dict) -> bool:
        last_seen = int(status.get("lastSeenMs", 0) or 0)
        return bool(last_seen and self._now_ms() - last_seen > self._catalog_stale_after_ms)

    def _object_catalog_summary(self, object_name: str) -> dict:
        with self._catalog_lock:
            entries = [
                dict(entry)
                for entry in self._global_catalog.get(object_name, {}).values()
            ]
            repo_status = {
                repo: dict(status)
                for repo, status in self._repo_status.items()
            }
            repo_status[self.repo_node] = self._catalog_status_entry()
        if not entries:
            raise KeyError(object_name)
        for entry in entries:
            entry["stale"] = self._is_entry_stale(entry)
            ttl_ms = int(entry.get(
                "ttlMs",
                entry.get("manifest", {}).get("ttlMs", 0),
            ) or 0)
            updated_ms = int(entry.get("updatedAtMs", 0) or 0)
            entry["expired"] = (
                str(entry.get("state", "")) == "AVAILABLE" and
                ttl_ms > 0 and
                updated_ms > 0 and
                self._now_ms() - updated_ms > ttl_ms
            )
        tombstone_cutoff_ms = max(
            [
                int(entry.get("updatedAtMs", 0) or 0)
                for entry in entries
                if str(entry.get("state", "")) == "DELETED"
            ] or [0]
        )
        for entry in entries:
            entry_updated_ms = int(entry.get("updatedAtMs", 0) or 0)
            entry["shadowedByTombstone"] = (
                str(entry.get("state", "")) == "AVAILABLE" and
                tombstone_cutoff_ms > 0 and
                entry_updated_ms <= tombstone_cutoff_ms
            )
        available = [
            entry for entry in entries
            if (str(entry.get("state", "")) == "AVAILABLE" and
                not entry.get("stale") and
                not entry.get("expired") and
                not entry.get("shadowedByTombstone"))
        ]
        staged_entries = [
            entry for entry in entries
            if str(entry.get("state", "")) == "STAGED" and
            not entry.get("stale")
        ]
        staged_only = bool(staged_entries and not available)
        max_generation = max(
            [int(entry.get("generation", 0) or 0) for entry in available] or [0])
        live_generation = [
            entry for entry in available
            if int(entry.get("generation", 0) or 0) == max_generation
        ]
        live_digests = {
            str(entry.get("objectSha256", "")) for entry in live_generation
            if str(entry.get("objectSha256", ""))
        }
        conflicted = len(live_digests) > 1
        tombstones = [
            entry for entry in entries
            if str(entry.get("state", "")) == "DELETED"
        ]
        deleted = bool(tombstones and not available)
        min_required = max(
            [
                int(entry.get(
                    "minReplicationFactor",
                    entry.get("desiredReplicationFactor",
                              entry.get("manifest", {}).get("minReplicationFactor", 1)),
                ) or 1)
                for entry in entries
            ] or [1]
        )
        max_allowed = max(
            [
                int(entry.get(
                    "maxReplicationFactor",
                    entry.get("manifest", {}).get(
                        "maxReplicationFactor",
                        entry.get("manifest", {}).get(
                            "replicationFactor", min_required),
                    ),
                ) or min_required)
                for entry in entries
            ] or [min_required]
        )
        source_repos = {str(entry.get("sourceRepo", "")) for entry in entries}
        repair_allowed = any(
            bool(entry.get(
                "repairAllowed",
                entry.get("manifest", {}).get("repairAllowed", True),
            ))
            for entry in entries
        )
        stale_repos = [
            str(entry.get("sourceRepo", ""))
            for entry in entries
            if entry.get("stale")
        ]
        expired_repos = [
            str(entry.get("sourceRepo", ""))
            for entry in entries
            if entry.get("expired")
        ]
        expired = bool(expired_repos and not available)
        eligible_for_repair = bool(
            repair_allowed and not expired and not deleted and not conflicted and
            not staged_only)
        target_candidates = [
            repo for repo, status in repo_status.items()
            if repo not in source_repos and
            str(status.get("repoMode", "")) == "persistent" and
            bool(status.get("acceptsBackupReplica", True)) and
            not self._is_repo_status_stale(status)
        ]
        missing = (
            0 if not eligible_for_repair else
            max(0, min_required - len(available))
        )
        repair_actions = []
        source_repo = str(available[0].get("sourceRepo", "")) if available else ""
        if source_repo:
            for target_repo in target_candidates[:missing]:
                repair_actions.append(RepoRepairAction(
                    object_name=object_name,
                    object_sha256=str(available[0].get("objectSha256", "")),
                    manifest_sha256=str(available[0].get("manifestSha256", "")),
                    min_replication_factor=min_required,
                    max_replication_factor=max_allowed,
                    source_repo=source_repo,
                    target_repo=target_repo,
                    reason="under-replicated",
                ).to_dict())
        if staged_only:
            repair_reason = "not-finalized"
        elif expired:
            repair_reason = "expired"
        elif not repair_allowed:
            repair_reason = "repair-disabled"
        elif missing and not source_repo:
            repair_reason = "no-live-source"
        elif missing and len(repair_actions) < missing:
            repair_reason = "insufficient-live-targets"
        elif missing:
            repair_reason = "ready"
        else:
            repair_reason = "not-needed"
        repair_plan = {
            "needed": missing > 0,
            "missingReplicas": missing,
            "sourceRepos": [str(entry.get("sourceRepo", "")) for entry in available],
            "targetCandidates": target_candidates[:missing],
            "actions": repair_actions,
            "reason": repair_reason,
        }
        best = sorted(
            (live_generation or available or entries),
            key=lambda item: (
                -int(item.get("generation", 0) or 0),
                str(item.get("sourceRepo", "")),
            ),
        )[0]
        manifest_dict = (
            dict(best.get("manifest", {}))
            if isinstance(best.get("manifest", {}), dict) else {}
        )
        metadata = (
            dict(best.get("metadata", {}))
            if isinstance(best.get("metadata", {}), dict) else
            dict(manifest_dict.get("metadata", {}))
            if isinstance(manifest_dict.get("metadata", {}), dict) else {}
        )
        query_tags = (
            [str(value) for value in best.get("queryTags", [])]
            if isinstance(best.get("queryTags", []), list) else
            [str(value) for value in manifest_dict.get("queryTags", [])]
            if isinstance(manifest_dict.get("queryTags", []), list) else []
        )
        created_at_ms = int(best.get(
            "createdAtMs",
            manifest_dict.get("createdAtMs", best.get("updatedAtMs", 0)),
        ) or 0)
        updated_at_ms = int(best.get(
            "updatedAtMs",
            manifest_dict.get("updatedAtMs", 0),
        ) or 0)
        return {
            "objectName": object_name,
            "objectSha256": best.get("objectSha256", ""),
            "manifestSha256": best.get("manifestSha256", ""),
            "objectType": best.get("objectType", ""),
            "objectClass": best.get("objectClass", best.get("manifest", {}).get("objectClass", "")),
            "publisher": best.get("publisher", self._publisher_from_object_name(object_name)),
            "createdAtMs": created_at_ms,
            "updatedAtMs": updated_at_ms,
            "size": best.get("size", 0),
            "segmentCount": best.get("segmentCount", 0),
            "metadata": metadata,
            "queryTags": query_tags,
            "state": ("CONFLICT" if conflicted else
                      "DELETED" if deleted else
                      "EXPIRED" if expired else
                      "STAGED" if staged_only else
                      "UNDER_REPLICATED" if missing else "AVAILABLE"),
            "minReplicationFactor": min_required,
            "maxReplicationFactor": max_allowed,
            "desiredReplicationFactor": min_required,
            "ttlMs": best.get("ttlMs", best.get("manifest", {}).get("ttlMs", 0)),
            "repairAllowed": repair_allowed,
            "autoDelete": best.get(
                "autoDelete",
                best.get("manifest", {}).get("autoDelete", False),
            ),
            "deletePolicy": best.get(
                "deletePolicy",
                best.get("manifest", {}).get("deletePolicy", ""),
            ),
            "priority": best.get(
                "priority",
                best.get("manifest", {}).get("priority", 0),
            ),
            "expired": expired,
            "expiredRepos": expired_repos,
            "eligibleForRepair": eligible_for_repair,
            "conflicted": conflicted,
            "conflictingDigests": sorted(live_digests) if conflicted else [],
            "availableReplicaCount": len(available),
            "underReplicated": missing > 0,
            "staleRepos": stale_repos,
            "entries": entries,
            "candidateReplicas": available,
            "repairPlan": repair_plan,
        }

    def _remember_catalog_change(self, manifest: RepoObjectManifest, state: str) -> None:
        with self._catalog_lock:
            self._catalog_sequence += 1
            self._catalog_epoch = self._catalog_sequence
            entry = self._catalog_entry(manifest, state)
            entry["catalogEpoch"] = self._catalog_epoch
            entry["sourceSequence"] = self._catalog_sequence
            self._catalog_changes.append(entry)
            self._upsert_catalog_entry(entry)
            with self._db_lock:
                self._persist_catalog_entry_locked(entry)
                self._db.execute("""
                    INSERT INTO repo_meta(key, value) VALUES('catalog_sequence', ?)
                    ON CONFLICT(key) DO UPDATE SET value=excluded.value
                """, (str(self._catalog_sequence),))
                self._compact_catalog_locked()
                self._db.commit()

    def _catalog_snapshot(self) -> dict:
        inventory = self._sqlite_inventory()
        with self._catalog_lock:
            for manifest in inventory.values():
                self._upsert_catalog_entry(self._catalog_entry(
                    manifest, self._catalog_state_for_manifest(manifest)))
            self._refresh_local_catalog_liveness_locked()
            entries = self._flatten_catalog_entries()
            object_names = sorted({str(entry.get("objectName", "")) for entry in entries})
            objects = []
            for object_name in object_names:
                if not object_name:
                    continue
                try:
                    objects.append(self._object_catalog_summary(object_name))
                except KeyError:
                    pass
            return {
                "repoNode": self.repo_node,
                "repoMode": self.capability.repo_mode,
                "catalogEpoch": self._catalog_epoch,
                "generatedAtMs": self._now_ms(),
                "staleAfterMs": self._catalog_stale_after_ms,
                "repoStatus": self._catalog_status_entry(),
                "knownRepos": list(self._repo_status.values()),
                "entries": entries,
                "objects": objects,
            }

    def _catalog_delta(self, since_epoch: int) -> dict:
        with self._catalog_lock:
            self._refresh_local_catalog_liveness_locked()
            return {
                "repoNode": self.repo_node,
                "repoMode": self.capability.repo_mode,
                "sinceEpoch": since_epoch,
                "catalogEpoch": self._catalog_epoch,
                "generatedAtMs": self._now_ms(),
                "staleAfterMs": self._catalog_stale_after_ms,
                "repoStatus": self._catalog_status_entry(),
                "entries": [
                    entry for entry in self._catalog_changes
                    if int(entry.get("catalogEpoch", 0)) > since_epoch
                ],
            }

    def _catalog_bucket_digest(self, bucket_count: int = 64) -> dict:
        bucket_count = max(1, min(1024, int(bucket_count)))
        buckets: list[list[str]] = [[] for _ in range(bucket_count)]
        with self._catalog_lock:
            entries = self._flatten_catalog_entries()
        for entry in entries:
            canonical = json.dumps(entry, sort_keys=True, separators=(",", ":"))
            bucket = int(hashlib.sha256(
                str(entry.get("objectName", "")).encode()).hexdigest(), 16) % bucket_count
            buckets[bucket].append(canonical)
        return {
            "bucketCount": bucket_count,
            "digests": [
                hashlib.sha256("\n".join(sorted(items)).encode()).hexdigest()
                for items in buckets
            ],
            "catalogEpoch": self._catalog_epoch,
            "repoStatus": self._catalog_status_entry(),
        }

    def _catalog_bucket_entries(self, bucket: int, bucket_count: int = 64) -> dict:
        bucket_count = max(1, min(1024, int(bucket_count)))
        bucket = int(bucket)
        if bucket < 0 or bucket >= bucket_count:
            raise ValueError("catalog bucket is outside bucketCount")
        with self._catalog_lock:
            entries = [
                entry for entry in self._flatten_catalog_entries()
                if int(hashlib.sha256(
                    str(entry.get("objectName", "")).encode()).hexdigest(), 16) %
                bucket_count == bucket
            ]
        return {
            "bucket": bucket,
            "bucketCount": bucket_count,
            "entries": entries,
            "repoStatus": self._catalog_status_entry(),
        }

    @staticmethod
    def _repair_job_dict(row: sqlite3.Row | tuple) -> dict:
        result = json.loads(str(row[10])) if str(row[10]) else {}
        return {
            "repairId": str(row[0]),
            "objectName": str(row[1]),
            "generation": int(row[2]),
            "sourceRepo": str(row[3]),
            "targetRepo": str(row[4]),
            "state": str(row[5]),
            "attempts": int(row[6]),
            "nextAttemptMs": int(row[7]),
            "leaseOwner": str(row[8]),
            "leaseDeadlineMs": int(row[9]),
            "result": result,
            "action": dict(result.get("action", {})),
            "availableReplicas": int(row[11]),
            "missingReplicas": int(row[12]),
            "objectPriority": int(row[13]),
            "objectUpdatedAtMs": int(row[14]),
        }

    def _scan_repair_jobs(self) -> dict:
        with self._catalog_lock:
            object_names = sorted(self._global_catalog)
        created: list[str] = []
        now_ms = self._now_ms()
        with self._db_lock:
            self._db.execute("""
                UPDATE repair_jobs SET state='RETRY', lease_owner='',
                  lease_deadline_ms=0, next_attempt_ms=?
                WHERE state='RUNNING' AND lease_deadline_ms>0 AND lease_deadline_ms<=?
            """, (now_ms, now_ms))
            for object_name in object_names:
                try:
                    summary = self._object_catalog_summary(object_name)
                except KeyError:
                    continue
                plan = summary.get("repairPlan", {})
                for action in plan.get("actions", []) if isinstance(plan, dict) else []:
                    if not isinstance(action, dict):
                        continue
                    generation = max([
                        int(entry.get("generation", 0) or 0)
                        for entry in summary.get("candidateReplicas", [])
                    ] or [0])
                    source_sequence = max([
                        int(entry.get("sourceSequence", entry.get("catalogEpoch", 0)) or 0)
                        for entry in summary.get("entries", [])
                    ] or [self._catalog_sequence])
                    identity = "|".join((
                        object_name, str(generation), str(source_sequence),
                        str(action.get("sourceRepo", "")),
                        str(action.get("targetRepo", "")),
                    ))
                    repair_id = hashlib.sha256(identity.encode()).hexdigest()
                    available_replicas = int(
                        summary.get("availableReplicaCount", 0) or 0)
                    missing_replicas = int(
                        plan.get("missingReplicas", 1) or 1)
                    object_priority = int(summary.get("priority", 0) or 0)
                    object_updated_at_ms = int(
                        summary.get("updatedAtMs", 0) or 0)
                    result = {
                        "action": action,
                        "createdAtMs": now_ms,
                        "scheduling": {
                            "availableReplicas": available_replicas,
                            "missingReplicas": missing_replicas,
                            "objectPriority": object_priority,
                            "objectUpdatedAtMs": object_updated_at_ms,
                        },
                    }
                    cursor = self._db.execute("""
                        INSERT OR IGNORE INTO repair_jobs
                          (repair_id, object_name, generation, source_repo,
                           target_repo, state, attempts, next_attempt_ms,
                           lease_owner, lease_deadline_ms, result_json,
                           available_replicas, missing_replicas,
                           object_priority, object_updated_at_ms)
                        VALUES (?, ?, ?, ?, ?, 'PENDING', 0, ?, '', 0, ?,
                                ?, ?, ?, ?)
                    """, (
                        repair_id, object_name, generation,
                        str(action.get("sourceRepo", "")),
                        str(action.get("targetRepo", "")), now_ms,
                        json.dumps(result, sort_keys=True),
                        available_replicas, missing_replicas,
                        object_priority, object_updated_at_ms,
                    ))
                    if cursor.rowcount > 0:
                        created.append(repair_id)
                    else:
                        self._db.execute("""
                            UPDATE repair_jobs
                            SET available_replicas=?, missing_replicas=?,
                                object_priority=?, object_updated_at_ms=?
                            WHERE repair_id=? AND state IN ('PENDING', 'RETRY')
                        """, (
                            available_replicas, missing_replicas,
                            object_priority, object_updated_at_ms, repair_id,
                        ))
            state_rows = self._db.execute("""
                SELECT state, COUNT(*) FROM repair_jobs GROUP BY state
            """).fetchall()
            claimable_count = int(self._db.execute("""
                SELECT COUNT(*) FROM repair_jobs
                WHERE target_repo=? AND state IN ('PENDING', 'RETRY')
                  AND next_attempt_ms<=?
            """, (self.repo_node, now_ms)).fetchone()[0])
            earliest_retry_row = self._db.execute("""
                SELECT MIN(next_attempt_ms) FROM repair_jobs
                WHERE target_repo=? AND state IN ('PENDING', 'RETRY')
                  AND next_attempt_ms>?
            """, (self.repo_node, now_ms)).fetchone()
            self._db.commit()
        state_counts = {str(state): int(count) for state, count in state_rows}
        return {
            "created": created,
            "createdCount": len(created),
            "jobCount": sum(state_counts.values()),
            "stateCounts": state_counts,
            "claimableCount": claimable_count,
            "targetRepo": self.repo_node,
            "earliestRetryMs": (
                int(earliest_retry_row[0])
                if earliest_retry_row and earliest_retry_row[0] is not None
                else None
            ),
        }

    def _claim_repair_job(self, lease_owner: str,
                          lease_ms: int = 30_000,
                          target_repo: str = "") -> Optional[dict]:
        now_ms = self._now_ms()
        owner = lease_owner or self.repo_node
        with self._db_lock:
            self._db.execute("""
                UPDATE repair_jobs SET state='RETRY', lease_owner='',
                  lease_deadline_ms=0, next_attempt_ms=?
                WHERE state='RUNNING' AND lease_deadline_ms>0 AND lease_deadline_ms<=?
            """, (now_ms, now_ms))
            row = self._db.execute("""
                SELECT repair_id, object_name, generation, source_repo,
                       target_repo, state, attempts, next_attempt_ms,
                       lease_owner, lease_deadline_ms, result_json,
                       available_replicas, missing_replicas,
                       object_priority, object_updated_at_ms
                FROM repair_jobs
                WHERE target_repo=? AND state IN ('PENDING', 'RETRY')
                  AND next_attempt_ms<=?
                ORDER BY available_replicas ASC,
                         object_priority DESC,
                         object_updated_at_ms ASC,
                         missing_replicas DESC,
                         attempts ASC,
                         repair_id ASC
                LIMIT 1
            """, (target_repo or self.repo_node, now_ms)).fetchone()
            if row is None:
                self._db.commit()
                return None
            deadline = now_ms + max(1000, int(lease_ms))
            self._db.execute("""
                UPDATE repair_jobs SET state='RUNNING', lease_owner=?,
                  lease_deadline_ms=?, attempts=attempts+1
                WHERE repair_id=?
            """, (owner, deadline, str(row[0])))
            self._db.commit()
            updated = list(row)
            updated[5] = "RUNNING"
            updated[6] = int(updated[6]) + 1
            updated[8] = owner
            updated[9] = deadline
            return self._repair_job_dict(tuple(updated))

    def _finish_repair_job(self, repair_id: str, *, success: bool,
                           result: Optional[dict] = None,
                           error: str = "") -> dict:
        with self._db_lock:
            row = self._db.execute(
                "SELECT attempts, result_json FROM repair_jobs WHERE repair_id=?",
                (repair_id,),
            ).fetchone()
            if row is None:
                raise KeyError(repair_id)
            attempts = int(row[0])
            payload = json.loads(str(row[1]) or "{}")
            payload.update(result or {})
            if error:
                payload["error"] = error
            state = "COMPLETED" if success else "RETRY"
            next_attempt_ms = 0 if success else (
                self._now_ms() + min(60_000, 500 * (2 ** min(attempts, 7))))
            self._db.execute("""
                UPDATE repair_jobs SET state=?, next_attempt_ms=?,
                  lease_owner='', lease_deadline_ms=0, result_json=?
                WHERE repair_id=?
            """, (state, next_attempt_ms, json.dumps(payload, sort_keys=True), repair_id))
            self._db.commit()
        return {"repairId": repair_id, "state": state,
                "nextAttemptMs": next_attempt_ms, "result": payload}

    def _scrub(self, limit: int = 100) -> dict:
        checked = 0
        corrupt: list[str] = []
        inventory = self._sqlite_inventory()
        for object_name in sorted(inventory)[:max(1, min(10_000, int(limit)))]:
            checked += 1
            try:
                kind, manifest, value = self._load_persisted_for_fetch(object_name)
                if kind == "object":
                    if hashlib.sha256(bytes(value)).hexdigest() != manifest.sha256:
                        corrupt.append(object_name)
                else:
                    for packet in value:
                        decoded = decode_data_packet(packet.wire)
                        if decoded.name != packet.name:
                            raise ValueError("wire/name mismatch")
            except Exception:
                corrupt.append(object_name)
        return {"checked": checked, "corrupt": corrupt,
                "corruptCount": len(corrupt)}

    def _merge_catalog_entries(self, entries: Iterable[dict],
                               source_status: Optional[dict] = None) -> None:
        with self._catalog_lock:
            source_repo = ""
            if source_status:
                self._merge_repo_status(source_status)
                source_repo = str(source_status.get("repoNode", ""))
            for raw_entry in entries:
                entry = dict(raw_entry)
                if source_repo == self.repo_node:
                    self._catalog_epoch += 1
                    entry["catalogEpoch"] = self._catalog_epoch
                    entry.setdefault("updatedAtMs", self._now_ms())
                    self._catalog_changes.append(dict(entry))
                self._upsert_catalog_entry(entry)
                if self._db is not None:
                    with self._db_lock:
                        self._persist_catalog_entry_locked(entry)
                        self._db.commit()

    def _catalog_lookup(self, object_name: str) -> dict:
        with self._catalog_lock:
            has_local_entry = self.repo_node in self._global_catalog.get(object_name, {})
            if has_local_entry:
                self._refresh_local_catalog_liveness_locked()
        if not has_local_entry:
            try:
                manifest = self._load_manifest(object_name)
                self._upsert_catalog_entry(self._catalog_entry(manifest, "AVAILABLE"))
            except KeyError:
                pass
        return self._object_catalog_summary(object_name)

    @staticmethod
    def _metadata_matches(metadata: dict, required: dict) -> bool:
        for key, expected in required.items():
            actual = metadata.get(key)
            if isinstance(expected, list):
                if actual not in expected:
                    return False
            elif actual != expected:
                return False
        return True

    @staticmethod
    def _tags_match(tags: list[str], required_tags: list[str]) -> bool:
        if not required_tags:
            return True
        tag_set = {str(tag) for tag in tags}
        return all(str(tag) in tag_set for tag in required_tags)

    def _catalog_query(self, query: dict) -> dict:
        if not isinstance(query, dict):
            raise ValueError("CATALOG_QUERY query must be an object")
        with self._catalog_lock:
            self._refresh_local_catalog_liveness_locked()
            object_names = sorted(self._global_catalog)
        object_class = str(query.get("objectClass", ""))
        object_type = str(query.get("objectType", ""))
        publisher = str(query.get("publisher", ""))
        state = str(query.get("state", ""))
        required_tags = [
            str(value) for value in query.get("tags", query.get("queryTags", []))
        ] if isinstance(query.get("tags", query.get("queryTags", [])), list) else []
        required_metadata = (
            dict(query.get("metadata", {}))
            if isinstance(query.get("metadata", {}), dict) else {}
        )
        created_after_ms = int(query.get("createdAfterMs", 0) or 0)
        created_before_ms = int(query.get("createdBeforeMs", 0) or 0)
        updated_after_ms = int(query.get("updatedAfterMs", 0) or 0)
        updated_before_ms = int(query.get("updatedBeforeMs", 0) or 0)
        limit = int(query.get("limit", 0) or 0)
        results = []
        for object_name in object_names:
            try:
                summary = self._object_catalog_summary(object_name)
            except KeyError:
                continue
            metadata = (
                dict(summary.get("metadata", {}))
                if isinstance(summary.get("metadata", {}), dict) else {}
            )
            tags = [
                str(value) for value in summary.get("queryTags", [])
            ] if isinstance(summary.get("queryTags", []), list) else []
            if object_class and str(summary.get("objectClass", "")) != object_class:
                continue
            if object_type and str(summary.get("objectType", "")) != object_type:
                continue
            if publisher and str(summary.get("publisher", "")) != publisher:
                continue
            if state and str(summary.get("state", "")) != state:
                continue
            if not self._tags_match(tags, required_tags):
                continue
            if required_metadata and not self._metadata_matches(metadata, required_metadata):
                continue
            created_at_ms = int(summary.get("createdAtMs", summary.get("updatedAtMs", 0)) or 0)
            updated_at_ms = int(summary.get("updatedAtMs", 0) or 0)
            if created_after_ms and created_at_ms < created_after_ms:
                continue
            if created_before_ms and created_at_ms > created_before_ms:
                continue
            if updated_after_ms and updated_at_ms < updated_after_ms:
                continue
            if updated_before_ms and updated_at_ms > updated_before_ms:
                continue
            results.append(summary)
            if limit > 0 and len(results) >= limit:
                break
        return {
            "repoNode": self.repo_node,
            "query": dict(query),
            "count": len(results),
            "objects": results,
        }

    def _sqlite_payload_bytes(self, object_name: str) -> bytes:
        _, payload = self._load_persisted_object(object_name)
        return payload

    @_serialize_repo_storage
    def _delete_object(self, object_name: str) -> bool:
        deleted_manifest: RepoObjectManifest | None = None
        try:
            deleted_manifest = self._load_manifest(object_name)
        except KeyError:
            pass
        assert self._db is not None
        with self._db_lock:
            try:
                old_packet_names = [
                    str(row[0]) for row in self._db.execute(
                        "SELECT data_name FROM object_packet_refs WHERE object_name=?",
                        (object_name,),
                    ).fetchall()
                ]
                self._db.execute(
                    "DELETE FROM object_packet_refs WHERE object_name=?",
                    (object_name,),
                )
                cursor = self._db.execute(
                    "DELETE FROM objects WHERE object_name=?",
                    (object_name,),
                )
                self._db.execute(
                    "DELETE FROM data_segments WHERE object_name=?",
                    (object_name,),
                )
                self._db.execute(
                    "DELETE FROM serving_packets WHERE object_name=?",
                    (object_name,),
                )
                self._db.execute(
                    "UPDATE serving_prefixes SET active=0 WHERE object_name=?",
                    (object_name,),
                )
                self._db.execute("""
                    DELETE FROM data_packets
                    WHERE NOT EXISTS (
                      SELECT 1 FROM object_packet_refs
                      WHERE object_packet_refs.data_name=data_packets.data_name
                    )
                """)
                self._refresh_used_bytes_locked()
                self._db.commit()
                removed = cursor.rowcount > 0
            except Exception:
                self._db.rollback()
                raise
        if removed:
            self._hot_cache.record_backing_write()
            self._cache_invalidate(object_name)
            for data_name in old_packet_names:
                self._hot_cache.invalidate_packet(data_name)
        if removed and deleted_manifest is not None:
            self._remember_catalog_change(deleted_manifest, "DELETED")
        return removed

    @staticmethod
    def _ndn_uri(name: str) -> str:
        return "ndn:" + name if name.startswith("/") else "ndn:/" + name

    @staticmethod
    def data_name(repo_node: str, object_name: str) -> str:
        return (
            f"{repo_node.rstrip('/')}/NDNSF-DISTRIBUTED-REPO/DATA/"
            f"{hashlib.sha256(object_name.encode()).hexdigest()}"
        )

    @staticmethod
    def object_data_name(object_name: str) -> str:
        return (
            "/NDNSF/DistributedRepo/Object/"
            f"{hashlib.sha256(object_name.encode()).hexdigest()}"
        )

    def _catch_chunks(self, name: str, timeout_s: int = 30) -> bytes:
        return fetch_segmented_object(
            name,
            timeout_ms=timeout_s * 1000,
            interest_lifetime_ms=_large_data_interest_lifetime_ms(),
            init_cwnd=8.0,
        )

    def _persist_serving_packets(self, object_name: str, generation: int,
                                 packets: list[DataPacket]) -> None:
        assert self._db is not None
        with self._db_lock:
            try:
                self._db.execute(
                    "DELETE FROM serving_packets WHERE object_name=? AND generation=?",
                    (object_name, generation),
                )
                self._db.executemany("""
                    INSERT INTO serving_packets
                      (data_name, object_name, generation, wire, wire_sha256, wire_size)
                    VALUES (?, ?, ?, ?, ?, ?)
                    ON CONFLICT(data_name) DO UPDATE SET
                      object_name=excluded.object_name,
                      generation=excluded.generation,
                      wire=excluded.wire,
                      wire_sha256=excluded.wire_sha256,
                      wire_size=excluded.wire_size
                """, [
                    (
                        packet.name, object_name, generation,
                        sqlite3.Binary(packet.wire),
                        hashlib.sha256(packet.wire).hexdigest(), len(packet.wire),
                    )
                    for packet in packets
                ])
                self._refresh_used_bytes_locked()
                self._db.commit()
            except Exception:
                self._db.rollback()
                raise

    def _activate_serving_prefix(self, prefix: str, object_name: str,
                                 generation: int = 0) -> None:
        normalized = prefix.rstrip("/")
        if not normalized:
            return
        assert self._db is not None
        with self._db_lock:
            self._db.execute("""
                INSERT INTO serving_prefixes(prefix, object_name, generation, active)
                VALUES (?, ?, ?, 1)
                ON CONFLICT(prefix) DO UPDATE SET
                  object_name=excluded.object_name,
                  generation=excluded.generation,
                  active=1
            """, (normalized, object_name, generation))
            self._db.commit()
        self._data_plane.activate_prefix(normalized)

    def _restore_serving_prefixes(self) -> None:
        assert self._db is not None
        with self._db_lock:
            prefixes = [
                str(row[0]) for row in self._db.execute(
                    "SELECT prefix FROM serving_prefixes WHERE active=1 ORDER BY prefix"
                ).fetchall()
            ]
        for prefix in prefixes:
            self._data_plane.activate_prefix(prefix)

    def _lookup_data_plane_wire(self, interest_name: str,
                                can_be_prefix: bool) -> Optional[bytes]:
        cached = self._hot_cache.get_packet(interest_name)
        if cached is not None:
            return cached.wire
        assert self._db is not None
        with self._db_lock:
            row = self._db.execute(
                "SELECT data_name, wire FROM data_packets WHERE data_name=?",
                (interest_name,),
            ).fetchone()
            if row is None:
                row = self._db.execute(
                    "SELECT data_name, wire FROM serving_packets WHERE data_name=?",
                    (interest_name,),
                ).fetchone()
            if row is None and can_be_prefix:
                like_prefix = interest_name.rstrip("/") + "/%"
                row = self._db.execute("""
                    SELECT data_name, wire FROM data_packets
                    WHERE data_name LIKE ? ORDER BY data_name LIMIT 1
                """, (like_prefix,)).fetchone()
                if row is None:
                    row = self._db.execute("""
                        SELECT data_name, wire FROM serving_packets
                        WHERE data_name LIKE ? ORDER BY data_name LIMIT 1
                    """, (like_prefix,)).fetchone()
        if row is None:
            return None
        packet = decode_data_packet(bytes(row[1]))
        if packet.name != str(row[0]):
            raise ValueError(
                f"{REPO_REASON_INTEGRITY_FAILURE}: stored serving wire/name mismatch")
        self._hot_cache.put_packet(packet)
        return packet.wire

    def _serve_object(self, name: str, payload: bytes,
                      object_name: str = "") -> RepoDataPlaneProducer:
        object_name = object_name or name
        generation = 0
        try:
            generation = self._load_manifest(object_name).generation
        except Exception:
            pass
        with self._db_lock:
            rows = self._db.execute("""
                SELECT data_name, wire FROM serving_packets
                WHERE object_name=? AND generation=? ORDER BY data_name
            """, (object_name, generation)).fetchall()
        if rows:
            packets = [decode_data_packet(bytes(row[1])) for row in rows]
        else:
            packets = make_segmented_data_packets(
                name,
                payload,
                signing_identity=self.provider_name,
                max_segment_size=6000,
                freshness_ms=60000,
            )
            self._persist_serving_packets(object_name, generation, packets)
        versioned_prefix = _packet_set_versioned_data_name(packets)
        self._activate_serving_prefix(name, object_name, generation)
        self._activate_serving_prefix(versioned_prefix, object_name, generation)
        return self._data_plane

    def _serve_packets(self, packets: list[DataPacket],
                       serving_prefix: str = "") -> RepoDataPlaneProducer:
        name = serving_prefix or _packet_set_versioned_data_name(packets)
        object_name = ""
        generation = 0
        if packets:
            with self._db_lock:
                row = self._db.execute("""
                    SELECT r.object_name, o.manifest_json
                    FROM object_packet_refs AS r
                    JOIN objects AS o ON o.object_name=r.object_name
                    WHERE r.data_name=? LIMIT 1
                """, (packets[0].name,)).fetchone()
            if row is not None:
                object_name = str(row[0])
                generation = RepoObjectManifest.from_dict(
                    json.loads(str(row[1]))).generation
        object_name = object_name or name
        self._activate_serving_prefix(name, object_name, generation)
        versioned_prefix = _packet_set_versioned_data_name(packets)
        if versioned_prefix != name:
            self._activate_serving_prefix(versioned_prefix, object_name, generation)
        return self._data_plane

    def _stop_producers(self) -> None:
        data_plane = getattr(self, "_data_plane", None)
        if data_plane is not None:
            data_plane.stop()

    def _advertise_prefix(self, prefix: str) -> None:
        if not self.advertise_stored_prefixes:
            return
        normalized = prefix.rstrip("/")
        if not normalized or normalized in self._advertised_prefixes:
            return
        result = subprocess.run(
            [self.advertise_command, "advertise", normalized],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"repo node {self.repo_node} failed to advertise {normalized}: "
                f"{result.stdout.strip()}"
            )
        self._advertised_prefixes.add(normalized)

    def _serving_forwarding_hint(self, data_prefix: str) -> str:
        del data_prefix
        return f"{self.provider_name.rstrip('/')}/NDNSF/REPO-SERVING"

    def _decode_packet_object(self, packet_obj: dict, operation: str) -> DataPacket:
        declared_name = str(packet_obj["name"])
        declared_segment = int(packet_obj["segment"])
        try:
            packet = decode_data_packet(
                base64.b64decode(str(packet_obj["wireB64"])))
        except Exception as exc:
            raise ValueError(f"repo-invalid-data-wire: {operation}: {exc}") from exc
        expected_name = str(packet_obj.get("segmentName", declared_name))
        if packet.name != declared_name or packet.name != expected_name:
            raise ValueError(
                f"repo-data-name-mismatch: {operation} Data name/wire mismatch: "
                f"declared={declared_name} metadata={expected_name} "
                f"encoded={packet.name}"
            )
        if packet.segment != declared_segment:
            raise ValueError(
                f"repo-packet-set-invalid: {operation} segment/wire mismatch: "
                f"declared={declared_segment} encoded={packet.segment}")
        expected_hash = str(packet_obj.get("wireSha256", ""))
        if (getattr(self, "exact_data_validation_policy",
                    "wire-name-and-request-digest") ==
                "wire-name-and-request-digest" and not expected_hash):
            raise ValueError(
                f"{operation} requires wireSha256 under strict validation policy")
        if expected_hash:
            actual_hash = hashlib.sha256(packet.wire).hexdigest()
            if actual_hash != expected_hash:
                raise ValueError(
                    f"{operation} wire hash mismatch for {packet.name}: "
                    f"metadata={expected_hash} actual={actual_hash}"
                )
        return packet

    def _has_manifest(self, object_name: str) -> bool:
        return self._sqlite_has_manifest(object_name)

    def _has_object(self, object_name: str) -> bool:
        return self._sqlite_has_object(object_name)

    def _validate_versioned_request(
        self,
        service_name: str,
        payload: bytes,
        requester_identity: str | None = None,
    ) -> dict:
        request = decode_repo_request(payload)
        operation = str(request["operation"]).upper()
        expected_service = repo_service_for_operation(operation, self.service_name)
        if service_name != expected_service:
            raise PermissionError(
                "repo-operation-service-mismatch: "
                f"operation={operation} expected={expected_service} got={service_name}"
            )
        if is_internal_repo_service(service_name) and requester_identity is not None:
            requester = requester_identity.rstrip("/")
            if not requester:
                raise PermissionError("repo-peer-identity-required")
            explicitly_allowed = getattr(self, "peer_provider_identities", frozenset())
            provider_prefix = getattr(self, "provider_prefix", "").rstrip("/")
            is_provider_identity = bool(
                provider_prefix and (
                    requester == provider_prefix or
                    requester.startswith(provider_prefix + "/")
                )
            )
            if explicitly_allowed:
                is_provider_identity = (
                    requester == self.provider_name.rstrip("/") or
                    requester in explicitly_allowed
                )
            if not is_provider_identity:
                raise PermissionError(
                    f"repo-peer-identity-required: requester={requester}")
        return request

    def _ack(self, payload: bytes, service_name: str | None = None) -> AckDecision:
        has_manifest = False
        has_object = False
        try:
            request = (
                self._validate_versioned_request(service_name, payload)
                if service_name is not None
                else decode_repo_request(payload)
            )
            operation = canonical_repo_operation(request["operation"])
            object_name = str(request.get("objectName", ""))
            data_name = str(request.get("dataName", ""))
            has_manifest = bool(object_name and self._has_manifest(object_name))
            has_object = bool(object_name and self._has_object(object_name))
            if operation in {
                "STORE_PACKETS",
                "STORE_PACKET",
                "STORE_PACKET_BATCH",
                "STORE_PACKET_PULL",
            }:
                manifest_obj = request.get("manifest", {})
                replica_nodes = set(manifest_obj.get("replicaNodes", []))
                if replica_nodes and self.repo_node not in replica_nodes:
                    return AckDecision(False, "repo-not-selected")
            if operation == "MANIFEST" and not has_manifest:
                return AckDecision(False, "repo-manifest-miss")
            if operation in {"FETCH", "FETCH_PREPARE"} and not has_object:
                return AckDecision(False, "repo-object-miss")
            if operation == "FETCH_PACKET_PREPARE" and not (
                    data_name and self._sqlite_has_packet(data_name)):
                return AckDecision(False, "repo-packet-miss")
        except Exception:
            return AckDecision(False, "repo-bad-request")
        capability = self._capability()
        cache_status = self._cache_status()
        runtime = self._runtime_snapshot()
        legacy_fields: dict[str, object] = {
            "repoNode": capability.repo_node,
            "freeBytes": capability.free_bytes,
            "usedBytes": capability.used_bytes,
            "load": capability.recent_load,
            "availability": capability.availability_score,
            "failureDomain": capability.failure_domain,
            "repoMode": capability.repo_mode,
            "acceptsBackupReplica": 1 if capability.accepts_backup_replica else 0,
            "memoryCacheBytes": self.memory_cache_bytes,
            "memoryCacheUsedBytes": cache_status["usedBytes"],
            "storageBackend": cache_status["storageBackend"],
            "authoritativeBackend": cache_status["authoritativeBackend"],
            "cachePolicy": cache_status["cachePolicy"],
            "hasManifest": 1 if has_manifest else 0,
            "hasObject": 1 if has_object else 0,
            "exactDataValidationPolicy": getattr(
                self, "exact_data_validation_policy",
                "wire-name-and-request-digest"),
            **runtime,
        }
        capability_hint = ProviderCapabilityHint(
            provider_name=capability.repo_node,
            service_name=(service_name or repo_service_for_operation(
                operation, self.service_name)),
            ready=True,
            message="repo-ready",
            runtime_hint=GenericProviderRuntimeHint(
                provider_name=capability.repo_node,
                timestamp_ms=int(runtime["metricsTimestampMs"]),
                active_work_count=(
                    int(runtime["inflightReads"]) +
                    int(runtime["inflightWrites"]) +
                    int(runtime["inflightRepair"])
                ),
                queue_length=int(runtime["queueDepth"]),
                estimated_queue_wait_ms=max(
                    float(runtime["storageReadLatencyMs"]),
                    float(runtime["storageWriteLatencyMs"]),
                ),
                capacity_hints={
                    "freeBytes": capability.free_bytes,
                    "usedBytes": capability.used_bytes,
                    "repoMode": capability.repo_mode,
                    "storageClasses": list(capability.storage_classes),
                },
                confidence=capability.availability_score,
            ),
            service_payload_schema="ndnsf-repo-capability-v1",
            service_payload={
                **legacy_fields,
                "storageClasses": list(capability.storage_classes),
            },
        )
        ack_payload = encode_provider_capability_ack(capability_hint)
        return AckDecision(status=True, message="repo-ready", payload=ack_payload)

    def _operation_status_payload(self,
                                  operation: str,
                                  status: str,
                                  *,
                                  object_name: str = "",
                                  manifest: RepoObjectManifest | None = None,
                                  message: str = "") -> dict[str, object]:
        state = (
            ServiceOperationState.DONE
            if status not in {"failed", "error", "skipped"}
            else ServiceOperationState.CANCELED
            if status == "skipped"
            else ServiceOperationState.FAILED
        )
        operation_status = ServiceOperationStatus(
            operation_id=f"{operation}:{object_name or (manifest.object_name if manifest else self.repo_node)}",
            operation=operation,
            service_name=repo_service_for_operation(operation, self.service_name),
            provider_name=self.repo_node,
            state=state,
            message=message or status,
            progress=1.0 if state == ServiceOperationState.DONE else 0.0,
            result_reference=(
                {"objectName": object_name or manifest.object_name}
                if object_name or manifest is not None else {}
            ),
            metadata={"legacyStatus": status},
        )
        payload: dict[str, object] = {"operationStatus": to_plain(operation_status)}
        if manifest is not None and state == ServiceOperationState.DONE:
            payload["dataProductReference"] = to_plain(DataProductReference(
                object_name=manifest.object_name,
                object_class=manifest.object_class or manifest.object_type,
                content_type="application/vnd.ndn.data",
                producer_name=self.repo_node,
                repo_manifest=manifest.to_dict(),
                size_bytes=manifest.size,
                digest=manifest.sha256,
                ttl_ms=manifest.ttl_ms,
                metadata={
                    "repoNode": self.repo_node,
                    "segmentCount": manifest.segment_count,
                    "replicationFactor": manifest.replication_factor,
                },
            ))
        return payload

    def _handle_context(self, context: dict[str, str], payload: bytes) -> ServiceResponse:
        return self._handle(payload, str(context.get("requesterIdentity", "")))

    def _handle_versioned_context(
        self,
        service_name: str,
        context: dict[str, str],
        payload: bytes,
    ) -> ServiceResponse:
        requester_identity = str(context.get("requesterIdentity", ""))
        try:
            self._validate_versioned_request(
                service_name, payload, requester_identity)
        except Exception as exc:  # noqa: BLE001
            return ServiceResponse(False, str(exc).encode(), str(exc))
        return self._handle(payload, requester_identity)

    def _handle(self, payload: bytes,
                requester_identity: str = "") -> ServiceResponse:
        admission: Optional[tuple[str, float]] = None
        try:
            request = decode_repo_request(payload)
            operation = canonical_repo_operation(request["operation"])
            self._enforce_request_ownership(
                operation, request, requester_identity)
            admission = self._admit_operation(operation)
            if operation == "CAPABILITY":
                capability = self._capability()
                cache_status = self._cache_status()
                runtime = self._runtime_snapshot()
                return ServiceResponse(True, json.dumps({
                    "repoNode": capability.repo_node,
                    "freeBytes": capability.free_bytes,
                    "usedBytes": capability.used_bytes,
                    "recentLoad": capability.recent_load,
                    "availabilityScore": capability.availability_score,
                    "failureDomain": capability.failure_domain,
                    "repoMode": capability.repo_mode,
                    "acceptsBackupReplica": capability.accepts_backup_replica,
                    "storageClasses": list(capability.storage_classes),
                    "storageBackend": cache_status["storageBackend"],
                    "authoritativeBackend": cache_status["authoritativeBackend"],
                    "capacityBytes": self.capacity_bytes,
                    "memoryCacheBytes": self.memory_cache_bytes,
                    "memoryCacheUsedBytes": cache_status["usedBytes"],
                    "exactDataValidationPolicy": getattr(
                        self, "exact_data_validation_policy",
                        "wire-name-and-request-digest"),
                    **runtime,
                    "providerCapabilityHint": to_plain(ProviderCapabilityHint(
                        provider_name=capability.repo_node,
                        service_name=repo_service_for_operation(
                            operation, self.service_name),
                        runtime_hint=GenericProviderRuntimeHint(
                            provider_name=capability.repo_node,
                            timestamp_ms=int(runtime["metricsTimestampMs"]),
                            active_work_count=(
                                int(runtime["inflightReads"]) +
                                int(runtime["inflightWrites"]) +
                                int(runtime["inflightRepair"])
                            ),
                            queue_length=int(runtime["queueDepth"]),
                            estimated_queue_wait_ms=max(
                                float(runtime["storageReadLatencyMs"]),
                                float(runtime["storageWriteLatencyMs"]),
                            ),
                            capacity_hints={
                                "freeBytes": capability.free_bytes,
                                "usedBytes": capability.used_bytes,
                                "repoMode": capability.repo_mode,
                            },
                            confidence=capability.availability_score,
                        ),
                        service_payload_schema="ndnsf-repo-capability-v1",
                        service_payload={
                            "repoNode": capability.repo_node,
                            "freeBytes": capability.free_bytes,
                            "usedBytes": capability.used_bytes,
                            "recentLoad": capability.recent_load,
                            "availabilityScore": capability.availability_score,
                            "failureDomain": capability.failure_domain,
                            "repoMode": capability.repo_mode,
                            "acceptsBackupReplica": capability.accepts_backup_replica,
                            "storageClasses": list(capability.storage_classes),
                        },
                    )),
                }, sort_keys=True).encode())
            if operation == "CACHE_STATUS":
                return ServiceResponse(
                    True,
                    json.dumps(self._cache_status(), sort_keys=True).encode(),
                )
            if operation == "RESERVE_CAPACITY":
                reservation = self._reserve_capacity(
                    str(request["reservationId"]),
                    str(request["operationId"]),
                    int(request["reservedBytes"]),
                    int(request.get("ttlMs", 30_000)),
                )
                return ServiceResponse(
                    True, json.dumps(reservation.to_dict(), sort_keys=True).encode())
            if operation == "RELEASE_CAPACITY":
                released = self._release_capacity(
                    reservation_id=str(request.get("reservationId", "")),
                    operation_id=str(request.get("operationId", "")),
                )
                return ServiceResponse(True, json.dumps({
                    "status": "released", "releasedCount": released,
                }, sort_keys=True).encode())
            if operation == "FINALIZE_WRITE":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                intent = self._write_intent_from_request(request, manifest)
                receipts = tuple(
                    RepoWriteReceipt.from_dict(value)
                    for value in request.get("writeReceipts", [])
                    if isinstance(value, dict)
                )
                finalized = self._finalize_write(manifest, intent, receipts)
                return ServiceResponse(True, json.dumps({
                    "status": "finalized",
                    "repoNode": self.repo_node,
                    "manifest": finalized.to_dict(),
                }, sort_keys=True).encode())
            if operation == "STORE":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                        **self._operation_status_payload(
                            operation,
                            "skipped",
                            object_name=manifest.object_name,
                            message="repo was not selected for this object",
                        ),
                    }, sort_keys=True).encode())
                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                if "payloadB64" in request:
                    object_payload = base64.b64decode(str(request["payloadB64"]))
                else:
                    object_payload = bytes.fromhex(str(request["payloadHex"]))
                receipt = self._persist_object(manifest, object_payload, intent=intent)
                if self._catalog_state_for_manifest(manifest) == "AVAILABLE":
                    self._serve_object(
                        self.data_name(self.repo_node, manifest.object_name),
                        object_payload,
                        manifest.object_name,
                    )
                return ServiceResponse(True, json.dumps({
                    "status": "stored",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                    "writeReceipt": receipt.to_dict(),
                    **self._operation_status_payload(operation, "stored", manifest=manifest),
                }, sort_keys=True).encode())
            if operation == "INSERT":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                        **self._operation_status_payload(
                            operation,
                            "skipped",
                            object_name=manifest.object_name,
                            message="repo was not selected for this object",
                        ),
                    }, sort_keys=True).encode())
                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                object_payload = self._catch_chunks(str(request["sourceName"]))
                if len(object_payload) != manifest.size:
                    raise ValueError(f"repo object size mismatch: {manifest.object_name}")
                if hashlib.sha256(object_payload).hexdigest() != manifest.sha256:
                    raise ValueError(f"repo object hash mismatch: {manifest.object_name}")
                receipt = self._persist_object(manifest, object_payload, intent=intent)
                if self._catalog_state_for_manifest(manifest) == "AVAILABLE":
                    self._serve_object(
                        self.data_name(self.repo_node, manifest.object_name),
                        object_payload,
                        manifest.object_name,
                    )
                return ServiceResponse(True, json.dumps({
                    "status": "inserted",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                    "writeReceipt": receipt.to_dict(),
                    **self._operation_status_payload(operation, "inserted", manifest=manifest),
                }, sort_keys=True).encode())
            if operation == "STORE_PACKETS":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                        **self._operation_status_payload(
                            operation,
                            "skipped",
                            object_name=manifest.object_name,
                            message="repo was not selected for this object",
                        ),
                    }, sort_keys=True).encode())
                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                packets = [
                    self._decode_packet_object(packet, operation)
                    for packet in request.get("packets", [])
                ]
                if not packets:
                    raise ValueError(f"STORE_PACKETS has no packets: {manifest.object_name}")
                stored_manifest = replace(
                    manifest,
                    segment_count=len(packets),
                    packet_names=tuple(packet.name for packet in packets),
                )
                receipt = self._persist_packets(stored_manifest, packets, intent=intent)
                serve_name = _packet_set_versioned_data_name(packets)
                if self._catalog_state_for_manifest(stored_manifest) == "AVAILABLE":
                    self._serve_packets(packets)
                return ServiceResponse(True, json.dumps({
                    "status": "stored-packets",
                    "repoNode": self.repo_node,
                    "manifest": stored_manifest.to_dict(),
                    "writeReceipt": receipt.to_dict(),
                    "dataName": serve_name,
                    **self._operation_status_payload(operation, "stored-packets", manifest=stored_manifest),
                }, sort_keys=True).encode())
            if operation in {"STORE_PACKET", "STORE_PACKET_BATCH"}:
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())
                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                metadata = dict(manifest.metadata or {})
                metadata["quorumFinalized"] = False
                manifest = replace(
                    manifest, lifecycle_state="RUNNING", metadata=metadata)
                if operation == "STORE_PACKET":
                    packets = [
                        self._decode_packet_object(request["packet"], operation)
                    ]
                else:
                    packets = [
                        self._decode_packet_object(packet, operation)
                        for packet in request.get("packets", [])
                    ]
                    if not packets:
                        raise ValueError(
                            f"STORE_PACKET_BATCH has no packets: {manifest.object_name}"
                        )
                existing_packets: list[DataPacket] = []
                try:
                    _, existing_packets = self._load_persisted_packets(manifest.object_name)
                except KeyError:
                    pass
                by_segment = {packet.segment: packet for packet in existing_packets}
                for packet in packets:
                    by_segment[packet.segment] = packet
                receipt = self._persist_packets(
                    manifest, list(by_segment.values()), intent=intent)
                try:
                    _, stored_packets = self._load_persisted_packets(manifest.object_name)
                    if (len(stored_packets) >= manifest.segment_count and
                            self._catalog_state_for_manifest(manifest) == "AVAILABLE"):
                        self._serve_packets(stored_packets)
                        self._remember_catalog_change(
                            manifest, self._catalog_state_for_manifest(manifest))
                except KeyError:
                    pass
                return ServiceResponse(True, json.dumps({
                    "status": "stored-packet-batch" if operation == "STORE_PACKET_BATCH" else "stored-packet",
                    "repoNode": self.repo_node,
                    "objectName": manifest.object_name,
                    "segments": [packet.segment for packet in packets],
                    "writeReceipt": receipt.to_dict(),
                }, sort_keys=True).encode())
            if operation == "STORE_PACKET_PULL":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())

                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                if isinstance(request.get("repairAuthorization"), dict):
                    metadata = dict(manifest.metadata or {})
                    metadata["quorumFinalized"] = True
                    manifest = replace(
                        manifest,
                        lifecycle_state="COMMITTED",
                        confirmed_replica_nodes=(self.repo_node,),
                        metadata=metadata,
                    )
                packet_manifest_name = str(request["packetManifestName"])
                packet_manifest_bytes = fetch_segmented_object(
                    packet_manifest_name,
                    timeout_ms=_pull_fetch_timeout_ms(max(1, manifest.segment_count // 32)),
                    interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                    init_cwnd=8.0,
                )
                expected_manifest_hash = str(request.get("packetManifestSha256", ""))
                if expected_manifest_hash:
                    actual_manifest_hash = hashlib.sha256(packet_manifest_bytes).hexdigest()
                    if actual_manifest_hash != expected_manifest_hash:
                        raise ValueError(
                            f"STORE_PACKET_PULL packet manifest hash mismatch: "
                            f"metadata={expected_manifest_hash} actual={actual_manifest_hash}"
                        )
                packet_manifest = json.loads(packet_manifest_bytes.decode())
                expected_packets = {
                    str(packet["name"]): str(packet["wireSha256"])
                    for packet in packet_manifest.get("packets", [])
                }
                expected_packets_by_segment = {
                    int(packet["segment"]): str(packet["wireSha256"])
                    for packet in packet_manifest.get("packets", [])
                }
                source_name = str(request["sourceName"])
                fetch_name = _packet_manifest_versioned_data_name(packet_manifest)
                packets = fetch_segmented_data_packets(
                    fetch_name or source_name,
                    timeout_ms=_pull_fetch_timeout_ms(manifest.segment_count),
                    interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                )
                if len(packets) != manifest.segment_count:
                    raise ValueError(
                        f"STORE_PACKET_PULL segment count mismatch for {manifest.object_name}: "
                        f"expected={manifest.segment_count} actual={len(packets)}"
                    )
                for packet in packets:
                    expected_hash = expected_packets.get(packet.name)
                    if expected_hash is None:
                        # The app publishes the packet manifest before the producer
                        # installs its final versioned Data name. NDN segmented
                        # retrieval may therefore return /prefix/v=.../seg=N while
                        # the manifest records /prefix/seg=N. The wire hash and
                        # segment number are the integrity boundary here.
                        expected_hash = expected_packets_by_segment.get(packet.segment)
                        if expected_hash is None:
                            raise ValueError(
                                f"STORE_PACKET_PULL unexpected segment name: {packet.name}"
                            )
                    actual_hash = hashlib.sha256(packet.wire).hexdigest()
                    if actual_hash != expected_hash:
                        raise ValueError(
                            f"STORE_PACKET_PULL wire hash mismatch for {packet.name}: "
                            f"metadata={expected_hash} actual={actual_hash}"
                        )
                manifest = replace(
                    manifest,
                    segment_count=len(packets),
                    packet_names=tuple(packet.name for packet in packets),
                )
                receipt = self._persist_packets(manifest, packets, intent=intent)
                serve_name = _packet_set_versioned_data_name(packets)
                if self._catalog_state_for_manifest(manifest) == "AVAILABLE":
                    self._serve_packets(packets)
                with self._catalog_lock:
                    catalog_entry = dict(
                        self._global_catalog[manifest.object_name][self.repo_node]
                    )
                return ServiceResponse(True, json.dumps({
                    "status": "stored-packet-pull",
                    "repoNode": self.repo_node,
                    "objectName": manifest.object_name,
                    "segmentCount": len(packets),
                    "dataName": serve_name,
                    "versionedDataName": serve_name,
                    "packetNames": [packet.name for packet in packets],
                    "manifest": manifest.to_dict(),
                    "catalogEntry": catalog_entry,
                    "writeReceipt": receipt.to_dict(),
                }, sort_keys=True).encode())
            if operation == "STORE_MANIFEST":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())
                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                receipt = self._persist_manifest(manifest, intent=intent)
                return ServiceResponse(True, json.dumps({
                    "status": "manifest-stored",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                    "writeReceipt": receipt.to_dict(),
                }, sort_keys=True).encode())
            if operation == "COMMIT_PACKET_SET":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())
                intent = self._write_intent_from_request(request, manifest)
                manifest = self._manifest_for_write_intent(manifest, intent)
                receipt = self._commit_existing_packet_set(manifest, intent)
                return ServiceResponse(True, json.dumps({
                    "status": "committed-packet-set",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                    "writeReceipt": receipt.to_dict(),
                }, sort_keys=True).encode())
            if operation == "FETCH":
                object_name = str(request["objectName"])
                self._require_finalized_manifest(self._load_manifest(object_name))
                fetched = self._sqlite_payload_bytes(object_name)
                return ServiceResponse(True, json.dumps({
                    "payloadB64": base64.b64encode(fetched).decode(),
                }, sort_keys=True).encode())
            if operation == "FETCH_PACKET_PREPARE":
                data_name = str(request["dataName"])
                self._require_finalized_packet_owner(data_name)
                packet = self._load_persisted_packet(data_name)
                if packet.name != data_name:
                    raise ValueError(
                        f"repo exact packet mismatch: requested={data_name} "
                        f"stored={packet.name}")
                # One producer owns the complete original versioned packet set.
                # Per-segment producers would compete for the same NFD prefix.
                packets = self._load_persisted_packet_prefix(data_name)
                serving_prefix = _packet_set_versioned_data_name(packets)
                self._serve_packets(packets)
                return ServiceResponse(True, json.dumps({
                    "dataName": packet.name,
                    "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
                    "forwardingHints": [
                        self._serving_forwarding_hint(serving_prefix)
                    ],
                }, sort_keys=True).encode())
            if operation == "FETCH_PREPARE":
                object_name = str(request["objectName"])
                stored_kind, manifest, stored_value = self._load_persisted_for_fetch(
                    object_name)
                self._require_finalized_manifest(manifest)
                if stored_kind == "packets":
                    packets = list(stored_value)
                    data_name = _packet_set_versioned_data_name(packets)
                    self._serve_packets(packets)
                    return ServiceResponse(True, json.dumps({
                        "dataName": data_name,
                        "versionedDataName": data_name,
                        "packetNames": [packet.name for packet in packets],
                        "forwardingHints": [
                            self._serving_forwarding_hint(data_name)
                        ],
                        "manifest": manifest.to_dict(),
                    }, sort_keys=True).encode())
                data_name = self.data_name(self.repo_node, object_name)
                fetched = bytes(stored_value)
                self._serve_object(data_name, fetched, object_name)
                return ServiceResponse(True, json.dumps({
                    "dataName": data_name,
                    "forwardingHints": [
                        self._serving_forwarding_hint(data_name)
                    ],
                    "manifest": manifest.to_dict(),
                }, sort_keys=True).encode())
            if operation == "MANIFEST":
                object_name = str(request["objectName"])
                manifest = self._require_finalized_manifest(
                    self._load_manifest(object_name))
                return ServiceResponse(True, manifest.to_bytes())
            if operation == "INVENTORY":
                inventory = {
                    name: manifest
                    for name, manifest in self._sqlite_inventory().items()
                    if self._catalog_state_for_manifest(manifest) == "AVAILABLE"
                }
                return ServiceResponse(True, json.dumps({
                    name: manifest.to_dict()
                    for name, manifest in inventory.items()
                }, sort_keys=True).encode())
            if operation == "CATALOG_STATUS":
                snapshot = self._catalog_snapshot()
                status = self._catalog_status_entry()
                return ServiceResponse(True, json.dumps({
                    **status,
                    "objectCount": len(snapshot["entries"]),
                    "acceptsBackupReplica": self.capability.accepts_backup_replica,
                    "staleAfterMs": self._catalog_stale_after_ms,
                }, sort_keys=True).encode())
            if operation == "CATALOG_SNAPSHOT":
                return ServiceResponse(
                    True,
                    json.dumps(self._catalog_snapshot(), sort_keys=True).encode(),
                )
            if operation == "CATALOG_DELTA":
                since_epoch = int(request.get("sinceEpoch", 0))
                return ServiceResponse(
                    True,
                    json.dumps(self._catalog_delta(since_epoch), sort_keys=True).encode(),
                )
            if operation == "CATALOG_BUCKET_DIGEST":
                return ServiceResponse(True, json.dumps(
                    self._catalog_bucket_digest(int(request.get("bucketCount", 64))),
                    sort_keys=True,
                ).encode())
            if operation == "CATALOG_BUCKET_ENTRIES":
                return ServiceResponse(True, json.dumps(
                    self._catalog_bucket_entries(
                        int(request.get("bucket", 0)),
                        int(request.get("bucketCount", 64)),
                    ),
                    sort_keys=True,
                ).encode())
            if operation == "CATALOG_LOOKUP":
                object_name = str(request["objectName"])
                return ServiceResponse(
                    True,
                    json.dumps(self._catalog_lookup(object_name), sort_keys=True).encode(),
                )
            if operation == "CATALOG_QUERY":
                query = request.get("query", {})
                if not isinstance(query, dict):
                    raise ValueError("CATALOG_QUERY query must be an object")
                return ServiceResponse(
                    True,
                    json.dumps(self._catalog_query(query), sort_keys=True).encode(),
                )
            if operation == "CATALOG_MERGE":
                entries = request.get("entries", [])
                if not isinstance(entries, list):
                    raise ValueError("CATALOG_MERGE entries must be a list")
                source_status = request.get("sourceStatus", {})
                if source_status is not None and not isinstance(source_status, dict):
                    raise ValueError("CATALOG_MERGE sourceStatus must be an object")
                self._merge_catalog_entries(entries, source_status)
                return ServiceResponse(True, json.dumps({
                    "status": "merged",
                    "repoNode": self.repo_node,
                    "entryCount": len(entries),
                }, sort_keys=True).encode())
            if operation == "CATALOG_MERGE_PULL":
                schema_version = int(request.get("schemaVersion", 0) or 0)
                if schema_version != 1:
                    raise ValueError(
                        "CATALOG_MERGE_PULL unsupported schemaVersion")
                source_name = str(request.get("sourceName", ""))
                expected_sha256 = str(request.get("payloadSha256", ""))
                expected_bytes = int(request.get("payloadBytes", 0) or 0)
                expected_entries = int(request.get("entryCount", -1))
                if not source_name or not expected_sha256:
                    raise ValueError(
                        "CATALOG_MERGE_PULL requires sourceName and payloadSha256")
                if expected_bytes < 1 or expected_bytes > CATALOG_MERGE_MAX_PULL_BYTES:
                    raise ValueError(
                        "CATALOG_MERGE_PULL payloadBytes outside allowed range")
                if expected_entries < 0:
                    raise ValueError(
                        "CATALOG_MERGE_PULL entryCount must be non-negative")
                merged_payload = fetch_segmented_object(
                    source_name,
                    timeout_ms=max(60_000, min(300_000, expected_bytes // 100 + 30_000)),
                    interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                    init_cwnd=8.0,
                )
                if len(merged_payload) != expected_bytes:
                    raise ValueError(
                        "CATALOG_MERGE_PULL payload length mismatch")
                actual_sha256 = hashlib.sha256(merged_payload).hexdigest()
                if actual_sha256 != expected_sha256:
                    raise ValueError(
                        "CATALOG_MERGE_PULL payload hash mismatch")
                decoded_merge = json.loads(merged_payload.decode())
                if not isinstance(decoded_merge, dict):
                    raise ValueError(
                        "CATALOG_MERGE_PULL payload must be an object")
                if int(decoded_merge.get("schemaVersion", 0) or 0) != schema_version:
                    raise ValueError(
                        "CATALOG_MERGE_PULL payload schema mismatch")
                entries = decoded_merge.get("entries", [])
                source_status = decoded_merge.get("sourceStatus", {})
                if not isinstance(entries, list):
                    raise ValueError(
                        "CATALOG_MERGE_PULL entries must be a list")
                if len(entries) != expected_entries:
                    raise ValueError(
                        "CATALOG_MERGE_PULL entryCount mismatch")
                if source_status is not None and not isinstance(source_status, dict):
                    raise ValueError(
                        "CATALOG_MERGE_PULL sourceStatus must be an object")
                self._merge_catalog_entries(entries, source_status)
                return ServiceResponse(True, json.dumps({
                    "status": "merged",
                    "mode": "pull",
                    "repoNode": self.repo_node,
                    "entryCount": len(entries),
                    "payloadBytes": len(merged_payload),
                }, sort_keys=True).encode())
            if operation == "CATALOG_REPAIR":
                raise ValueError(
                    "provider-side CATALOG_REPAIR is disabled; use the "
                    "client/sidecar repair orchestrator"
                )
            if operation == "REPAIR_SCAN":
                return ServiceResponse(
                    True, json.dumps(self._scan_repair_jobs(), sort_keys=True).encode())
            if operation == "REPAIR_CLAIM":
                job = self._claim_repair_job(
                    str(request.get("leaseOwner", "")),
                    int(request.get("leaseMs", 30_000)),
                )
                return ServiceResponse(True, json.dumps({
                    "status": "claimed" if job else "empty",
                    "job": job,
                }, sort_keys=True).encode())
            if operation == "REPAIR_COMPLETE":
                return ServiceResponse(True, json.dumps(self._finish_repair_job(
                    str(request["repairId"]), success=True,
                    result=dict(request.get("result", {})),
                ), sort_keys=True).encode())
            if operation == "REPAIR_FAIL":
                return ServiceResponse(True, json.dumps(self._finish_repair_job(
                    str(request["repairId"]), success=False,
                    error=str(request.get("error", "repair failed")),
                ), sort_keys=True).encode())
            if operation == "SCRUB":
                return ServiceResponse(True, json.dumps(
                    self._scrub(int(request.get("limit", 100))),
                    sort_keys=True,
                ).encode())
            if operation == "DELETE":
                object_name = str(request["objectName"])
                removed = self._delete_object(object_name)
                return ServiceResponse(True, json.dumps({
                    "status": "deleted" if removed else "not-found",
                    "repoNode": self.repo_node,
                    "objectName": object_name,
                }, sort_keys=True).encode())
            raise ValueError(f"unsupported repo operation {operation}")
        except Exception as exc:  # noqa: BLE001
            return ServiceResponse(False, str(exc).encode(), str(exc))
        finally:
            if admission is not None:
                self._release_operation(admission)

    def _request_peer_catalog_delta(self, peer_repo_node: str, since_epoch: int) -> dict:
        sync_user = ServiceUser(
            group=self.group,
            controller=self.controller,
            user=self.provider_name,
            trust_schema=self.trust_schema,
            permission_wait_ms=6000,
            adaptive_admission=False,
            serve_certificates=False,
        )

        def selector(candidates: list[AckCandidate]) -> list[str]:
            for candidate in candidates:
                fields = self._parse_ack_payload(candidate.payload)
                if fields.get("repoNode") == peer_repo_node:
                    return [candidate.provider_name]
            return []

        response = sync_user.request_service_select(
            repo_service_for_operation("CATALOG_DELTA", self.service_name),
            encode_repo_request("CATALOG_DELTA", sinceEpoch=since_epoch),
            selector,
            ack_timeout_ms=1000,
            timeout_ms=15000,
            request_strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog delta response must be a JSON object")
        return decoded

    def _catalog_sync_loop(self) -> None:
        time.sleep(min(2.0, self.catalog_sync_interval_s))
        while not self._catalog_stop.is_set():
            for peer in self.peer_repo_nodes:
                since_epoch = self._peer_catalog_epochs.get(peer, 0)
                try:
                    delta = self._request_peer_catalog_delta(peer, since_epoch)
                    source_status = delta.get("repoStatus", {})
                    if source_status is not None and not isinstance(source_status, dict):
                        source_status = {}
                    self._merge_catalog_entries(delta.get("entries", []), source_status)
                    peer_sequence = int(delta.get("catalogEpoch", since_epoch))
                    self._peer_catalog_epochs[peer] = peer_sequence
                    with self._db_lock:
                        self._db.execute("""
                            INSERT INTO peer_watermarks
                              (peer_repo, peer_boot_id, source_sequence, updated_at_ms)
                            VALUES (?, ?, ?, ?)
                            ON CONFLICT(peer_repo) DO UPDATE SET
                              peer_boot_id=excluded.peer_boot_id,
                              source_sequence=excluded.source_sequence,
                              updated_at_ms=excluded.updated_at_ms
                        """, (
                            peer, str(source_status.get("bootId", "")),
                            peer_sequence, self._now_ms(),
                        ))
                        self._db.commit()
                except Exception as exc:  # noqa: BLE001
                    print(
                        f"repo catalog sync warning self={self.repo_node} "
                        f"peer={peer}: {exc}",
                        flush=True,
                    )
            try:
                self._scan_repair_jobs()
            except Exception as exc:  # noqa: BLE001
                print(f"repo repair scan warning self={self.repo_node}: {exc}", flush=True)
            self._catalog_stop.wait(self.catalog_sync_interval_s)

    def _start_catalog_sync(self) -> None:
        if self.capability.repo_mode != "persistent" or not self.peer_repo_nodes:
            return
        if self._catalog_thread is not None:
            return
        self._catalog_thread = threading.Thread(
            target=self._catalog_sync_loop,
            name=f"repo-catalog-sync-{self.repo_node}",
            daemon=True,
        )
        self._catalog_thread.start()

    def run(self) -> int:
        for service_name in repo_versioned_services(self.service_name):
            self.provider.add_context_handler(
                service_name,
                lambda context, payload, registered=service_name:
                self._handle_versioned_context(registered, context, payload),
            )
            self.provider.set_ack_handler(
                service_name,
                lambda payload, registered=service_name:
                self._ack(payload, registered),
            )
        self._data_plane.start()
        self._start_catalog_sync()
        try:
            return self.provider.run()
        finally:
            self._catalog_stop.set()
            self._stop_producers()
            if self._db is not None:
                with self._db_lock:
                    self._db.close()
                    self._db = None

    def seed_object(
        self,
        object_name: str,
        payload: bytes | bytearray | memoryview | str,
        *,
        object_type: str = "bootstrap-config",
        policy_epoch: str = "",
    ) -> RepoObjectManifest:
        """Preload an object into this repo node before serving requests."""

        payload_bytes = payload.encode() if isinstance(payload, str) else bytes(payload)
        manifest = RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=hashlib.sha256(payload_bytes).hexdigest(),
            size=len(payload_bytes),
            segment_count=1,
            replication_factor=1,
            min_replication_factor=1,
            max_replication_factor=1,
            replica_nodes=(self.repo_node,),
            policy_epoch=policy_epoch,
        )
        self._persist_object(manifest, payload_bytes)
        self._serve_object(
            self.data_name(self.repo_node, manifest.object_name),
            payload_bytes,
            manifest.object_name,
        )
        return manifest


class NetworkDistributedRepoClient:
    """NDNSF client for a versioned-operation DistributedRepo cluster."""

    def __init__(
        self,
        *,
        user: ServiceUser,
        service_name: str = "/NDNSF/DistributedRepo",
        upload_prefix: str = "/NDNSF-DistributeInference/example/user/NDNSF-DISTRIBUTED-REPO/UPLOAD",
        ack_timeout_ms: int = 500,
        timeout_ms: int = 10000,
        max_segment_payload: int = 4800,
        verbose: bool = False,
        max_store_batch_wire_bytes: int = 2500,
        pull_store_threshold_bytes: int = 65536,
        placement_cache_ttl_ms: int = 5000,
        replica_cooldown_ms: int = 3000,
        hedged_read_delay_ms: int = 0,
        enable_capacity_reservations: bool = True,
        control_mode: str = "targeted",
        enable_targeted_fallback: bool = True,
    ) -> None:
        self.user = user
        self.service_name = service_name
        self.upload_prefix = upload_prefix.rstrip("/")
        self.ack_timeout_ms = ack_timeout_ms
        self.timeout_ms = timeout_ms
        self.max_segment_payload = max(512, max_segment_payload)
        self._placement_cache: list[str] = []
        self._placement_cache_updated_ms = 0
        self.placement_cache_ttl_ms = max(0, int(placement_cache_ttl_ms))
        self.replica_cooldown_ms = max(0, int(replica_cooldown_ms))
        self.hedged_read_delay_ms = max(0, int(hedged_read_delay_ms))
        self.enable_capacity_reservations = bool(enable_capacity_reservations)
        normalized_control_mode = str(control_mode).strip().lower()
        if normalized_control_mode not in {"normal", "targeted"}:
            raise ValueError("repo control_mode must be 'normal' or 'targeted'")
        self.control_mode = normalized_control_mode
        self.enable_targeted_fallback = bool(enable_targeted_fallback)
        self._replica_health: dict[str, dict[str, float]] = {}
        self._replica_health_lock = threading.RLock()
        self._client_local = threading.local()
        # ndn-cxx/ServiceUser owns asynchronous callback state. Route every
        # control-plane invocation through one stable caller thread; a mutex
        # alone still lets successive calls enter from different OS threads.
        self._control_executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="ndnsf-repo-control")
        self._control_local = threading.local()
        self._control_metrics_lock = threading.Lock()
        self._control_metrics = {
            "normalCalls": 0,
            "targetedCalls": 0,
            "targetedAsyncSubmitted": 0,
            "targetedAsyncCompleted": 0,
            "targetedTimeouts": 0,
            "targetedFallbacks": 0,
            "replicaFanouts": 0,
            "currentReplicaCalls": 0,
            "maxConcurrentReplicaCalls": 0,
            "reserveCount": 0,
            "reserveTotalMs": 0.0,
            "reserveLastMs": 0.0,
            "storeCount": 0,
            "storeTotalMs": 0.0,
            "storeLastMs": 0.0,
        }
        self._fanout_states_lock = threading.Lock()
        self._fanout_states: list[dict] = []
        self._closed = threading.Event()
        self._prepare_lock = threading.RLock()
        self._prepared_objects: dict[tuple[str, str, str], dict] = {}
        self.verbose = verbose
        self.max_store_batch_wire_bytes = max(1024, max_store_batch_wire_bytes)
        self.pull_store_threshold_bytes = max(0, pull_store_threshold_bytes)

    def _service_for(self, operation: str) -> str:
        return repo_service_for_operation(operation, self.service_name)

    def _service_for_payload(self, payload: bytes) -> str:
        request = decode_repo_request(payload)
        return self._service_for(str(request["operation"]))

    def _log(self, message: str) -> None:
        if self.verbose:
            print(message, flush=True)

    def _control_call(self, callback):
        if self._closed.is_set():
            raise RuntimeError("repo client is closed")
        if getattr(self._control_local, "active", False):
            return callback()

        def invoke():
            self._control_local.active = True
            try:
                return callback()
            finally:
                self._control_local.active = False

        return self._control_executor.submit(invoke).result()

    def _ensure_control_metrics(self) -> None:
        if hasattr(self, "_control_metrics_lock"):
            return
        self._control_metrics_lock = threading.Lock()
        self._control_metrics = {
            "normalCalls": 0,
            "targetedCalls": 0,
            "targetedAsyncSubmitted": 0,
            "targetedAsyncCompleted": 0,
            "targetedTimeouts": 0,
            "targetedFallbacks": 0,
            "replicaFanouts": 0,
            "currentReplicaCalls": 0,
            "maxConcurrentReplicaCalls": 0,
            "reserveCount": 0,
            "reserveTotalMs": 0.0,
            "reserveLastMs": 0.0,
            "storeCount": 0,
            "storeTotalMs": 0.0,
            "storeLastMs": 0.0,
        }

    def _metric_increment(self, key: str, amount: int = 1) -> None:
        self._ensure_control_metrics()
        with self._control_metrics_lock:
            self._control_metrics[key] = int(
                self._control_metrics.get(key, 0)) + amount

    def _metric_begin_replica_call(self) -> None:
        self._ensure_control_metrics()
        with self._control_metrics_lock:
            current = int(self._control_metrics["currentReplicaCalls"]) + 1
            self._control_metrics["currentReplicaCalls"] = current
            self._control_metrics["maxConcurrentReplicaCalls"] = max(
                int(self._control_metrics["maxConcurrentReplicaCalls"]), current)

    def _metric_end_replica_call(self) -> None:
        self._ensure_control_metrics()
        with self._control_metrics_lock:
            self._control_metrics["currentReplicaCalls"] = max(
                0, int(self._control_metrics["currentReplicaCalls"]) - 1)

    def _record_control_phase(self, phase: str, elapsed_ms: float) -> None:
        self._ensure_control_metrics()
        count_key = f"{phase}Count"
        total_key = f"{phase}TotalMs"
        last_key = f"{phase}LastMs"
        with self._control_metrics_lock:
            self._control_metrics[count_key] = int(
                self._control_metrics.get(count_key, 0)) + 1
            self._control_metrics[total_key] = float(
                self._control_metrics.get(total_key, 0.0)) + elapsed_ms
            self._control_metrics[last_key] = elapsed_ms
        operation_metrics = getattr(
            getattr(self, "_client_local", None),
            "operation_phase_metrics", None)
        if operation_metrics is not None:
            operation_metrics[f"{phase}Ms"] = (
                float(operation_metrics.get(f"{phase}Ms", 0.0)) + elapsed_ms)

    def begin_operation_metrics(self) -> None:
        if not hasattr(self, "_client_local"):
            self._client_local = threading.local()
        self._client_local.operation_phase_metrics = {}

    def end_operation_metrics(self) -> dict[str, float]:
        if not hasattr(self, "_client_local"):
            return {}
        metrics = dict(getattr(
            self._client_local, "operation_phase_metrics", {}))
        if hasattr(self._client_local, "operation_phase_metrics"):
            del self._client_local.operation_phase_metrics
        return metrics

    def control_metrics(self) -> dict[str, int | float | str]:
        self._ensure_control_metrics()
        with self._control_metrics_lock:
            metrics = dict(self._control_metrics)
        metrics["controlMode"] = self.control_mode
        metrics["targetedFallbackEnabled"] = self.enable_targeted_fallback
        return metrics

    def reset_control_metrics(self) -> None:
        """Reset measured counters after bootstrap or experiment warmup."""

        self._ensure_control_metrics()
        with self._control_metrics_lock:
            for key, value in self._control_metrics.items():
                self._control_metrics[key] = 0.0 if isinstance(value, float) else 0

    def close(self) -> None:
        self._closed.set()
        with self._fanout_states_lock:
            states = list(self._fanout_states)
        for state in states:
            condition = state["condition"]
            with condition:
                state["closed"] = True
                condition.notify_all()
        executor = getattr(self, "_control_executor", None)
        if executor is not None:
            self._control_executor = None
            executor.shutdown(wait=True)

    def _placement_cache_valid(self, replication_factor: int) -> bool:
        if len(self._placement_cache) < replication_factor:
            return False
        return (
            getattr(self, "placement_cache_ttl_ms", 0) > 0 and
            self._now_ms() - getattr(self, "_placement_cache_updated_ms", 0) <=
            getattr(self, "placement_cache_ttl_ms", 0) and
            all(not self._replica_in_cooldown(repo)
                for repo in self._placement_cache[:replication_factor])
        )

    @staticmethod
    def _now_ms() -> int:
        return int(time.time() * 1000)

    def _replica_in_cooldown(self, repo_node: str) -> bool:
        if not hasattr(self, "_replica_health_lock"):
            self._replica_health_lock = threading.RLock()
        with self._replica_health_lock:
            return self._now_ms() < int(
                getattr(self, "_replica_health", {}).get(
                    repo_node, {}).get("cooldownUntilMs", 0))

    def _record_replica_result(
        self,
        repo_node: str,
        *,
        success: bool,
        latency_ms: float = 0.0,
        definitive_failure: bool = False,
    ) -> None:
        if not hasattr(self, "_replica_health"):
            self._replica_health = {}
        if not hasattr(self, "_replica_health_lock"):
            self._replica_health_lock = threading.RLock()
        with self._replica_health_lock:
            state = self._replica_health.setdefault(repo_node, {
                "latencyEwmaMs": 0.0, "failures": 0.0, "cooldownUntilMs": 0.0})
            if success:
                previous = float(state["latencyEwmaMs"])
                state["latencyEwmaMs"] = latency_ms if previous <= 0 else (
                    0.8 * previous + 0.2 * latency_ms)
                state["failures"] = max(0.0, float(state["failures"]) - 0.5)
                state["cooldownUntilMs"] = 0.0
            else:
                state["failures"] = float(state["failures"]) + 1.0
                base_cooldown_ms = max(
                    1, int(getattr(self, "replica_cooldown_ms", 3000)))
                failure_multiplier = 1 << min(
                    max(0, int(state["failures"]) - 1), 4)
                cooldown_ms = base_cooldown_ms * failure_multiplier
                if definitive_failure:
                    cooldown_ms = max(
                        cooldown_ms,
                        max(1, int(getattr(self, "timeout_ms", 10000))) * 8)
                state["cooldownUntilMs"] = self._now_ms() + cooldown_ms
                if repo_node in getattr(self, "_placement_cache", []):
                    self._placement_cache = []
                    self._placement_cache_updated_ms = 0

    def _ordered_replicas(self, replicas: Iterable[str]) -> list[str]:
        if not hasattr(self, "_replica_health_lock"):
            self._replica_health_lock = threading.RLock()
        with self._replica_health_lock:
            health = {
                repo: dict(state)
                for repo, state in getattr(self, "_replica_health", {}).items()
            }
        return sorted(
            (str(repo) for repo in replicas if str(repo)),
            key=lambda repo: (
                self._replica_in_cooldown(repo),
                float(health.get(repo, {}).get("failures", 0.0)),
                float(health.get(repo, {}).get("latencyEwmaMs", 0.0)),
                repo,
            ),
        )

    @staticmethod
    def _packet_to_request(packet: DataPacket) -> dict:
        return {
            "name": packet.name,
            "segment": packet.segment,
            "segmentName": packet.name,
            "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
            "wireB64": base64.b64encode(packet.wire).decode(),
        }

    def _packet_batches(self, packets: list[DataPacket]) -> Iterable[list[DataPacket]]:
        batch: list[DataPacket] = []
        batch_bytes = 0
        for packet in packets:
            packet_bytes = len(packet.wire)
            if batch and batch_bytes + packet_bytes > self.max_store_batch_wire_bytes:
                yield batch
                batch = []
                batch_bytes = 0
            batch.append(packet)
            batch_bytes += packet_bytes
        if batch:
            yield batch

    def _upload_data_name(self, repo_node: str, object_name: str) -> str:
        repo_hash = hashlib.sha256(repo_node.encode()).hexdigest()[:16]
        object_hash = hashlib.sha256(object_name.encode()).hexdigest()
        return f"{self.upload_prefix}/DATA/{object_hash}/{repo_hash}"

    def _shared_upload_data_name(self, object_name: str) -> str:
        object_hash = hashlib.sha256(object_name.encode()).hexdigest()
        return f"{self.upload_prefix}/DATA/{object_hash}"

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
    def _packet_data_name(packets: list[DataPacket]) -> str:
        if not packets:
            raise ValueError("signed packet list is empty")
        first_name = packets[0].name
        if "/seg=" not in first_name:
            return first_name
        data_name = first_name.rsplit("/seg=", 1)[0]
        if "/v=" in data_name:
            return data_name.rsplit("/v=", 1)[0]
        return data_name

    @staticmethod
    def _packet_versioned_data_name(packets: list[DataPacket]) -> str:
        if not packets:
            raise ValueError("signed packet list is empty")
        first_name = packets[0].name
        if "/seg=" not in first_name:
            return first_name
        return first_name.rsplit("/seg=", 1)[0]

    def _packet_manifest_name(self, repo_node: str, object_name: str) -> str:
        repo_hash = hashlib.sha256(repo_node.encode()).hexdigest()[:16]
        object_hash = hashlib.sha256(object_name.encode()).hexdigest()
        return f"{self.upload_prefix}/PACKET-MANIFEST/{object_hash}/{repo_hash}"

    def capability(self, *, timeout_ms: int | None = None) -> dict:
        response = self._control_call(lambda: self.user.request_service(
            self._service_for("CAPABILITY"),
            encode_repo_request("CAPABILITY"),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms if timeout_ms is not None else self.timeout_ms,
            strategy="first-responding",
        ))
        if not response.status:
            raise RuntimeError(response.error)
        return json.loads(response.payload.decode())

    def cache_status(self, repo_node: str) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("CACHE_STATUS"),
            timeout_ms=self.timeout_ms,
        )
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo cache status response must be a JSON object")
        return decoded

    def reserve_capacity(self, repo_node: str, *, operation_id: str,
                         reserved_bytes: int, ttl_ms: int = 30_000) -> dict:
        reservation_id = hashlib.sha256(
            f"{operation_id}|{repo_node}".encode()).hexdigest()
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "RESERVE_CAPACITY", reservationId=reservation_id,
                operationId=operation_id, reservedBytes=reserved_bytes,
                ttlMs=ttl_ms),
        )
        return json.loads(response.payload.decode())

    def release_capacity(self, repo_node: str, *, reservation_id: str = "",
                         operation_id: str = "") -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "RELEASE_CAPACITY", reservationId=reservation_id,
                operationId=operation_id),
        )
        return json.loads(response.payload.decode())

    def catalog_bucket_digest(self, repo_node: str,
                              bucket_count: int = 64) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "CATALOG_BUCKET_DIGEST", bucketCount=bucket_count),
        )
        return json.loads(response.payload.decode())

    def catalog_bucket_entries(self, repo_node: str, bucket: int,
                               bucket_count: int = 64) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "CATALOG_BUCKET_ENTRIES", bucket=bucket,
                bucketCount=bucket_count),
        )
        return json.loads(response.payload.decode())

    def repair_scan(self, repo_node: str) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node, payload=encode_repo_request("REPAIR_SCAN"))
        return json.loads(response.payload.decode())

    def repair_claim(
        self,
        repo_node: str,
        *,
        lease_owner: str,
        lease_ms: int = 60_000,
    ) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "REPAIR_CLAIM", leaseOwner=lease_owner, leaseMs=lease_ms),
        )
        return json.loads(response.payload.decode())

    def repair_complete(
        self,
        repo_node: str,
        *,
        repair_id: str,
        result: dict,
    ) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "REPAIR_COMPLETE", repairId=repair_id, result=result),
        )
        return json.loads(response.payload.decode())

    def repair_fail(
        self,
        repo_node: str,
        *,
        repair_id: str,
        error: str,
    ) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "REPAIR_FAIL", repairId=repair_id, error=error),
        )
        return json.loads(response.payload.decode())

    def scrub(self, repo_node: str, limit: int = 100) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("SCRUB", limit=limit),
            timeout_ms=max(self.timeout_ms, 60_000),
        )
        return json.loads(response.payload.decode())

    def store_versioned(
        self, *, object_name: str, payload: bytes, object_type: str,
        generation: int, expected_generation: int,
        write_consistency: str = WriteConsistency.ALL.value,
        replication_factor: int = 1, replica_nodes: tuple[str, ...] = (),
        policy_epoch: str = "", metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        required = required_write_acks(replication_factor, write_consistency)
        manifest = RepoObjectManifest(
            object_name=object_name, object_type=object_type,
            sha256=hashlib.sha256(payload).hexdigest(), size=len(payload),
            replication_factor=replication_factor,
            generation=generation, parent_generation=expected_generation,
            write_consistency=write_consistency,
            required_write_acks=required,
            metadata=dict(metadata or {}),
        )
        return self._store_once(
            object_name=object_name, payload=payload, object_type=object_type,
            replication_factor=replication_factor, replica_nodes=replica_nodes,
            policy_epoch=policy_epoch, manifest_override=manifest,
            metadata=metadata,
        )

    def _prepare_fetch_source(
        self,
        repo_node: str,
        object_name: str,
        expected_data_name: str = "",
        timeout_ms: int | None = None,
    ) -> dict:
        if not hasattr(self, "_prepare_lock"):
            self._prepare_lock = threading.RLock()
        with self._prepare_lock:
            return self._prepare_fetch_source_locked(
                repo_node, object_name, expected_data_name, timeout_ms)

    def _prepare_fetch_source_locked(
        self,
        repo_node: str,
        object_name: str,
        expected_data_name: str = "",
        timeout_ms: int | None = None,
    ) -> dict:
        """Activate transient packet serving from a persistent Repo node."""

        key = (repo_node, object_name, expected_data_name)
        with getattr(self, "_prepare_lock", nullcontext()):
            cached = getattr(self, "_prepared_objects", {}).get(key)
            if cached is not None:
                return dict(cached)
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("FETCH_PREPARE", objectName=object_name),
            timeout_ms=timeout_ms or max(self.timeout_ms, 30000),
        )
        prepared = json.loads(response.payload.decode())
        if not isinstance(prepared, dict):
            raise ValueError("repo fetch preparation response must be a JSON object")
        data_name = str(prepared.get("dataName", ""))
        if not data_name:
            raise ValueError(f"repo fetch preparation returned no Data name: {repo_node}")
        expected_prefix = expected_data_name.rstrip("/")
        if (expected_data_name and data_name != expected_data_name and
                not data_name.startswith(expected_prefix + "/")):
            raise ValueError(
                "repo fetch preparation Data name mismatch: "
                f"manifest={expected_data_name} prepared={data_name}"
            )
        prepared_manifest = RepoObjectManifest.from_dict(prepared["manifest"])
        if prepared_manifest.object_name != object_name:
            raise ValueError(
                "repo fetch preparation object mismatch: "
                f"requested={object_name} prepared={prepared_manifest.object_name}"
            )
        if not hasattr(self, "_prepared_objects"):
            self._prepared_objects = {}
        if not hasattr(self, "_prepare_lock"):
            self._prepare_lock = threading.RLock()
        with self._prepare_lock:
            self._prepared_objects[key] = dict(prepared)
        return prepared

    def fetch_packet(self, repo_node: str, data_name: str) -> DataPacket:
        """Fetch one immutable packet by its complete original NDN Data name."""

        if not hasattr(self, "_client_local"):
            self._client_local = threading.local()
        attempt_timeout_ms = int(getattr(
            self._client_local, "attempt_timeout_ms", max(self.timeout_ms, 30000)))
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "FETCH_PACKET_PREPARE", dataName=data_name),
            timeout_ms=attempt_timeout_ms,
        )
        prepared = json.loads(response.payload.decode())
        if str(prepared.get("dataName", "")) != data_name:
            raise ValueError(
                "repo exact packet preparation name mismatch: "
                f"requested={data_name} prepared={prepared.get('dataName', '')}")
        versioned_prefix = (
            data_name.rsplit("/seg=", 1)[0]
            if "/seg=" in data_name else data_name
        )
        prepared_prefixes = getattr(self, "_prepared_packet_prefixes", set())
        if versioned_prefix not in prepared_prefixes:
            # The Repo has just advertised this app-owned prefix through NLSR.
            # Wait once for route propagation; later segments reuse the route.
            time.sleep(min(5.0, max(0.0, attempt_timeout_ms / 2000.0)))
            prepared_prefixes.add(versioned_prefix)
            self._prepared_packet_prefixes = prepared_prefixes
        raw_forwarding_hints = prepared.get("forwardingHints", [])
        if not isinstance(raw_forwarding_hints, list):
            raise ValueError(
                "repo exact packet preparation forwardingHints must be a list")
        forwarding_hints = [
            str(hint) for hint in raw_forwarding_hints if str(hint)
        ]
        packet = fetch_exact_data_packet(
            data_name,
            timeout_ms=attempt_timeout_ms,
            interest_lifetime_ms=_large_data_interest_lifetime_ms(),
            forwarding_hints=forwarding_hints,
        )
        expected_hash = str(prepared.get("wireSha256", ""))
        actual_hash = hashlib.sha256(packet.wire).hexdigest()
        if expected_hash and actual_hash != expected_hash:
            raise ValueError(
                f"repo exact packet wire hash mismatch for {data_name}: "
                f"expected={expected_hash} actual={actual_hash}")
        return packet

    def fetch_signed_packets(
        self,
        manifest: RepoObjectManifest,
        *,
        repo_node: str = "",
    ) -> list[DataPacket]:
        """Fetch one complete app-produced packet set in manifest order.

        Each replica attempt starts a fresh local result. A missing or invalid
        packet therefore fails that replica atomically instead of exposing a
        partial packet set to the caller.
        """

        if not hasattr(self, "_client_local"):
            self._client_local = threading.local()
        packet_names = tuple(manifest.packet_names)
        if not packet_names:
            raise ValueError(
                f"repo packet index is empty: {manifest.object_name}")
        if len(packet_names) != manifest.segment_count:
            raise ValueError(
                "repo packet index count mismatch: "
                f"object={manifest.object_name} names={len(packet_names)} "
                f"segments={manifest.segment_count}")
        if len(set(packet_names)) != len(packet_names):
            raise ValueError(
                f"repo packet index contains duplicate names: {manifest.object_name}")

        candidates = self._ordered_replicas(
            (repo_node,) if repo_node else tuple(manifest.replica_nodes))
        if not candidates:
            raise ValueError(
                f"repo packet manifest has no replicas: {manifest.object_name}")

        last_error: Exception | None = None
        deadline = time.monotonic() + max(
            getattr(self, "timeout_ms", 30_000), 30_000) / 1000.0
        for index, candidate in enumerate(candidates):
            packets: list[DataPacket] = []
            started = time.monotonic()
            try:
                remaining_ms = max(1, int((deadline - started) * 1000))
                remaining_replicas = max(1, len(candidates) - index)
                self._client_local.attempt_timeout_ms = max(
                    500, remaining_ms // remaining_replicas)
                for expected_name in packet_names:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("repo total read deadline exceeded")
                    packet = self.fetch_packet(candidate, expected_name)
                    decoded = decode_data_packet(packet.wire)
                    if packet.name != expected_name or decoded.name != expected_name:
                        raise ValueError(
                            "repo exact packet name mismatch: "
                            f"expected={expected_name} packet={packet.name} "
                            f"wire={decoded.name}")
                    packets.append(DataPacket(
                        decoded.name, decoded.segment, bytes(decoded.wire)))
                self._record_replica_result(
                    candidate, success=True,
                    latency_ms=(time.monotonic() - started) * 1000.0)
                return packets
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                self._record_replica_result(candidate, success=False)
            finally:
                if hasattr(self._client_local, "attempt_timeout_ms"):
                    del self._client_local.attempt_timeout_ms
        raise RuntimeError(
            f"no repo replica could serve exact packet set "
            f"{manifest.object_name}: {last_error}") from last_error

    @staticmethod
    def _parse_ack_payload(payload: bytes) -> dict[str, object]:
        decoded = decode_provider_capability_ack(
            payload, service_name="/NDNSF/DistributedRepo")
        capability_hint = decoded.hint
        fields: dict[str, object] = dict(capability_hint.service_payload)
        fields["repoNode"] = capability_hint.provider_name
        return fields

    @staticmethod
    def _ndn_uri(name: str) -> str:
        return "ndn:" + name if name.startswith("/") else "ndn:/" + name

    @staticmethod
    def data_name(repo_node: str, object_name: str) -> str:
        return RepoNodeApp.data_name(repo_node, object_name)

    def _serve_object(self, name: str, payload: bytes) -> SegmentedObjectProducer:
        producer = SegmentedObjectProducer(
            name,
            payload,
            signing_identity=self.user.user,
            max_segment_size=6000,
            freshness_ms=60000,
        ).start()
        time.sleep(0.2)
        return producer

    def _catch_chunks(self, name: str, timeout_s: int = 30) -> bytes:
        return fetch_segmented_object(
            name,
            timeout_ms=timeout_s * 1000,
            interest_lifetime_ms=_large_data_interest_lifetime_ms(),
            init_cwnd=8.0,
        )

    def _select_replicas_from_acks(
        self,
        candidates: list[AckCandidate],
        replication_factor: int,
        object_size: int,
    ) -> list[str]:
        capabilities = []
        provider_for_repo: dict[str, str] = {}
        for candidate in candidates:
            if not candidate.status:
                continue
            fields = self._parse_ack_payload(candidate.payload)
            repo_node = fields.get("repoNode", "")
            if not repo_node:
                continue
            try:
                free_bytes = int(fields.get("freeBytes", "0"))
                used_bytes = int(fields.get("usedBytes", "0"))
                recent_load = float(fields.get("load", "0"))
                availability = float(fields.get("availability", "1"))
            except ValueError:
                continue
            capabilities.append(StorageCapability(
                repo_node=repo_node,
                free_bytes=free_bytes,
                used_bytes=used_bytes,
                recent_load=recent_load,
                availability_score=availability,
                failure_domain=fields.get("failureDomain", ""),
                repo_mode=fields.get("repoMode", "persistent"),
                accepts_backup_replica=str(
                    fields.get("acceptsBackupReplica", "1")
                ).lower() not in {"0", "false", "no", "off"},
                queue_depth=int(fields.get("queueDepth", 0) or 0),
                inflight_operations=(
                    int(fields.get("inflightReads", 0) or 0) +
                    int(fields.get("inflightWrites", 0) or 0) +
                    int(fields.get("inflightRepair", 0) or 0)
                ),
                storage_latency_ms=max(
                    float(fields.get("storageReadLatencyMs", 0) or 0),
                    float(fields.get("storageWriteLatencyMs", 0) or 0),
                ),
                network_rtt_ms=float(fields.get("networkRttMs", 0) or 0),
                network_bandwidth_mbps=float(
                    fields.get("networkBandwidthMbps", 0) or 0),
            ))
            provider_for_repo[repo_node] = candidate.provider_name

        replicas = select_replicas(
            capabilities,
            PlacementPolicy(replication_factor=replication_factor),
            object_size,
        )
        return [provider_for_repo[replica.repo_node] for replica in replicas]

    def _select_repo_nodes(
        self,
        *,
        object_name: str,
        object_size: int,
        replication_factor: int,
        replica_nodes: tuple[str, ...] = (),
    ) -> list[str]:
        if replica_nodes:
            return list(replica_nodes)[:replication_factor]
        if self._placement_cache_valid(replication_factor):
            cached = self._placement_cache[:replication_factor]
            self._log(f"repo select cache object={object_name} selected={cached}")
            return cached

        selected_repo_nodes: list[str] = []

        def selector(candidates: list[AckCandidate]) -> list[str]:
            selected_providers = self._select_replicas_from_acks(
                candidates,
                replication_factor,
                object_size,
            )
            selected_repo_nodes.clear()
            provider_to_repo = {
                candidate.provider_name:
                self._parse_ack_payload(candidate.payload).get("repoNode", "")
                for candidate in candidates
            }
            selected_repo_nodes.extend(
                provider_to_repo[provider]
                for provider in selected_providers
                if provider_to_repo.get(provider)
            )
            return selected_providers

        response = self._control_call(lambda: self.user.request_service_select(
            self._service_for("CAPABILITY"),
            encode_repo_request("CAPABILITY", objectName=object_name),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        ))
        if not response.status:
            raise RuntimeError(response.error)
        if len(selected_repo_nodes) >= replication_factor:
            self._placement_cache = list(selected_repo_nodes)
            self._placement_cache_updated_ms = self._now_ms()
        return selected_repo_nodes[:replication_factor]

    def _request_specific_repo_normal(
        self,
        *,
        repo_node: str,
        payload: bytes,
        timeout_ms: int | None = None,
    ) -> ServiceResponse:
        def selector(candidates: list[AckCandidate]) -> list[str]:
            for candidate in candidates:
                fields = self._parse_ack_payload(candidate.payload)
                if fields.get("repoNode") == repo_node:
                    return [candidate.provider_name]
            return []

        self._metric_increment("normalCalls")
        response = self._control_call(lambda: self.user.request_service_select(
            self._service_for_payload(payload),
            payload,
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms or self.timeout_ms,
            request_strategy="all-selected",
        ))
        if not response.status:
            raise RuntimeError(response.error)
        if self.verbose and response.payload:
            self._log(
                f"repo specific response repo={repo_node} bytes={len(response.payload)}"
            )
        return response

    def _request_specific_repos_parallel(
        self,
        payload_by_repo: dict[str, bytes],
        *,
        timeout_ms: int | None = None,
    ) -> tuple[dict[str, ServiceResponse], dict[str, str]]:
        ordered_repos = list(payload_by_repo)
        if not ordered_repos:
            return {}, {}
        total_timeout_ms = max(1, int(timeout_ms or self.timeout_ms))
        self._metric_increment("replicaFanouts")

        targeted_async = getattr(
            self.user, "request_service_targeted_async", None)
        if self.control_mode == "normal" or not callable(targeted_async):
            responses: dict[str, ServiceResponse] = {}
            failures: dict[str, str] = {}
            if self.control_mode == "targeted":
                self._metric_increment("targetedFallbacks", len(ordered_repos))
            for repo_node in ordered_repos:
                call_started = time.monotonic()
                try:
                    responses[repo_node] = self._request_specific_repo_normal(
                        repo_node=repo_node,
                        payload=payload_by_repo[repo_node],
                        timeout_ms=total_timeout_ms,
                    )
                    self._record_replica_result(
                        repo_node, success=True,
                        latency_ms=(time.monotonic() - call_started) * 1000.0)
                except Exception as exc:  # noqa: BLE001
                    failures[repo_node] = str(exc)
                    self._record_replica_result(
                        repo_node, success=False, definitive_failure=True)
            return responses, failures

        condition = threading.Condition()
        state = {
            "condition": condition,
            "pending": set(ordered_repos),
            "responses": {},
            "failures": {},
            "accepting": True,
            "closed": False,
            "started": {},
        }
        with self._fanout_states_lock:
            self._fanout_states.append(state)

        fallback_budget_ms = (
            max(500, int(total_timeout_ms * 0.30))
            if self.enable_targeted_fallback else 0)
        targeted_budget_ms = max(1, total_timeout_ms - fallback_budget_ms)
        started = time.monotonic()

        def complete(repo_node: str, response=None, error: str = "") -> None:
            with condition:
                if not state["accepting"] or repo_node not in state["pending"]:
                    return
                state["pending"].remove(repo_node)
                self._metric_end_replica_call()
                if response is not None and response.status:
                    state["responses"][repo_node] = response
                    self._metric_increment("targetedAsyncCompleted")
                    self._record_replica_result(
                        repo_node, success=True,
                        latency_ms=(time.monotonic() - state["started"].get(
                            repo_node, started)) * 1000.0)
                else:
                    state["failures"][repo_node] = (
                        error or getattr(response, "error", "targeted request failed"))
                    self._record_replica_result(repo_node, success=False)
                condition.notify_all()

        def submit_all() -> None:
            for repo_node in ordered_repos:
                try:
                    self._metric_increment("targetedCalls")
                    self._metric_increment("targetedAsyncSubmitted")
                    self._metric_begin_replica_call()
                    state["started"][repo_node] = time.monotonic()
                    targeted_async(
                        repo_node,
                        self._service_for_payload(payload_by_repo[repo_node]),
                        payload_by_repo[repo_node],
                        on_response=(
                            lambda response, repo=repo_node:
                            complete(repo, response=response)),
                        on_timeout=(
                            lambda request_id, repo=repo_node:
                            complete(repo, error=f"timeout: {request_id}")),
                        timeout_ms=targeted_budget_ms,
                    )
                except Exception as exc:  # noqa: BLE001
                    complete(repo_node, error=str(exc))

        try:
            self._control_call(submit_all)
            targeted_deadline = started + targeted_budget_ms / 1000.0
            with condition:
                while state["pending"] and not state["closed"]:
                    remaining = targeted_deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    condition.wait(remaining)
                for repo_node in list(state["pending"]):
                    state["pending"].remove(repo_node)
                    state["failures"][repo_node] = "targeted total deadline"
                    self._metric_end_replica_call()
                    self._metric_increment("targetedTimeouts")
                    self._record_replica_result(repo_node, success=False)
                state["accepting"] = False

            responses = dict(state["responses"])
            failures = dict(state["failures"])
        finally:
            with condition:
                state["accepting"] = False
            with self._fanout_states_lock:
                if state in self._fanout_states:
                    self._fanout_states.remove(state)

        if failures and self.enable_targeted_fallback and not self._closed.is_set():
            overall_deadline = started + total_timeout_ms / 1000.0
            failed_repos = [repo for repo in ordered_repos if repo in failures]
            for index, repo_node in enumerate(failed_repos):
                remaining_ms = int((overall_deadline - time.monotonic()) * 1000)
                if remaining_ms <= 0:
                    break
                attempt_ms = max(1, remaining_ms // (len(failed_repos) - index))
                self._metric_increment("targetedFallbacks")
                try:
                    responses[repo_node] = self._request_specific_repo_normal(
                        repo_node=repo_node,
                        payload=payload_by_repo[repo_node],
                        timeout_ms=attempt_ms,
                    )
                    failures.pop(repo_node, None)
                    self._record_replica_result(repo_node, success=True)
                except Exception as exc:  # noqa: BLE001
                    failures[repo_node] = str(exc)
                    self._record_replica_result(
                        repo_node, success=False, definitive_failure=True)

        ordered_responses = {
            repo: responses[repo] for repo in ordered_repos if repo in responses}
        ordered_failures = {
            repo: failures[repo] for repo in ordered_repos if repo in failures}
        return ordered_responses, ordered_failures

    def _request_specific_repo(
        self,
        *,
        repo_node: str,
        payload: bytes,
        timeout_ms: int | None = None,
        isolated_runtime: bool = False,
    ) -> ServiceResponse:
        # isolated_runtime remains accepted for source compatibility. Creating
        # a second ServiceUser with the same identity/SVS session is unsafe.
        _ = isolated_runtime
        responses, failures = self._request_specific_repos_parallel(
            {repo_node: payload}, timeout_ms=timeout_ms)
        if repo_node in responses:
            response = responses[repo_node]
            if self.verbose and response.payload:
                self._log(
                    f"repo specific response repo={repo_node} "
                    f"bytes={len(response.payload)}")
            return response
        raise RuntimeError(failures.get(repo_node, "repo request failed"))

    def _reserve_replicas(
        self,
        repo_nodes: Iterable[str],
        operation_id: str,
        reserved_bytes: int,
        required_reservations: int | None = None,
    ) -> dict[str, RepoCapacityReservation]:
        if not getattr(self, "enable_capacity_reservations", False):
            return {}
        started = time.monotonic()
        ordered_repo_nodes = list(repo_nodes)
        required = (
            len(ordered_repo_nodes)
            if required_reservations is None else int(required_reservations))
        if required < 1 or required > len(ordered_repo_nodes):
            raise ValueError(
                "required Repo reservations must be within selected replicas")
        reservations: dict[str, RepoCapacityReservation] = {}
        try:
            payload_by_repo = {}
            for repo_node in ordered_repo_nodes:
                reservation_id = hashlib.sha256(
                    f"{operation_id}|{repo_node}".encode()).hexdigest()
                payload_by_repo[repo_node] = encode_repo_request(
                        "RESERVE_CAPACITY",
                        reservationId=reservation_id,
                        operationId=operation_id,
                        reservedBytes=max(1, int(reserved_bytes)),
                        ttlMs=max(30_000, self.timeout_ms * 2),
                    )
            responses, failures = self._request_specific_repos_parallel(
                payload_by_repo)
            for repo_node, response in responses.items():
                try:
                    obj = json.loads(response.payload.decode())
                    reservations[repo_node] = RepoCapacityReservation(
                        reservation_id=str(obj["reservationId"]),
                        operation_id=str(obj["operationId"]),
                        repo_node=str(obj["repoNode"]),
                        reserved_bytes=int(obj["reservedBytes"]),
                        state=str(obj["state"]),
                        expires_at_ms=int(obj["expiresAtMs"]),
                    )
                except Exception as exc:  # noqa: BLE001
                    failures[repo_node] = str(exc)
            if len(reservations) < required:
                raise RuntimeError(
                    "repo capacity reservation failed: "
                    f"confirmed={len(reservations)} required={required} "
                    f"failures={failures}")
            return reservations
        except Exception:
            self._release_reservations_parallel(reservations)
            raise
        finally:
            self._record_control_phase(
                "reserve", (time.monotonic() - started) * 1000.0)

    def _release_reservations_parallel(
        self,
        reservations: dict[str, RepoCapacityReservation],
    ) -> dict[str, str]:
        if not reservations:
            return {}
        payload_by_repo = {
            repo_node: encode_repo_request(
                "RELEASE_CAPACITY",
                reservationId=reservation.reservation_id,
                operationId=reservation.operation_id,
            )
            for repo_node, reservation in reservations.items()
        }
        _, failures = self._request_specific_repos_parallel(payload_by_repo)
        return failures

    def _store_once(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        operation: str = "STORE_PACKETS",
        manifest_override: RepoObjectManifest | None = None,
        packet_data_name: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        packets: list[DataPacket] = []
        effective_packet_data_name = (
            packet_data_name or RepoNodeApp.object_data_name(object_name))
        if operation == "STORE_PACKETS":
            packets = make_segmented_data_packets(
                effective_packet_data_name,
                payload,
                signing_identity=self.user.user,
                max_segment_size=6000,
                freshness_ms=60000,
            )

        required_acks = (
            manifest_override.required_write_acks
            if manifest_override is not None else replication_factor)
        selected_repo_nodes = self._select_repo_nodes(
            object_name=object_name,
            object_size=len(payload),
            replication_factor=replication_factor,
            replica_nodes=replica_nodes,
        )
        healthy_repo_nodes = [
            repo_node for repo_node in selected_repo_nodes
            if not self._replica_in_cooldown(repo_node)]
        if len(healthy_repo_nodes) >= required_acks:
            selected_repo_nodes = healthy_repo_nodes
        if len(selected_repo_nodes) < required_acks:
            raise RuntimeError(
                f"repo store selected {len(selected_repo_nodes)} replicas, "
                f"need {required_acks} acknowledgements")
        operation_id = (
            manifest_override.operation_id
            if manifest_override is not None and manifest_override.operation_id
            else str(uuid.uuid4())
        )
        reservations = self._reserve_replicas(
            selected_repo_nodes, operation_id, len(payload), required_acks)
        store_repo_nodes = (
            list(reservations)
            if getattr(self, "enable_capacity_reservations", False)
            else list(selected_repo_nodes))
        if len(store_repo_nodes) < required_acks:
            self._release_reservations_parallel(reservations)
            raise RuntimeError(
                f"repo store has {len(store_repo_nodes)} reserved replicas, "
                f"need {required_acks} acknowledgements")
        manifest = manifest_override or RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
            segment_count=len(packets) if packets else 1,
            replication_factor=replication_factor,
            replica_nodes=tuple(selected_repo_nodes),
            replica_data_names=(
                tuple(effective_packet_data_name for _ in selected_repo_nodes)
                if packets else ()
            ),
            packet_names=tuple(packet.name for packet in packets),
            policy_epoch=policy_epoch,
            metadata=dict(metadata or {}),
            operation_id=operation_id,
            lifecycle_state="RUNNING",
            confirmed_replica_nodes=(),
        )
        manifest = replace(
            manifest,
            replica_nodes=tuple(selected_repo_nodes),
            replica_data_names=(
                tuple(effective_packet_data_name for _ in selected_repo_nodes)
                if packets else manifest.replica_data_names
            ),
            operation_id=operation_id,
            lifecycle_state="RUNNING",
            confirmed_replica_nodes=(),
        )
        intent = RepoWriteIntent(
            operation_id=operation_id,
            object_name=manifest.object_name,
            generation=manifest.generation,
            expected_generation=manifest.parent_generation,
            digest=manifest.sha256,
            replication_factor=manifest.replication_factor,
            required_acks=manifest.required_write_acks,
            consistency=manifest.write_consistency,
            selected_replicas=tuple(store_repo_nodes),
        )
        packet_fields = {}
        if operation == "STORE_PACKETS":
            packet_fields["packets"] = [
                {
                    "name": packet.name,
                    "segment": packet.segment,
                    "segmentName": packet.name,
                    "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
                    "wireB64": base64.b64encode(packet.wire).decode(),
                }
                for packet in packets
            ]
        request_payload = encode_repo_request(
                operation,
                manifest=manifest.to_dict(),
                writeIntent=intent.to_dict(),
                **({
                    "payloadB64": base64.b64encode(payload).decode()
                } if operation == "STORE" else {}),
                **packet_fields,
            )
        receipts: list[RepoWriteReceipt] = []
        store_started = time.monotonic()
        responses, failures = self._request_specific_repos_parallel({
            repo_node: request_payload for repo_node in store_repo_nodes
        })
        try:
            for repo_node, response in responses.items():
                try:
                    response_obj = json.loads(response.payload.decode())
                    receipts.append(RepoWriteReceipt.from_dict(response_obj["writeReceipt"]))
                except Exception as exc:  # noqa: BLE001
                    failures[repo_node] = str(exc)
            validated = validate_write_receipts(intent, receipts, failures=failures)
        except Exception:
            self._release_reservations_parallel(reservations)
            raise
        finally:
            self._record_control_phase(
                "store", (time.monotonic() - store_started) * 1000.0)
        confirmed = tuple(receipt.repo_node for receipt in validated)
        committed_manifest = replace(
            manifest,
            replica_nodes=confirmed,
            replica_data_names=(
                tuple(effective_packet_data_name for _ in confirmed)
                if packets else manifest.replica_data_names
            ),
            confirmed_replica_nodes=confirmed,
            lifecycle_state="COMMITTED",
        )
        return self._finalize_receipt_quorum(
            intent, committed_manifest, validated)

    def _finalize_receipt_quorum(
        self,
        intent: RepoWriteIntent,
        manifest: RepoObjectManifest,
        receipts: Iterable[RepoWriteReceipt],
    ) -> RepoObjectManifest:
        validated = validate_write_receipts(intent, receipts)
        confirmed = tuple(receipt.repo_node for receipt in validated)
        metadata = dict(manifest.metadata or {})
        metadata["quorumFinalized"] = True
        replica_data_names = tuple(manifest.replica_data_names)
        if replica_data_names and len(replica_data_names) != len(confirmed):
            replica_data_names = tuple(replica_data_names[0] for _ in confirmed)
        finalized_manifest = replace(
            manifest,
            replica_nodes=confirmed,
            replica_data_names=replica_data_names,
            confirmed_replica_nodes=confirmed,
            lifecycle_state="COMMITTED",
            metadata=metadata,
        )
        if intent.required_acks <= 1:
            return finalized_manifest
        payload = encode_repo_request(
            "FINALIZE_WRITE",
            manifest=finalized_manifest.to_dict(),
            writeIntent=intent.to_dict(),
            writeReceipts=[receipt.to_dict() for receipt in validated],
        )
        responses, failures = self._request_specific_repos_parallel({
            repo_node: payload for repo_node in confirmed
        })
        finalized_repos = []
        for repo_node, response in responses.items():
            try:
                decoded = json.loads(response.payload.decode())
                if str(decoded.get("status", "")) != "finalized":
                    raise ValueError("repo finalize response is not finalized")
                finalized_repos.append(repo_node)
            except Exception as exc:  # noqa: BLE001
                failures[repo_node] = str(exc)
        if not finalized_repos:
            raise RepoIncompleteWriteError(intent, (), failures=failures)
        return finalized_manifest

    def store(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        if len(payload) <= self.max_segment_payload:
            return self._store_once(
                object_name=object_name,
                payload=payload,
                object_type=object_type,
                replication_factor=replication_factor,
                replica_nodes=replica_nodes,
                policy_epoch=policy_epoch,
                metadata=metadata,
            )

        segment_manifests: list[RepoObjectManifest] = []
        for index in range(0, len(payload), self.max_segment_payload):
            segment_index = len(segment_manifests)
            segment_payload = payload[index:index + self.max_segment_payload]
            segment_manifest = self._store_once(
                object_name=f"{object_name}/seg/{segment_index}",
                payload=segment_payload,
                object_type=f"{object_type}.segment",
                replication_factor=replication_factor,
                replica_nodes=replica_nodes,
                policy_epoch=policy_epoch,
                metadata=metadata,
            )
            segment_manifests.append(segment_manifest)

        replica_set: list[str] = []
        for segment_manifest in segment_manifests:
            for repo_node in segment_manifest.replica_nodes:
                if repo_node not in replica_set:
                    replica_set.append(repo_node)
        manifest = RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
            segment_count=len(segment_manifests),
            replication_factor=replication_factor,
            replica_nodes=tuple(replica_set),
            policy_epoch=policy_epoch,
            metadata=dict(metadata or {}),
        )
        return self._store_once(
            object_name=object_name,
            payload=b"",
            object_type=object_type,
            replication_factor=replication_factor,
            replica_nodes=tuple(replica_set[:replication_factor]) or replica_nodes,
            policy_epoch=policy_epoch,
            operation="STORE_MANIFEST",
            manifest_override=manifest,
            metadata=metadata,
        )

    def store_object(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        object_name = self._require_publisher_object_name(object_name)
        operation_id = str(uuid.uuid4())

        def select_replicas_once() -> list[str]:
            if self._placement_cache_valid(replication_factor):
                cached = self._placement_cache[:replication_factor]
                self._log(
                    f"repo select cache object={object_name} selected={cached}",
                )
                return cached
            selected_repo_nodes: list[str] = []
            self._log(
                f"repo select start object={object_name} "
                f"replicas={replication_factor} bytes={len(payload)}",
            )
            def selector(candidates: list[AckCandidate]) -> list[str]:
                selected_providers = self._select_replicas_from_acks(
                    candidates,
                    replication_factor,
                    len(payload),
                )
                selected_repo_nodes.clear()
                provider_to_repo = {
                    candidate.provider_name:
                    self._parse_ack_payload(candidate.payload).get("repoNode", "")
                    for candidate in candidates
                }
                selected_repo_nodes.extend(
                    provider_to_repo[provider]
                    for provider in selected_providers
                    if provider_to_repo.get(provider)
                )
                return selected_providers

            self._log(f"repo select request object={object_name}")
            response = self._control_call(lambda: self.user.request_service_select(
                self._service_for("CAPABILITY"),
                encode_repo_request("CAPABILITY", objectName=object_name),
                selector,
                ack_timeout_ms=self.ack_timeout_ms,
                timeout_ms=self.timeout_ms,
                request_strategy="all-selected",
            ))
            if not response.status:
                raise RuntimeError(response.error)
            self._log(
                f"repo select done object={object_name} selected={selected_repo_nodes}",
            )
            if len(selected_repo_nodes) >= replication_factor:
                self._placement_cache = list(selected_repo_nodes)
                self._placement_cache_updated_ms = self._now_ms()
            return selected_repo_nodes

        last_error = ""
        for attempt in range(3):
            try:
                selected = list(replica_nodes) if replica_nodes else select_replicas_once()
                if len(selected) < replication_factor:
                    if not replica_nodes:
                        self._placement_cache = []
                    raise RuntimeError(
                        f"repo store selected {len(selected)} replicas, "
                        f"need {replication_factor}")
                selected = selected[:replication_factor]
                self._reserve_replicas(selected, operation_id, len(payload))
                use_pull_store = len(payload) >= self.pull_store_threshold_bytes
                data_names = tuple(
                    self._upload_data_name(repo_node, object_name)
                    if use_pull_store else
                    self.data_name(repo_node, object_name)
                    for repo_node in selected
                )
                segment_locations: list[dict] = []
                final_manifest = RepoObjectManifest(
                    object_name=object_name,
                    object_type=object_type,
                    sha256=hashlib.sha256(payload).hexdigest(),
                    size=len(payload),
                    segment_count=0,
                    replication_factor=replication_factor,
                    replica_nodes=tuple(selected),
                    replica_data_names=data_names,
                    policy_epoch=policy_epoch,
                    metadata=dict(metadata or {}),
                    operation_id=operation_id,
                    lifecycle_state="RUNNING",
                    confirmed_replica_nodes=(),
                )
                segment_count = 0
                producers: list[object] = []
                for repo_node, data_name in zip(selected, data_names):
                    packets = make_segmented_data_packets(
                        data_name,
                        payload,
                        signing_identity=self.user.user,
                        max_segment_size=4000,
                        freshness_ms=60000,
                    )
                    segment_count = max(segment_count, len(packets))
                    self._log(
                        f"repo store target={repo_node} "
                        f"object={object_name} packets={len(packets)}",
                    )
                    target_manifest = RepoObjectManifest(
                        object_name=object_name,
                        object_type=object_type,
                        sha256=final_manifest.sha256,
                        size=final_manifest.size,
                        segment_count=len(packets),
                        replication_factor=replication_factor,
                        replica_nodes=(repo_node,),
                        replica_data_names=(data_name,),
                        policy_epoch=policy_epoch,
                        metadata=dict(metadata or {}),
                    )
                    location_hints = [] if data_name.startswith(
                        repo_node.rstrip("/") + "/"
                    ) else [repo_node]
                    segment_locations.append({
                        "start": 0,
                        "end": len(packets) - 1,
                        "dataName": data_name,
                        "repoNode": repo_node,
                        "hints": location_hints,
                        "routeStrategy": "hint-first",
                    })
                    if use_pull_store:
                        packet_manifest = json.dumps({
                            "objectName": object_name,
                            "dataName": data_name,
                            "packets": [
                                {
                                    "name": packet.name,
                                    "segment": packet.segment,
                                    "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
                                }
                                for packet in packets
                            ],
                        }, sort_keys=True).encode()
                        data_producer = StoredDataProducer(
                            data_name,
                            [packet.wire for packet in packets],
                            signing_identity=self.user.user,
                        ).start()
                        producers.append(data_producer)
                        packet_manifest_name = self._packet_manifest_name(
                            repo_node,
                            object_name,
                        )
                        manifest_producer = SegmentedObjectProducer(
                            packet_manifest_name,
                            packet_manifest,
                            signing_identity=self.user.user,
                            max_segment_size=6000,
                            freshness_ms=60000,
                        ).start()
                        producers.append(manifest_producer)
                        time.sleep(0.2)
                        response = self._control_call(lambda: self.user.request_service(
                            self._service_for("STORE_PACKET_PULL"),
                            encode_repo_request(
                                "STORE_PACKET_PULL",
                                manifest=target_manifest.to_dict(),
                                sourceName=data_name,
                                packetManifestName=manifest_producer.versioned_name,
                                packetManifestSha256=hashlib.sha256(
                                    packet_manifest
                                ).hexdigest(),
                            ),
                            ack_timeout_ms=self.ack_timeout_ms,
                            timeout_ms=max(
                                self.timeout_ms,
                                _pull_fetch_timeout_ms(target_manifest.segment_count) + 30000,
                            ),
                            strategy="first-responding",
                        ))
                        if not response.status:
                            raise RuntimeError(response.error)
                        continue

                    for packet in packets:
                        self._log(
                            f"repo store packet target={repo_node} "
                            f"segment={packet.segment} name={packet.name}",
                        )
                        response = self._control_call(lambda: self.user.request_service(
                            self._service_for("STORE_PACKET"),
                            encode_repo_request(
                                "STORE_PACKET",
                                manifest=target_manifest.to_dict(),
                                packet=self._packet_to_request(packet),
                            ),
                            ack_timeout_ms=self.ack_timeout_ms,
                            timeout_ms=self.timeout_ms,
                            strategy="first-responding",
                        ))
                        if not response.status:
                            raise RuntimeError(response.error)
                        self._log(
                            f"repo stored packet target={repo_node} "
                            f"segment={packet.segment}",
                        )
                for producer in producers:
                    try:
                        producer.stop()
                    except Exception:
                        pass
                prepared_manifest = RepoObjectManifest(
                    object_name=final_manifest.object_name,
                    object_type=final_manifest.object_type,
                    sha256=final_manifest.sha256,
                    size=final_manifest.size,
                    segment_count=segment_count,
                    replication_factor=final_manifest.replication_factor,
                    replica_nodes=final_manifest.replica_nodes,
                    replica_data_names=final_manifest.replica_data_names,
                    segment_locations=tuple(segment_locations),
                    policy_epoch=final_manifest.policy_epoch,
                    object_class=final_manifest.object_class,
                    ttl_ms=final_manifest.ttl_ms,
                    repair_allowed=final_manifest.repair_allowed,
                    metadata=dict(final_manifest.metadata or {}),
                    operation_id=operation_id,
                    lifecycle_state="RUNNING",
                    confirmed_replica_nodes=(),
                )
                intent = RepoWriteIntent(
                    operation_id=operation_id,
                    object_name=prepared_manifest.object_name,
                    generation=prepared_manifest.generation,
                    expected_generation=prepared_manifest.parent_generation,
                    digest=prepared_manifest.sha256,
                    replication_factor=replication_factor,
                    required_acks=prepared_manifest.required_write_acks,
                    consistency=prepared_manifest.write_consistency,
                    selected_replicas=tuple(selected),
                )
                receipts: list[RepoWriteReceipt] = []
                failures: dict[str, str] = {}
                for repo_node in selected:
                    try:
                        response = self._request_specific_repo(
                            repo_node=repo_node,
                            payload=encode_repo_request(
                                "COMMIT_PACKET_SET",
                                manifest=prepared_manifest.to_dict(),
                                writeIntent=intent.to_dict(),
                            ),
                            timeout_ms=max(self.timeout_ms, 60000),
                        )
                        response_obj = json.loads(response.payload.decode())
                        receipts.append(RepoWriteReceipt.from_dict(
                            response_obj["writeReceipt"]))
                    except Exception as exc:  # noqa: BLE001
                        failures[repo_node] = str(exc)
                validated = validate_write_receipts(intent, receipts, failures=failures)
                confirmed = tuple(receipt.repo_node for receipt in validated)
                return replace(
                    prepared_manifest,
                    replica_nodes=confirmed,
                    confirmed_replica_nodes=confirmed,
                    lifecycle_state="COMMITTED",
                )
            except Exception as exc:  # noqa: BLE001
                last_error = str(exc)
            time.sleep(0.2 * (attempt + 1))
        raise RuntimeError(
            f"repo store failed for {object_name}: {last_error}"
        )

    def store_signed_packets(
        self,
        *,
        object_name: str,
        packets: list[DataPacket],
        object_type: str,
        object_size: int,
        object_sha256: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        data_name: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        """Store app-produced signed NDN Data packets without re-signing them.

        The application remains responsible for segmentation, signatures,
        payload encryption, and the object-level hash. The repo verifies only
        that each submitted packet name and wire hash matches the request
        metadata, then stores the signed Data wire bytes as-is.
        """

        if not packets:
            raise ValueError("store_signed_packets requires at least one packet")
        object_name = self._require_publisher_object_name(object_name)
        data_name = data_name or self._packet_data_name(packets)
        normalized_data_prefix = data_name.rstrip("/")
        for packet in packets:
            if (packet.name != normalized_data_prefix and
                    not packet.name.startswith(normalized_data_prefix + "/")):
                raise ValueError(
                    "signed Data packet name is outside the declared original "
                    f"Data prefix: prefix={data_name} packet={packet.name}"
                )
        versioned_data_name = self._packet_versioned_data_name(packets)
        selected = self._select_repo_nodes(
            object_name=object_name,
            object_size=object_size,
            replication_factor=replication_factor,
            replica_nodes=replica_nodes,
        )
        if len(selected) < replication_factor:
            raise RuntimeError(
                f"repo store selected {len(selected)} replicas, "
                f"need {replication_factor}")
        selected = selected[:replication_factor]
        operation_id = str(uuid.uuid4())
        self._reserve_replicas(selected, operation_id, object_size)
        manifest = RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=object_sha256,
            size=object_size,
            segment_count=len(packets),
            replication_factor=replication_factor,
            replica_nodes=tuple(selected),
            replica_data_names=tuple(data_name for _ in selected),
            packet_names=tuple(packet.name for packet in packets),
            segment_locations=({
                "start": 0,
                "end": len(packets) - 1,
                "dataName": data_name,
                "versionedDataName": versioned_data_name,
                "repoNodes": selected,
                "hints": selected,
                "routeStrategy": "direct-first",
            },),
            policy_epoch=policy_epoch,
            metadata=dict(metadata or {}),
            operation_id=operation_id,
            lifecycle_state="RUNNING",
            confirmed_replica_nodes=(),
        )

        for repo_node in selected:
            target_manifest = RepoObjectManifest(
                object_name=object_name,
                object_type=object_type,
                sha256=object_sha256,
                size=object_size,
                segment_count=len(packets),
                replication_factor=replication_factor,
                replica_nodes=(repo_node,),
                replica_data_names=(data_name,),
                # The Repo derives and accumulates exact names from each wire
                # batch. Repeating the full name list in every control request
                # can exceed the NDN packet limit for large objects.
                packet_names=(),
                segment_locations=manifest.segment_locations,
                policy_epoch=policy_epoch,
                metadata=dict(metadata or {}),
            )
            for batch_index, batch in enumerate(self._packet_batches(packets)):
                batch_operation_id = f"{operation_id}:batch:{batch_index}"
                batch_manifest = replace(
                    target_manifest,
                    operation_id=batch_operation_id,
                    lifecycle_state="RUNNING",
                    confirmed_replica_nodes=(),
                )
                batch_intent = RepoWriteIntent(
                    operation_id=batch_operation_id,
                    object_name=object_name,
                    generation=manifest.generation,
                    expected_generation=-1,
                    digest=object_sha256,
                    replication_factor=1,
                    required_acks=1,
                    consistency=WriteConsistency.ALL.value,
                    selected_replicas=(repo_node,),
                )
                response = self._request_specific_repo(
                    repo_node=repo_node,
                    payload=encode_repo_request(
                        "STORE_PACKET_BATCH",
                        manifest=batch_manifest.to_dict(),
                        writeIntent=batch_intent.to_dict(),
                        packets=[self._packet_to_request(packet) for packet in batch],
                    ),
                    timeout_ms=max(self.timeout_ms, 60000),
                    # Reuse the existing ServiceUser across packet batches.
                    # Each request has its own request ID; rebuilding the SVS
                    # runtime per packet adds seconds of bootstrap latency.
                    isolated_runtime=False,
                )
                response_obj = json.loads(response.payload.decode())
                validate_write_receipts(
                    batch_intent,
                    [RepoWriteReceipt.from_dict(response_obj["writeReceipt"])],
                )

        intent = RepoWriteIntent(
            operation_id=operation_id,
            object_name=object_name,
            generation=manifest.generation,
            expected_generation=manifest.parent_generation,
            digest=object_sha256,
            replication_factor=replication_factor,
            required_acks=manifest.required_write_acks,
            consistency=manifest.write_consistency,
            selected_replicas=tuple(selected),
        )
        receipts: list[RepoWriteReceipt] = []
        failures: dict[str, str] = {}
        for repo_node in selected:
            try:
                response = self._request_specific_repo(
                    repo_node=repo_node,
                    payload=encode_repo_request(
                        "COMMIT_PACKET_SET",
                        manifest=manifest.to_dict(),
                        writeIntent=intent.to_dict(),
                    ),
                    timeout_ms=max(self.timeout_ms, 60000),
                    isolated_runtime=False,
                )
                response_obj = json.loads(response.payload.decode())
                receipts.append(RepoWriteReceipt.from_dict(response_obj["writeReceipt"]))
            except Exception as exc:  # noqa: BLE001
                failures[repo_node] = str(exc)
        validated = validate_write_receipts(intent, receipts, failures=failures)
        confirmed = tuple(receipt.repo_node for receipt in validated)
        committed_manifest = replace(
            manifest,
            replica_nodes=confirmed,
            confirmed_replica_nodes=confirmed,
            lifecycle_state="COMMITTED",
        )
        return self._finalize_receipt_quorum(
            intent, committed_manifest, validated)

    def manifest(self, object_name: str) -> RepoObjectManifest:
        def selector(candidates: list[AckCandidate]) -> list[str]:
            for candidate in candidates:
                fields = self._parse_ack_payload(candidate.payload)
                if fields.get("hasManifest") == "1":
                    return [candidate.provider_name]
            return []

        response = self._control_call(lambda: self.user.request_service_select(
            self._service_for("MANIFEST"),
            encode_repo_request("MANIFEST", objectName=object_name),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="first-responding",
        ))
        if not response.status:
            raise RuntimeError(response.error)
        return RepoObjectManifest.from_dict(json.loads(response.payload.decode()))

    def inventory(self) -> dict[str, RepoObjectManifest]:
        def selector(candidates: list[AckCandidate]) -> list[str]:
            return [
                candidate.provider_name
                for candidate in candidates
                if candidate.status
            ][:1]

        response = self._control_call(lambda: self.user.request_service_select(
            self._service_for("INVENTORY"),
            encode_repo_request("INVENTORY"),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        ))
        if not response.status:
            raise RuntimeError(response.error)
        obj = json.loads(response.payload.decode())
        if not isinstance(obj, dict):
            raise ValueError("repo inventory response must be a JSON object")
        return {
            str(name): RepoObjectManifest.from_dict(value)
            for name, value in obj.items()
        }

    def catalog_lookup(self, object_name: str, repo_node: str) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("CATALOG_LOOKUP", objectName=object_name),
            timeout_ms=self.timeout_ms,
            isolated_runtime=True,
        )
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog lookup response must be a JSON object")
        return decoded

    def catalog_query(self, repo_node: str, query: dict) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("CATALOG_QUERY", query=dict(query)),
            timeout_ms=self.timeout_ms,
            isolated_runtime=True,
        )
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog query response must be a JSON object")
        return decoded

    def catalog_status(self, repo_node: str) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("CATALOG_STATUS"),
            timeout_ms=self.timeout_ms,
            isolated_runtime=True,
        )
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog status response must be a JSON object")
        return decoded

    def catalog_merge(
        self,
        repo_node: str,
        entries: Iterable[dict],
        source_status: Optional[dict] = None,
    ) -> dict:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request(
                "CATALOG_MERGE",
                entries=list(entries),
                sourceStatus=source_status or {},
            ),
            timeout_ms=self.timeout_ms,
            isolated_runtime=True,
        )
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog merge response must be a JSON object")
        return decoded

    def catalog_repair(self, target_repo_node: str, action: dict) -> dict:
        repair_action = RepoRepairAction.from_dict(
            dict(action), target_repo_node=target_repo_node)
        action = repair_action.to_dict()
        object_name = repair_action.object_name
        source_repo = repair_action.source_repo
        target_repo = repair_action.target_repo

        # The durable catalog action is created only for a target that is
        # missing this finalized generation. Probing that known miss through
        # FETCH_PREPARE turns a negative ACK into a fixed selection timeout and
        # serializes all repair workers behind the client owner thread. Replay
        # remains safe: STORE_PACKET_PULL verifies the exact packet manifest,
        # hashes, repair authorization, and replaces the same object digest.
        prepared_response = self._request_specific_repo(
            repo_node=source_repo,
            payload=encode_repo_request("FETCH_PREPARE", objectName=object_name),
            timeout_ms=min(max(self.timeout_ms, 5000), 10000),
            isolated_runtime=True,
        )
        prepared = json.loads(prepared_response.payload.decode())
        if not isinstance(prepared, dict):
            raise ValueError("repo repair source response must be a JSON object")
        source_manifest = RepoObjectManifest.from_dict(prepared["manifest"])
        if source_manifest.object_name != object_name:
            raise ValueError(
                f"catalog repair source object mismatch: {source_manifest.object_name}"
            )
        expected_hash = str(action.get("objectSha256", ""))
        if expected_hash and expected_hash != source_manifest.sha256:
            raise ValueError(
                f"catalog repair object hash mismatch: action={expected_hash} "
                f"source={source_manifest.sha256}"
            )

        data_name = str(prepared["dataName"])
        payload = fetch_segmented_object(
            data_name,
            timeout_ms=_pull_fetch_timeout_ms(source_manifest.segment_count),
            interest_lifetime_ms=_large_data_interest_lifetime_ms(),
            init_cwnd=8.0,
        )
        actual_hash = hashlib.sha256(payload).hexdigest()
        if actual_hash != source_manifest.sha256:
            raise ValueError(
                f"catalog repair fetched object hash mismatch: "
                f"manifest={source_manifest.sha256} actual={actual_hash}"
            )
        packets = fetch_segmented_data_packets(
            data_name,
            timeout_ms=_pull_fetch_timeout_ms(source_manifest.segment_count),
            interest_lifetime_ms=_large_data_interest_lifetime_ms(),
        )
        if len(packets) != source_manifest.segment_count:
            raise ValueError(
                f"catalog repair source segment count mismatch: "
                f"expected={source_manifest.segment_count} actual={len(packets)}"
            )

        repair_manifest = RepoObjectManifest(
            object_name=source_manifest.object_name,
            object_type=source_manifest.object_type,
            sha256=source_manifest.sha256,
            size=source_manifest.size,
            segment_count=source_manifest.segment_count,
            replication_factor=max(
                int(action.get("maxReplicationFactor",
                               source_manifest.replication_factor) or 1),
                int(action.get("minReplicationFactor", 1) or 1),
            ),
            min_replication_factor=int(action.get("minReplicationFactor", 1) or 1),
            max_replication_factor=int(action.get(
                "maxReplicationFactor",
                source_manifest.max_replication_factor or source_manifest.replication_factor,
            ) or source_manifest.replication_factor),
            replica_nodes=(target_repo,),
            replica_data_names=(data_name,),
            packet_names=tuple(packet.name for packet in packets),
            segment_locations=({
                "start": 0,
                "end": max(0, source_manifest.segment_count - 1),
                "dataName": data_name,
                "repoNode": target_repo,
                "hints": [target_repo],
                "routeStrategy": "hint-first",
                "repairSourceRepo": source_repo,
            },),
            policy_epoch=source_manifest.policy_epoch,
        )
        packet_manifest = json.dumps({
            "objectName": object_name,
            "dataName": data_name,
            "packets": [
                {
                    "name": packet.name,
                    "segment": packet.segment,
                    "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
                }
                for packet in packets
            ],
        }, sort_keys=True).encode()
        packet_manifest_name = self._packet_manifest_name(target_repo, object_name)
        manifest_producer = SegmentedObjectProducer(
            packet_manifest_name,
            packet_manifest,
            signing_identity=self.user.user,
            max_segment_size=6000,
            freshness_ms=60000,
        ).start()
        try:
            time.sleep(0.2)
            response = self._request_specific_repo(
                repo_node=target_repo,
                payload=encode_repo_request(
                    "STORE_PACKET_PULL",
                    manifest=repair_manifest.to_dict(),
                    sourceName=data_name,
                    packetManifestName=manifest_producer.versioned_name,
                    packetManifestSha256=hashlib.sha256(packet_manifest).hexdigest(),
                    repairAuthorization=action,
                ),
                timeout_ms=max(
                    self.timeout_ms,
                    _pull_fetch_timeout_ms(repair_manifest.segment_count) + 30000,
                ),
                isolated_runtime=True,
            )
        finally:
            try:
                manifest_producer.stop()
            except Exception:
                pass

        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog repair response must be a JSON object")
        catalog_entry = dict(decoded.get("catalogEntry", {}))
        if catalog_entry:
            catalog_entry["minReplicationFactor"] = int(
                action.get("minReplicationFactor", 1) or 1
            )
            catalog_entry["maxReplicationFactor"] = int(
                action.get(
                    "maxReplicationFactor",
                    repair_manifest.replication_factor,
                ) or repair_manifest.replication_factor
            )
            catalog_entry["desiredReplicationFactor"] = catalog_entry[
                "minReplicationFactor"
            ]
            catalog_entry["repairSourceRepo"] = source_repo
        return {
            "status": "repaired",
            "repoNode": target_repo,
            "objectName": object_name,
            "sourceRepo": source_repo,
            "targetRepo": target_repo,
            "segmentCount": repair_manifest.segment_count,
            "catalogEntry": catalog_entry,
            "storeResult": decoded,
        }

    def catalog_snapshot(self, repo_node: str) -> dict:
        decoded, _ = self.catalog_snapshot_with_payload(repo_node)
        return decoded

    def catalog_snapshot_with_payload(self, repo_node: str) -> tuple[dict, bytes]:
        response = self._request_specific_repo(
            repo_node=repo_node,
            payload=encode_repo_request("CATALOG_SNAPSHOT"),
            timeout_ms=self.timeout_ms,
            isolated_runtime=True,
        )
        decoded = json.loads(response.payload.decode())
        if not isinstance(decoded, dict):
            raise ValueError("repo catalog snapshot response must be a JSON object")
        return decoded, bytes(response.payload)

    def delete(
        self,
        object_name: str,
        replica_nodes: Iterable[str] = (),
    ) -> bool:
        selected_replicas = [str(repo) for repo in replica_nodes if str(repo)]
        if selected_replicas:
            removed = False
            payload = encode_repo_request("DELETE", objectName=object_name)
            for repo_node in selected_replicas:
                response = self._request_specific_repo(
                    repo_node=repo_node,
                    payload=payload,
                    timeout_ms=max(self.timeout_ms, 120000),
                    isolated_runtime=True,
                )
                try:
                    obj = json.loads(response.payload.decode())
                    removed = removed or str(obj.get("status", "")) == "deleted"
                except Exception:
                    removed = removed or response.payload.decode(
                        errors="replace") == "deleted"
            return removed

        def selector(candidates: list[AckCandidate]) -> list[str]:
            selected = []
            for candidate in candidates:
                fields = self._parse_ack_payload(candidate.payload)
                if fields.get("hasManifest") == "1" or fields.get("hasObject") == "1":
                    selected.append(candidate.provider_name)
            return selected

        response = self._control_call(lambda: self.user.request_service_select(
            self._service_for("DELETE"),
            encode_repo_request("DELETE", objectName=object_name),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        ))
        if not response.status:
            raise RuntimeError(response.error)
        try:
            obj = json.loads(response.payload.decode())
            return str(obj.get("status", "")) == "deleted"
        except Exception:
            return response.payload.decode(errors="replace") == "deleted"

    def fetch(self, object_name: str, manifest: RepoObjectManifest | None = None) -> bytes:
        manifest = manifest or self.manifest(object_name)
        return self.fetch_object(object_name, manifest)

    def fetch_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
        manifest = manifest or self.manifest(object_name)
        # A packet-backed manifest remains readable as an application payload
        # view. FETCH_PREPARE serves the original stored packets, so this path
        # reassembles content without renaming, re-signing, or persisting a
        # second representation. Use fetch_signed_packets() when exact wires are
        # required.
        if not manifest.replica_nodes:
            raise RuntimeError(f"manifest has no replicas: {object_name}")
        if manifest.segment_locations:
            by_data_name: dict[str, list[dict]] = {}
            for location in manifest.segment_locations:
                by_data_name.setdefault(str(location["dataName"]), []).append(location)
            last_error: Exception | None = None
            for data_name, locations in by_data_name.items():
                covered_segments = set()
                hint_ranges: list[SegmentHintRange] = []
                route_strategy = "hint-first"
                source_repos: set[str] = set()
                for location in locations:
                    start = int(location.get("start", 0))
                    end = int(location.get("end", start))
                    covered_segments.update(range(start, end + 1))
                    repo_node = str(location.get("repoNode", ""))
                    if repo_node:
                        source_repos.add(repo_node)
                    if str(location.get("routeStrategy", "")) == "direct-first":
                        route_strategy = "direct-first"
                    hint_ranges.append(SegmentHintRange(
                        start=start,
                        end=end,
                        forwarding_hints=tuple(
                            str(hint) for hint in location.get("hints", [])
                        ),
                    ))
                if len(covered_segments) < manifest.segment_count:
                    continue
                try:
                    for repo_node in sorted(source_repos):
                        self._prepare_fetch_source(
                            repo_node,
                            object_name,
                            expected_data_name=data_name,
                        )
                    versioned_names = {
                        str(location.get("versionedDataName", ""))
                        for location in locations
                        if location.get("versionedDataName")
                    }
                    if len(versioned_names) == 1:
                        versioned_name = next(iter(versioned_names))
                        if route_strategy == "direct-first":
                            first_hint_ranges: list[SegmentHintRange] = []
                            second_hint_ranges = hint_ranges
                        else:
                            first_hint_ranges = hint_ranges
                            second_hint_ranges = []
                        try:
                            payload = fetch_known_segmented_object_with_segment_hints(
                                versioned_name,
                                manifest.segment_count,
                                timeout_ms=max(self.timeout_ms, 30000),
                                interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                                hint_ranges=first_hint_ranges,
                            )
                        except Exception:
                            payload = fetch_known_segmented_object_with_segment_hints(
                                versioned_name,
                                manifest.segment_count,
                                timeout_ms=max(self.timeout_ms, 30000),
                                interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                                hint_ranges=second_hint_ranges,
                            )
                    else:
                        if route_strategy == "direct-first":
                            first_hint_ranges = []
                            second_hint_ranges = hint_ranges
                        else:
                            first_hint_ranges = hint_ranges
                            second_hint_ranges = []
                        try:
                            payload = fetch_segmented_object_with_segment_hints(
                                data_name,
                                timeout_ms=max(self.timeout_ms, 30000),
                                interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                                hint_ranges=first_hint_ranges,
                            )
                        except Exception:
                            payload = fetch_segmented_object_with_segment_hints(
                                data_name,
                                timeout_ms=max(self.timeout_ms, 30000),
                                interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                                hint_ranges=second_hint_ranges,
                            )
                    break
                except Exception as exc:  # noqa: BLE001
                    last_error = exc
            else:
                raise RuntimeError(
                    f"no repo segment location could serve {object_name}: {last_error}"
                )
            if len(payload) != manifest.size:
                raise RuntimeError(f"repo object size mismatch: {object_name}")
            if hashlib.sha256(payload).hexdigest() != manifest.sha256:
                raise RuntimeError(f"repo object hash mismatch: {object_name}")
            return payload
        data_names = manifest.replica_data_names or tuple(
            self.data_name(repo_node, object_name)
            for repo_node in manifest.replica_nodes
        )
        last_error: Exception | None = None
        payload = b""
        data_name_by_repo = dict(zip(manifest.replica_nodes, data_names))
        ordered_repos = self._ordered_replicas(manifest.replica_nodes)
        deadline = time.monotonic() + max(self.timeout_ms, 30_000) / 1000.0

        def fetch_replica(repo_node: str, attempt_ms: int) -> bytes:
            data_name = data_name_by_repo.get(repo_node, "")
            prepared = self._prepare_fetch_source(
                repo_node, object_name, expected_data_name=data_name,
                timeout_ms=attempt_ms)
            prepared_name = str(prepared["dataName"])
            raw_hints = prepared.get("forwardingHints", [])
            forwarding_hints = (
                [str(hint) for hint in raw_hints if str(hint)]
                if isinstance(raw_hints, list) else [])
            if not forwarding_hints and not prepared_name.startswith(
                    repo_node.rstrip("/") + "/"):
                forwarding_hints = [repo_node]
            candidate_payload = fetch_segmented_object(
                prepared_name,
                timeout_ms=attempt_ms,
                interest_lifetime_ms=_large_data_interest_lifetime_ms(),
                init_cwnd=8.0,
                forwarding_hints=forwarding_hints,
            )
            if len(candidate_payload) != manifest.size:
                raise RuntimeError(f"repo object size mismatch: {object_name}")
            if hashlib.sha256(candidate_payload).hexdigest() != manifest.sha256:
                raise RuntimeError(f"repo object hash mismatch: {object_name}")
            return candidate_payload

        hedge_delay_ms = getattr(self, "hedged_read_delay_ms", 0)
        if hedge_delay_ms > 0 and len(ordered_repos) > 1:
            executor = ThreadPoolExecutor(max_workers=2)
            try:
                started_by_future = {}
                first_repo = ordered_repos[0]
                first_started = time.monotonic()
                first = executor.submit(
                    fetch_replica, first_repo,
                    max(500, int((deadline - first_started) * 1000)))
                started_by_future[first] = (first_repo, first_started)
                done, _ = wait(
                    [first], timeout=hedge_delay_ms / 1000.0,
                    return_when=FIRST_COMPLETED)
                if not done:
                    second_repo = ordered_repos[1]
                    second_started = time.monotonic()
                    second = executor.submit(
                        fetch_replica, second_repo,
                        max(500, int((deadline - second_started) * 1000)))
                    started_by_future[second] = (second_repo, second_started)
                pending = set(started_by_future)
                while pending:
                    done, pending = wait(
                        pending, timeout=max(0.0, deadline - time.monotonic()),
                        return_when=FIRST_COMPLETED)
                    if not done:
                        break
                    for future in done:
                        repo_node, started = started_by_future[future]
                        try:
                            payload = future.result()
                            self._record_replica_result(
                                repo_node, success=True,
                                latency_ms=(time.monotonic() - started) * 1000.0)
                            for other in pending:
                                other.cancel()
                            return payload
                        except Exception as exc:  # noqa: BLE001
                            last_error = exc
                            self._record_replica_result(repo_node, success=False)
                raise RuntimeError(
                    f"no hedged repo replica could serve {object_name}: {last_error}")
            finally:
                # cancel_futures was added in Python 3.9; Ubuntu 20.04 uses
                # Python 3.8, so cancel pending work explicitly first.
                executor.shutdown(wait=False)

        for index, repo_node in enumerate(ordered_repos):
            started = time.monotonic()
            try:
                remaining_ms = max(1, int((deadline - started) * 1000))
                attempt_ms = max(
                    500, remaining_ms // max(1, len(ordered_repos) - index))
                payload = fetch_replica(repo_node, attempt_ms)
                self._record_replica_result(
                    repo_node, success=True,
                    latency_ms=(time.monotonic() - started) * 1000.0)
                break
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                self._record_replica_result(repo_node, success=False)
        else:
            raise RuntimeError(f"no repo replica could serve {object_name}: {last_error}")
        return payload

    def wait_until_ready(self, timeout_s: float = 10.0, *, probe_timeout_ms: int = 3000) -> dict:
        deadline = time.time() + timeout_s
        last_error = None
        while time.time() < deadline:
            try:
                return self.capability(timeout_ms=min(self.timeout_ms, probe_timeout_ms))
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                self._log(f"repo readiness probe failed: {exc}")
                time.sleep(0.2)
        raise RuntimeError(f"repo cluster not ready: {last_error}")


class DistributedRepo:
    """User-facing generic object-store facade.

    This wrapper hides NDNSF-specific setup details such as ``ServiceUser``,
    the shared repo service name, and the upload prefix. Applications can treat
    the repo as a named object store: ``put`` bytes, ``get`` bytes, and inspect
    returned manifests when placement metadata matters.
    """

    DEFAULT_SERVICE = "/NDNSF/DistributedRepo"
    DEFAULT_CONFIG_OBJECT = (
        "/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml"
    )

    def __init__(self, client: NetworkDistributedRepoClient):
        self._client = client
        self._known_manifests: dict[str, RepoObjectManifest] = {}

    @property
    def publisher_namespace(self) -> str:
        return self._client.publisher_namespace

    def object_name(self, suffix: str) -> str:
        return self._client.publisher_object_name(suffix)

    def _publisher_object_name(self, object_name: str) -> str:
        name = str(object_name).strip()
        if name.startswith("/"):
            return self._client._require_publisher_object_name(name)
        return self.object_name(name)

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-distributed-repo-policy",
        user: str | None = None,
        service_name: str = DEFAULT_SERVICE,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 10000,
        verbose: bool = False,
    ) -> "DistributedRepo":
        from ndnsf_distributed_inference.app import APPDeployment
        from ndnsf_distributed_inference.policy import load_config

        configure_repo_object_class_policies(load_config(config))

        deployment = APPDeployment.from_config(
            config,
            generated_policy_dir=generated_policy_dir,
        ).deployment
        user_name = user or deployment.user
        service_user = ServiceUser(
            group=deployment.group,
            controller=deployment.controller,
            user=user_name,
            trust_schema=deployment.trust_schema,
            permission_wait_ms=6000,
            adaptive_admission=False,
        )
        return cls(NetworkDistributedRepoClient(
            user=service_user,
            service_name=service_name,
            upload_prefix=f"{user_name}/NDNSF-DISTRIBUTED-REPO/UPLOAD",
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            verbose=verbose,
        ))

    @classmethod
    def from_ndn_config(
        cls,
        *,
        controller: str,
        user: str,
        group: str,
        trust_schema: str,
        config_object_name: str = DEFAULT_CONFIG_OBJECT,
        generated_policy_dir: str | Path = "/tmp/ndnsf-distributed-repo-policy",
        service_name: str = DEFAULT_SERVICE,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 10000,
        verbose: bool = False,
    ) -> "DistributedRepo":
        bootstrap_user = ServiceUser(
            group=group,
            controller=controller,
            user=user,
            trust_schema=trust_schema,
            permission_wait_ms=6000,
            adaptive_admission=False,
        )
        bootstrap_client = NetworkDistributedRepoClient(
            user=bootstrap_user,
            service_name=service_name,
            upload_prefix=f"{user}/NDNSF-DISTRIBUTED-REPO/UPLOAD",
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            verbose=verbose,
        )
        config_payload = bootstrap_client.fetch(config_object_name)
        policy_dir = Path(generated_policy_dir)
        policy_dir.mkdir(parents=True, exist_ok=True)
        config_path = policy_dir / "deployment-from-ndn.yaml"
        config_path.write_bytes(config_payload)
        return cls.from_config(
            config_path,
            generated_policy_dir=policy_dir,
            user=user,
            service_name=service_name,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            verbose=verbose,
        )

    from_repo_config = from_ndn_config

    def wait_until_ready(self, timeout_s: float = 10.0) -> dict:
        return self._client.wait_until_ready(timeout_s)

    def put(
        self,
        object_name: str,
        payload: bytes | bytearray | memoryview | str,
        *,
        object_type: str = "object",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        if isinstance(payload, str):
            payload_bytes = payload.encode()
        else:
            payload_bytes = bytes(payload)
        canonical_name = self._publisher_object_name(object_name)
        manifest = self._client.store_object(
            object_name=canonical_name,
            payload=payload_bytes,
            object_type=object_type,
            replication_factor=replication_factor,
            replica_nodes=tuple(replica_nodes),
            policy_epoch=policy_epoch,
            metadata=metadata,
        )
        self._known_manifests[manifest.object_name] = manifest
        return manifest

    def get(self, object_name: str, manifest: RepoObjectManifest | None = None) -> bytes:
        canonical_name = (
            manifest.object_name if manifest is not None
            else self._publisher_object_name(object_name)
        )
        return self._client.fetch_object(
            canonical_name,
            manifest or self._known_manifests.get(canonical_name),
        )

    def put_signed_packets(
        self,
        object_name: str,
        packets: list[DataPacket],
        *,
        object_type: str,
        object_size: int,
        object_sha256: str,
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        data_name: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
        canonical_name = self._publisher_object_name(object_name)
        manifest = self._client.store_signed_packets(
            object_name=canonical_name,
            packets=packets,
            object_type=object_type,
            object_size=object_size,
            object_sha256=object_sha256,
            replication_factor=replication_factor,
            replica_nodes=tuple(replica_nodes),
            policy_epoch=policy_epoch,
            data_name=data_name,
            metadata=metadata,
        )
        self._known_manifests[manifest.object_name] = manifest
        return manifest

    def fetch_packet(self, repo_node: str, data_name: str) -> DataPacket:
        return self._client.fetch_packet(repo_node, data_name)

    def get_signed_packets(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
        *,
        repo_node: str = "",
    ) -> list[DataPacket]:
        canonical_name = (
            manifest.object_name if manifest is not None
            else self._publisher_object_name(object_name)
        )
        resolved_manifest = (
            manifest or self._known_manifests.get(canonical_name) or
            self._client.manifest(canonical_name)
        )
        self._known_manifests[resolved_manifest.object_name] = resolved_manifest
        return self._client.fetch_signed_packets(
            resolved_manifest, repo_node=repo_node)

    def manifest(self, object_name: str) -> RepoObjectManifest:
        canonical_name = self._publisher_object_name(object_name)
        manifest = self._client.manifest(canonical_name)
        self._known_manifests[canonical_name] = manifest
        return manifest

    def list(self) -> dict[str, RepoObjectManifest]:
        return dict(self._known_manifests)

    def remote_inventory(self) -> dict[str, RepoObjectManifest]:
        inventory = self._client.inventory()
        self._known_manifests.update(inventory)
        return inventory

    def catalog_lookup(self, object_name: str, repo_node: str) -> dict:
        return self._client.catalog_lookup(object_name, repo_node)

    def catalog_query(self, repo_node: str, query: dict) -> dict:
        return self._client.catalog_query(repo_node, query)

    def catalog_status(self, repo_node: str) -> dict:
        return self._client.catalog_status(repo_node)

    def cache_status(self, repo_node: str) -> dict:
        return self._client.cache_status(repo_node)

    def catalog_merge(
        self,
        repo_node: str,
        entries: Iterable[dict],
        source_status: Optional[dict] = None,
    ) -> dict:
        return self._client.catalog_merge(repo_node, entries, source_status)

    def catalog_repair(self, target_repo_node: str, action: dict) -> dict:
        return self._client.catalog_repair(target_repo_node, action)

    def repair_scan(self, repo_node: str) -> dict:
        return self._client.repair_scan(repo_node)

    def repair_claim(
        self,
        repo_node: str,
        *,
        lease_owner: str,
        lease_ms: int = 60_000,
    ) -> dict:
        return self._client.repair_claim(
            repo_node, lease_owner=lease_owner, lease_ms=lease_ms)

    def repair_complete(
        self,
        repo_node: str,
        *,
        repair_id: str,
        result: dict,
    ) -> dict:
        return self._client.repair_complete(
            repo_node, repair_id=repair_id, result=result)

    def repair_fail(
        self,
        repo_node: str,
        *,
        repair_id: str,
        error: str,
    ) -> dict:
        return self._client.repair_fail(
            repo_node, repair_id=repair_id, error=error)

    def catalog_snapshot(self, repo_node: str) -> dict:
        return self._client.catalog_snapshot(repo_node)

    def catalog_snapshot_with_payload(self, repo_node: str) -> tuple[dict, bytes]:
        return self._client.catalog_snapshot_with_payload(repo_node)

    def remove(self, object_name: str) -> bool:
        canonical_name = self._publisher_object_name(object_name)
        manifest = self._known_manifests.get(canonical_name)
        removed = self._client.delete(
            canonical_name,
            manifest.replica_nodes if manifest is not None else (),
        )
        if removed:
            self._known_manifests.pop(canonical_name, None)
        return removed

    store = put
    fetch = get
    inventory = list
    delete = remove


def _score(capability: StorageCapability) -> tuple[float, str]:
    bandwidth_bonus = min(500.0, max(0.0, capability.network_bandwidth_mbps) * 0.1)
    score = (
        capability.free_bytes / (1024.0 * 1024.0) +
        1000.0 * capability.availability_score -
        1000.0 * capability.recent_load -
        20.0 * capability.queue_depth -
        10.0 * capability.inflight_operations -
        capability.storage_latency_ms -
        capability.network_rtt_ms +
        bandwidth_bonus
    )
    return score, capability.repo_node


def select_replicas(
    candidates: Iterable[StorageCapability],
    policy: PlacementPolicy,
    object_size: int,
) -> tuple[StorageCapability, ...]:
    eligible = [
        candidate for candidate in candidates
        if (candidate.repo_node and candidate.accepts_backup_replica and
            candidate.free_bytes >= object_size)
    ]
    eligible.sort(key=_score, reverse=True)

    selected: list[StorageCapability] = []
    failure_domains: set[str] = set()
    for candidate in eligible:
        if len(selected) >= policy.replication_factor:
            break
        if (policy.avoid_same_failure_domain and candidate.failure_domain and
                candidate.failure_domain in failure_domains):
            continue
        selected.append(candidate)
        if candidate.failure_domain:
            failure_domains.add(candidate.failure_domain)

    for candidate in eligible:
        if len(selected) >= policy.replication_factor:
            break
        if candidate not in selected:
            selected.append(candidate)

    return tuple(selected)
