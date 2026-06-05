"""NDNSF-DistributedRepo-facing helper API.

This module is the Python-facing companion to the experimental C++
``NDNSF-DistributedRepo`` subproject. It lets NDNSF-DI applications describe
artifact/intermediate storage intent without embedding repo placement logic in
model-specific splitters.
"""

from __future__ import annotations

import base64
from collections import OrderedDict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import threading
import time
from typing import Iterable, Optional

from ndnsf import (
    AckCandidate,
    AckDecision,
    DataPacket,
    SegmentedObjectProducer,
    ServiceProvider,
    ServiceResponse,
    ServiceUser,
    SegmentHintRange,
    StoredDataProducer,
    fetch_segmented_data_packets,
    fetch_segmented_object,
    fetch_segmented_object_with_segment_hints,
    fetch_known_segmented_object_with_segment_hints,
    make_segmented_data_packets,
)


def _pull_fetch_timeout_ms(segment_count: int) -> int:
    """Return a conservative timeout for repo-side pull of segmented objects."""
    if segment_count <= 0:
        return 60000
    return max(60000, min(600000, segment_count * 150))


@dataclass(frozen=True)
class RepoObjectManifest:
    object_name: str
    object_type: str
    sha256: str
    size: int
    segment_count: int = 1
    replication_factor: int = 1
    replica_nodes: tuple[str, ...] = ()
    replica_data_names: tuple[str, ...] = ()
    segment_locations: tuple[dict, ...] = ()
    policy_epoch: str = ""

    def to_dict(self) -> dict:
        return {
            "objectName": self.object_name,
            "objectType": self.object_type,
            "sha256": self.sha256,
            "size": self.size,
            "segmentCount": self.segment_count,
            "replicationFactor": self.replication_factor,
            "replicaNodes": list(self.replica_nodes),
            "replicaDataNames": list(self.replica_data_names),
            "segmentLocations": list(self.segment_locations),
            "policyEpoch": self.policy_epoch,
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
            replica_nodes=tuple(str(value) for value in obj.get("replicaNodes", [])),
            replica_data_names=tuple(str(value) for value in obj.get("replicaDataNames", [])),
            segment_locations=tuple(dict(value) for value in obj.get("segmentLocations", [])),
            policy_epoch=str(obj.get("policyEpoch", "")),
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
        "encrypted": bool(manifest_dict.get("encrypted", False)),
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
    if "repoManifest" in entry:
        return dict(entry["repoManifest"])
    if "repo_manifest" in entry:
        return dict(entry["repo_manifest"])
    return dict(entry)


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


class RepoNodeApp:
    """Real NDNSF repo-node application using one shared service name.

    Every repo node registers the same ``service_name``. Node identity and
    capability are carried in ACK metadata, while request payloads carry the
    operation. For a replicated INSERT/payload adapter request, the client includes selected
    ``replicaNodes`` in the manifest; nodes not in that set acknowledge the
    request but skip storing the payload.
    """

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
        catalog_sync_interval_s: float = 10.0,
        handler_threads: int = 4,
        ack_threads: int = 2,
        serve_certificates: bool = True,
    ) -> None:
        self.repo_node = repo_node
        self.service_name = service_name
        self.group = group
        self.controller = controller
        self.trust_schema = trust_schema
        self.provider_name = (
            f"{provider_prefix.rstrip('/')}/{provider_id.strip('/')}"
            if provider_id else provider_prefix.rstrip("/")
        )
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
        self._store = LocalDistributedRepo([self.capability])
        self.storage_dir = Path(storage_dir) if storage_dir else None
        if self.storage_dir is not None:
            self.storage_dir.mkdir(parents=True, exist_ok=True)
        self.capacity_bytes = free_bytes
        self.memory_cache_bytes = max(0, memory_cache_bytes)
        self._cache: OrderedDict[str, tuple[RepoObjectManifest, bytes]] = OrderedDict()
        self._packet_cache: OrderedDict[str, tuple[RepoObjectManifest, list[DataPacket]]] = OrderedDict()
        self._cache_bytes = 0
        self._db_lock = threading.RLock()
        self._db: Optional[sqlite3.Connection] = None
        if self.storage_dir is not None:
            self._db = sqlite3.connect(
                self.storage_dir / "repo.sqlite3",
                check_same_thread=False,
            )
            self._init_sqlite()
            if preallocate_bytes > 0:
                reserve = self.storage_dir / "repo.reserve"
                with reserve.open("ab") as file:
                    file.truncate(preallocate_bytes)
        self._object_producers: list[SegmentedObjectProducer | StoredDataProducer] = []
        self.advertise_stored_prefixes = advertise_stored_prefixes
        self.advertise_command = advertise_command
        self._advertised_prefixes: set[str] = set()
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
        self._catalog_stop = threading.Event()
        self._catalog_thread: Optional[threading.Thread] = None

    def _init_sqlite(self) -> None:
        assert self._db is not None
        with self._db_lock:
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.execute("PRAGMA synchronous=NORMAL")
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
            self._db.commit()

    def _cache_get(self, object_name: str) -> Optional[tuple[RepoObjectManifest, bytes]]:
        item = self._cache.get(object_name)
        if item is None:
            return None
        self._cache.move_to_end(object_name)
        return item

    def _cache_put(self, manifest: RepoObjectManifest, payload: bytes) -> None:
        if self.memory_cache_bytes <= 0 or len(payload) > self.memory_cache_bytes:
            return
        old = self._cache.pop(manifest.object_name, None)
        if old is not None:
            self._cache_bytes -= len(old[1])
        self._cache[manifest.object_name] = (manifest, payload)
        self._cache_bytes += len(payload)
        while self._cache_bytes > self.memory_cache_bytes and self._cache:
            _, (_, evicted) = self._cache.popitem(last=False)
            self._cache_bytes -= len(evicted)

    def _packet_cache_get(self, object_name: str) -> Optional[tuple[RepoObjectManifest, list[DataPacket]]]:
        item = self._packet_cache.get(object_name)
        if item is None:
            return None
        self._packet_cache.move_to_end(object_name)
        return item

    def _packet_cache_put(self, manifest: RepoObjectManifest, packets: list[DataPacket]) -> None:
        packet_bytes = sum(len(packet.wire) for packet in packets)
        if self.memory_cache_bytes <= 0 or packet_bytes > self.memory_cache_bytes:
            return
        old = self._packet_cache.pop(manifest.object_name, None)
        if old is not None:
            self._cache_bytes -= sum(len(packet.wire) for packet in old[1])
        self._packet_cache[manifest.object_name] = (manifest, list(packets))
        self._cache_bytes += packet_bytes
        while self._cache_bytes > self.memory_cache_bytes and self._packet_cache:
            _, (_, evicted) = self._packet_cache.popitem(last=False)
            self._cache_bytes -= sum(len(packet.wire) for packet in evicted)

    def _sqlite_used_bytes(self) -> int:
        if self._db is None:
            return sum(len(payload) for _, payload in self._store.objects.values())
        with self._db_lock:
            row = self._db.execute(
                """
                SELECT
                  (SELECT COALESCE(SUM(payload_size), 0) FROM objects) +
                  (SELECT COALESCE(SUM(wire_size), 0) FROM data_segments)
                """
            ).fetchone()
        return int(row[0] if row else 0)

    def _capability(self) -> StorageCapability:
        used = self._sqlite_used_bytes()
        return StorageCapability(
            repo_node=self.capability.repo_node,
            free_bytes=max(0, self.capacity_bytes - used),
            used_bytes=used,
            recent_load=self.capability.recent_load,
            availability_score=self.capability.availability_score,
            failure_domain=self.capability.failure_domain,
            storage_classes=self.capability.storage_classes,
            repo_mode=self.capability.repo_mode,
            accepts_backup_replica=self.capability.accepts_backup_replica,
        )

    def _load_manifest(self, object_name: str) -> RepoObjectManifest:
        if object_name in self._store.inventory():
            return self._store.manifest(object_name)
        if self._db is None:
            raise KeyError(object_name)
        with self._db_lock:
            row = self._db.execute(
                "SELECT manifest_json FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
        if row is None:
            raise KeyError(object_name)
        return RepoObjectManifest.from_dict(json.loads(str(row[0])))

    def _persist_object(self, manifest: RepoObjectManifest, payload: bytes) -> None:
        self._cache_put(manifest, payload)
        self._remember_catalog_change(manifest, "AVAILABLE")
        if self._db is None:
            return
        old_size = 0
        with self._db_lock:
            row = self._db.execute(
                "SELECT payload_size FROM objects WHERE object_name=?",
                (manifest.object_name,),
            ).fetchone()
            if row is not None:
                old_size = int(row[0])
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
            self._db.commit()

    def _persist_manifest(self, manifest: RepoObjectManifest) -> None:
        self._remember_catalog_change(manifest, "AVAILABLE")
        if self._db is None:
            return
        with self._db_lock:
            self._db.execute(
                """
                INSERT INTO objects
                  (object_name, manifest_json, payload, payload_size, sha256,
                   object_type, updated_at, hit_count)
                VALUES (?, ?, NULL, 0, ?, ?, ?, 0)
                ON CONFLICT(object_name) DO UPDATE SET
                  manifest_json=excluded.manifest_json,
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
            self._db.commit()

    def _persist_packets(self, manifest: RepoObjectManifest, packets: list[DataPacket]) -> None:
        self._packet_cache_put(manifest, packets)
        self._remember_catalog_change(manifest, "AVAILABLE")
        if self._db is None:
            return
        packet_bytes = sum(len(packet.wire) for packet in packets)
        old_size = 0
        with self._db_lock:
            row = self._db.execute(
                "SELECT COALESCE(SUM(wire_size), 0) FROM data_segments WHERE object_name=?",
                (manifest.object_name,),
            ).fetchone()
            if row is not None:
                old_size = int(row[0])
            if packet_bytes > self.capacity_bytes - self._sqlite_used_bytes() + old_size:
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
            self._db.executemany(
                """
                INSERT INTO data_segments
                  (object_name, segment_no, data_name, wire, wire_size,
                   updated_at, hit_count)
                VALUES (?, ?, ?, ?, ?, ?, 0)
                """,
                [
                    (
                        manifest.object_name,
                        packet.segment,
                        packet.name,
                        sqlite3.Binary(packet.wire),
                        len(packet.wire),
                        time.time(),
                    )
                    for packet in packets
                ],
            )
            self._db.commit()

    def _persist_packet(self, manifest: RepoObjectManifest, packet: DataPacket) -> None:
        self._packet_cache.pop(manifest.object_name, None)
        if self._db is None:
            return
        packet_bytes = len(packet.wire)
        old_size = 0
        with self._db_lock:
            row = self._db.execute(
                """
                SELECT wire_size FROM data_segments
                WHERE object_name=? AND segment_no=?
                """,
                (manifest.object_name, packet.segment),
            ).fetchone()
            if row is not None:
                old_size = int(row[0])
            if packet_bytes > self.capacity_bytes - self._sqlite_used_bytes() + old_size:
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
                """
                INSERT INTO data_segments
                  (object_name, segment_no, data_name, wire, wire_size,
                   updated_at, hit_count)
                VALUES (?, ?, ?, ?, ?, ?, 0)
                ON CONFLICT(object_name, segment_no) DO UPDATE SET
                  data_name=excluded.data_name,
                  wire=excluded.wire,
                  wire_size=excluded.wire_size,
                  updated_at=excluded.updated_at
                """,
                (
                    manifest.object_name,
                    packet.segment,
                    packet.name,
                    sqlite3.Binary(packet.wire),
                    packet_bytes,
                    time.time(),
                ),
            )
            self._db.commit()

    def _load_persisted_packets(self, object_name: str) -> tuple[RepoObjectManifest, list[DataPacket]]:
        cached = self._packet_cache_get(object_name)
        if cached is not None:
            return cached
        if self._db is None:
            raise KeyError(object_name)
        with self._db_lock:
            manifest_row = self._db.execute(
                "SELECT manifest_json FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
            rows = self._db.execute(
                """
                SELECT segment_no, data_name, wire
                FROM data_segments
                WHERE object_name=?
                ORDER BY segment_no ASC
                """,
                (object_name,),
            ).fetchall()
            if manifest_row is None or not rows:
                raise KeyError(object_name)
            self._db.execute(
                "UPDATE objects SET hit_count=hit_count+1 WHERE object_name=?",
                (object_name,),
            )
            self._db.execute(
                "UPDATE data_segments SET hit_count=hit_count+1 WHERE object_name=?",
                (object_name,),
            )
            self._db.commit()
        manifest = RepoObjectManifest.from_dict(json.loads(str(manifest_row[0])))
        packets = [
            DataPacket(name=str(data_name), segment=int(segment_no), wire=bytes(wire))
            for segment_no, data_name, wire in rows
        ]
        self._packet_cache_put(manifest, packets)
        return manifest, packets

    def _load_persisted_object(self, object_name: str) -> tuple[RepoObjectManifest, bytes]:
        cached = self._cache_get(object_name)
        if cached is not None:
            return cached
        if self._db is None:
            raise KeyError(object_name)
        with self._db_lock:
            row = self._db.execute(
                """
                SELECT manifest_json, payload
                FROM objects
                WHERE object_name=? AND payload IS NOT NULL
                """,
                (object_name,),
            ).fetchone()
            if row is None:
                raise KeyError(object_name)
            self._db.execute(
                "UPDATE objects SET hit_count=hit_count+1 WHERE object_name=?",
                (object_name,),
            )
            self._db.commit()
        manifest = RepoObjectManifest.from_dict(json.loads(str(row[0])))
        payload = bytes(row[1])
        if hashlib.sha256(payload).hexdigest() != manifest.sha256:
            raise ValueError(f"persisted repo object hash mismatch: {object_name}")
        self._cache_put(manifest, payload)
        return manifest, payload

    def _sqlite_has_manifest(self, object_name: str) -> bool:
        if self._db is None:
            return False
        with self._db_lock:
            row = self._db.execute(
                "SELECT 1 FROM objects WHERE object_name=?",
                (object_name,),
            ).fetchone()
        return row is not None

    def _sqlite_has_object(self, object_name: str) -> bool:
        if self._db is None:
            return False
        with self._db_lock:
            row = self._db.execute(
                """
                SELECT 1 FROM objects
                WHERE object_name=? AND
                  (payload IS NOT NULL OR EXISTS (
                    SELECT 1 FROM data_segments WHERE object_name=objects.object_name
                  ))
                """,
                (object_name,),
            ).fetchone()
        return row is not None

    def _sqlite_inventory(self) -> dict[str, RepoObjectManifest]:
        if self._db is None:
            return {}
        with self._db_lock:
            rows = self._db.execute(
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
    def _catalog_manifest_summary(manifest: RepoObjectManifest) -> dict:
        # Catalog entries keep object/repo control-plane semantics here. Payload
        # transport sizing is handled by NDNSF core large-response references.
        return {
            "objectName": manifest.object_name,
            "objectType": manifest.object_type,
            "sha256": manifest.sha256,
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "replicationFactor": manifest.replication_factor,
            "replicaNodes": list(manifest.replica_nodes),
            "policyEpoch": manifest.policy_epoch,
        }

    def _catalog_entry(self, manifest: RepoObjectManifest, state: str) -> dict:
        now_ms = self._now_ms()
        return {
            "objectName": manifest.object_name,
            "objectSha256": manifest.sha256,
            "manifestSha256": self._manifest_sha256(manifest),
            "objectType": manifest.object_type,
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "sourceRepo": self.repo_node,
            "repoMode": self.capability.repo_mode,
            "state": state,
            "catalogEpoch": self._catalog_epoch,
            "lastSeenMs": now_ms,
            "updatedAtMs": now_ms,
            "desiredReplicationFactor": manifest.replication_factor,
            "replicaNodes": list(manifest.replica_nodes),
            "manifest": self._catalog_manifest_summary(manifest),
        }

    def _catalog_status_entry(self) -> dict:
        capability = self._capability()
        return {
            "repoNode": self.repo_node,
            "repoMode": capability.repo_mode,
            "catalogEpoch": self._catalog_epoch,
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

    def _upsert_catalog_entry(self, entry: dict) -> None:
        object_name = str(entry.get("objectName", ""))
        source_repo = str(entry.get("sourceRepo", ""))
        if not object_name or not source_repo:
            return
        normalized = dict(entry)
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
            current_epoch = int(current.get("catalogEpoch", 0))
            entry_epoch = int(normalized.get("catalogEpoch", 0))
            if current_epoch > entry_epoch:
                return
            if (current_epoch == entry_epoch and
                    int(current.get("updatedAtMs", 0)) >
                    int(normalized.get("updatedAtMs", 0))):
                return
        by_source[source_repo] = normalized
        self._merge_repo_status({
            "repoNode": source_repo,
            "repoMode": normalized.get("repoMode", ""),
            "catalogEpoch": normalized.get("catalogEpoch", 0),
        })

    def _flatten_catalog_entries(self) -> list[dict]:
        entries: list[dict] = []
        for by_source in self._global_catalog.values():
            entries.extend(dict(entry) for entry in by_source.values())
        return entries

    def _is_entry_stale(self, entry: dict) -> bool:
        source_repo = str(entry.get("sourceRepo", ""))
        status = self._repo_status.get(source_repo, {})
        last_seen = int(status.get("lastSeenMs", entry.get("lastSeenMs", 0)) or 0)
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
        if not entries:
            raise KeyError(object_name)
        for entry in entries:
            entry["stale"] = self._is_entry_stale(entry)
        available = [
            entry for entry in entries
            if str(entry.get("state", "")) == "AVAILABLE" and not entry.get("stale")
        ]
        tombstones = [
            entry for entry in entries
            if str(entry.get("state", "")) == "DELETED"
        ]
        desired = max(
            [int(entry.get("desiredReplicationFactor",
                       entry.get("manifest", {}).get("replicationFactor", 1)) or 1)
             for entry in entries] or [1]
        )
        source_repos = {str(entry.get("sourceRepo", "")) for entry in entries}
        stale_repos = [
            str(entry.get("sourceRepo", ""))
            for entry in entries
            if entry.get("stale")
        ]
        target_candidates = [
            repo for repo, status in repo_status.items()
            if repo not in source_repos and
            str(status.get("repoMode", "")) == "persistent" and
            bool(status.get("acceptsBackupReplica", True))
        ]
        missing = max(0, desired - len(available))
        repair_plan = {
            "needed": missing > 0,
            "missingReplicas": missing,
            "sourceRepos": [str(entry.get("sourceRepo", "")) for entry in available],
            "targetCandidates": target_candidates[:missing],
        }
        best = (available or entries)[0]
        return {
            "objectName": object_name,
            "objectSha256": best.get("objectSha256", ""),
            "manifestSha256": best.get("manifestSha256", ""),
            "objectType": best.get("objectType", ""),
            "size": best.get("size", 0),
            "segmentCount": best.get("segmentCount", 0),
            "state": ("DELETED" if tombstones and not available else
                      "UNDER_REPLICATED" if missing else "AVAILABLE"),
            "desiredReplicationFactor": desired,
            "availableReplicaCount": len(available),
            "underReplicated": missing > 0,
            "staleRepos": stale_repos,
            "entries": entries,
            "candidateReplicas": available,
            "repairPlan": repair_plan,
        }

    def _remember_catalog_change(self, manifest: RepoObjectManifest, state: str) -> None:
        with self._catalog_lock:
            self._catalog_epoch += 1
            entry = self._catalog_entry(manifest, state)
            entry["catalogEpoch"] = self._catalog_epoch
            self._catalog_changes.append(entry)
            self._upsert_catalog_entry(entry)

    def _catalog_snapshot(self) -> dict:
        inventory = self._sqlite_inventory()
        inventory.update(self._store.inventory())
        with self._catalog_lock:
            for manifest in inventory.values():
                self._upsert_catalog_entry(self._catalog_entry(manifest, "AVAILABLE"))
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

    def _merge_catalog_entries(self, entries: Iterable[dict],
                               source_status: Optional[dict] = None) -> None:
        with self._catalog_lock:
            if source_status:
                self._merge_repo_status(source_status)
            for raw_entry in entries:
                self._upsert_catalog_entry(dict(raw_entry))

    def _catalog_lookup(self, object_name: str) -> dict:
        try:
            manifest = self._load_manifest(object_name)
            self._upsert_catalog_entry(self._catalog_entry(manifest, "AVAILABLE"))
        except KeyError:
            pass
        return self._object_catalog_summary(object_name)

    def _sqlite_payload_bytes(self, object_name: str) -> bytes:
        _, payload = self._load_persisted_object(object_name)
        return payload

    def _delete_object(self, object_name: str) -> bool:
        deleted_manifest: RepoObjectManifest | None = None
        try:
            deleted_manifest = self._load_manifest(object_name)
        except KeyError:
            pass
        removed = self._store.erase(object_name)
        self._cache.pop(object_name, None)
        self._packet_cache.pop(object_name, None)
        if self._db is not None:
            with self._db_lock:
                cursor = self._db.execute(
                    "DELETE FROM objects WHERE object_name=?",
                    (object_name,),
                )
                self._db.execute(
                    "DELETE FROM data_segments WHERE object_name=?",
                    (object_name,),
                )
                self._db.commit()
                removed = removed or cursor.rowcount > 0
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
            interest_lifetime_ms=1000,
            init_cwnd=8.0,
        )

    def _serve_object(self, name: str, payload: bytes) -> SegmentedObjectProducer:
        producer = SegmentedObjectProducer(
            name,
            payload,
            signing_identity=self.provider_name,
            max_segment_size=6000,
            freshness_ms=60000,
        ).start()
        self._advertise_prefix(name)
        self._object_producers.append(producer)
        return producer

    def _serve_packets(self, name: str, packets: list[DataPacket]) -> StoredDataProducer:
        producer = StoredDataProducer(
            name,
            [packet.wire for packet in packets],
            signing_identity=self.provider_name,
        ).start()
        time.sleep(0.2)
        self._advertise_prefix(name)
        self._object_producers.append(producer)
        return producer

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

    @staticmethod
    def _decode_packet_object(packet_obj: dict, operation: str) -> DataPacket:
        packet = DataPacket(
            name=str(packet_obj["name"]),
            segment=int(packet_obj["segment"]),
            wire=base64.b64decode(str(packet_obj["wireB64"])),
        )
        expected_name = str(packet_obj.get("segmentName", packet.name))
        if packet.name != expected_name:
            raise ValueError(
                f"{operation} segment name mismatch: "
                f"metadata={expected_name} packet={packet.name}"
            )
        expected_hash = str(packet_obj.get("wireSha256", ""))
        if expected_hash:
            actual_hash = hashlib.sha256(packet.wire).hexdigest()
            if actual_hash != expected_hash:
                raise ValueError(
                    f"{operation} wire hash mismatch for {packet.name}: "
                    f"metadata={expected_hash} actual={actual_hash}"
                )
        return packet

    def _has_manifest(self, object_name: str) -> bool:
        if object_name in self._store.inventory():
            return True
        return self._sqlite_has_manifest(object_name)

    def _has_object(self, object_name: str) -> bool:
        if object_name in self._store.objects:
            return True
        return self._sqlite_has_object(object_name)

    def _ack(self, payload: bytes) -> AckDecision:
        has_manifest = False
        has_object = False
        try:
            request = decode_repo_request(payload)
            operation = str(request["operation"]).upper()
            object_name = str(request.get("objectName", ""))
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
        except Exception:
            return AckDecision(False, "repo-bad-request")
        capability = self._capability()
        ack_payload = (
            f"repoNode={capability.repo_node};"
            f"freeBytes={capability.free_bytes};"
            f"usedBytes={capability.used_bytes};"
            f"load={capability.recent_load};"
            f"availability={capability.availability_score};"
            f"failureDomain={capability.failure_domain};"
            f"repoMode={capability.repo_mode};"
            f"acceptsBackupReplica={1 if capability.accepts_backup_replica else 0};"
            f"memoryCacheBytes={self.memory_cache_bytes};"
            f"memoryCacheUsedBytes={self._cache_bytes};"
            f"storageBackend={'sqlite' if self._db is not None else 'memory'};"
            f"hasManifest={1 if has_manifest else 0};"
            f"hasObject={1 if has_object else 0};"
        ).encode()
        return AckDecision(status=True, message="repo-ready", payload=ack_payload)

    def _handle(self, payload: bytes) -> ServiceResponse:
        try:
            request = decode_repo_request(payload)
            operation = str(request["operation"]).upper()
            if operation == "CAPABILITY":
                capability = self._capability()
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
                    "storageBackend": "sqlite" if self._db is not None else "memory",
                    "capacityBytes": self.capacity_bytes,
                    "memoryCacheBytes": self.memory_cache_bytes,
                    "memoryCacheUsedBytes": self._cache_bytes,
                }, sort_keys=True).encode())
            if operation == "STORE":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())
                if "payloadB64" in request:
                    object_payload = base64.b64decode(str(request["payloadB64"]))
                else:
                    object_payload = bytes.fromhex(str(request["payloadHex"]))
                self._store.put(
                    object_name=manifest.object_name,
                    payload=object_payload,
                    object_type=manifest.object_type,
                    policy=PlacementPolicy(replication_factor=1),
                    policy_epoch=manifest.policy_epoch,
                )
                self._persist_object(manifest, object_payload)
                self._serve_object(
                    self.data_name(self.repo_node, manifest.object_name),
                    object_payload,
                )
                return ServiceResponse(True, json.dumps({
                    "status": "stored",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                }, sort_keys=True).encode())
            if operation == "INSERT":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())
                object_payload = self._catch_chunks(str(request["sourceName"]))
                if len(object_payload) != manifest.size:
                    raise ValueError(f"repo object size mismatch: {manifest.object_name}")
                if hashlib.sha256(object_payload).hexdigest() != manifest.sha256:
                    raise ValueError(f"repo object hash mismatch: {manifest.object_name}")
                self._store.put(
                    object_name=manifest.object_name,
                    payload=object_payload,
                    object_type=manifest.object_type,
                    policy=PlacementPolicy(replication_factor=1),
                    policy_epoch=manifest.policy_epoch,
                )
                self._persist_object(manifest, object_payload)
                self._serve_object(
                    self.data_name(self.repo_node, manifest.object_name),
                    object_payload,
                )
                return ServiceResponse(True, json.dumps({
                    "status": "inserted",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                }, sort_keys=True).encode())
            if operation == "STORE_PACKETS":
                manifest = RepoObjectManifest.from_dict(request["manifest"])
                replica_nodes = set(manifest.replica_nodes)
                if replica_nodes and self.repo_node not in replica_nodes:
                    return ServiceResponse(True, json.dumps({
                        "status": "skipped",
                        "repoNode": self.repo_node,
                        "objectName": manifest.object_name,
                    }, sort_keys=True).encode())
                packets = [
                    self._decode_packet_object(packet, operation)
                    for packet in request.get("packets", [])
                ]
                if not packets:
                    raise ValueError(f"STORE_PACKETS has no packets: {manifest.object_name}")
                stored_manifest = RepoObjectManifest(
                    object_name=manifest.object_name,
                    object_type=manifest.object_type,
                    sha256=manifest.sha256,
                    size=manifest.size,
                    segment_count=len(packets),
                    replication_factor=manifest.replication_factor,
                    replica_nodes=manifest.replica_nodes,
                    replica_data_names=manifest.replica_data_names,
                    policy_epoch=manifest.policy_epoch,
                )
                self._store.put_manifest(stored_manifest)
                self._persist_packets(stored_manifest, packets)
                serve_name = (
                    stored_manifest.replica_data_names[0]
                    if stored_manifest.replica_data_names
                    else self.data_name(self.repo_node, stored_manifest.object_name)
                )
                self._serve_packets(
                    serve_name,
                    packets,
                )
                return ServiceResponse(True, json.dumps({
                    "status": "stored-packets",
                    "repoNode": self.repo_node,
                    "manifest": stored_manifest.to_dict(),
                    "dataName": serve_name,
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
                self._store.put_manifest(manifest)
                for packet in packets:
                    self._persist_packet(manifest, packet)
                try:
                    _, stored_packets = self._load_persisted_packets(manifest.object_name)
                    if len(stored_packets) >= manifest.segment_count:
                        serve_name = (
                            manifest.replica_data_names[0]
                            if manifest.replica_data_names
                            else self.data_name(self.repo_node, manifest.object_name)
                        )
                        self._serve_packets(
                            serve_name,
                            stored_packets,
                        )
                        self._remember_catalog_change(manifest, "AVAILABLE")
                except KeyError:
                    pass
                return ServiceResponse(True, json.dumps({
                    "status": "stored-packet-batch" if operation == "STORE_PACKET_BATCH" else "stored-packet",
                    "repoNode": self.repo_node,
                    "objectName": manifest.object_name,
                    "segments": [packet.segment for packet in packets],
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

                packet_manifest_name = str(request["packetManifestName"])
                packet_manifest_bytes = fetch_segmented_object(
                    packet_manifest_name,
                    timeout_ms=_pull_fetch_timeout_ms(max(1, manifest.segment_count // 32)),
                    interest_lifetime_ms=2000,
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
                source_name = str(request["sourceName"])
                packets = fetch_segmented_data_packets(
                    source_name,
                    timeout_ms=_pull_fetch_timeout_ms(manifest.segment_count),
                    interest_lifetime_ms=2000,
                )
                if len(packets) != manifest.segment_count:
                    raise ValueError(
                        f"STORE_PACKET_PULL segment count mismatch for {manifest.object_name}: "
                        f"expected={manifest.segment_count} actual={len(packets)}"
                    )
                for packet in packets:
                    expected_hash = expected_packets.get(packet.name)
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
                self._store.put_manifest(manifest)
                self._persist_packets(manifest, packets)
                self._serve_packets(source_name, packets)
                return ServiceResponse(True, json.dumps({
                    "status": "stored-packet-pull",
                    "repoNode": self.repo_node,
                    "objectName": manifest.object_name,
                    "segmentCount": len(packets),
                    "dataName": source_name,
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
                self._store.put_manifest(manifest)
                self._persist_manifest(manifest)
                return ServiceResponse(True, json.dumps({
                    "status": "manifest-stored",
                    "repoNode": self.repo_node,
                    "manifest": manifest.to_dict(),
                }, sort_keys=True).encode())
            if operation == "FETCH":
                object_name = str(request["objectName"])
                try:
                    fetched = self._store.fetch(object_name)
                except KeyError:
                    fetched = self._sqlite_payload_bytes(object_name)
                return ServiceResponse(True, json.dumps({
                    "payloadB64": base64.b64encode(fetched).decode(),
                }, sort_keys=True).encode())
            if operation == "FETCH_PREPARE":
                object_name = str(request["objectName"])
                try:
                    manifest = self._store.manifest(object_name)
                    fetched = self._store.fetch(object_name)
                except KeyError:
                    try:
                        manifest, packets = self._load_persisted_packets(object_name)
                        data_name = self.data_name(self.repo_node, object_name)
                        self._serve_packets(data_name, packets)
                        return ServiceResponse(True, json.dumps({
                            "dataName": data_name,
                            "forwardingHints": [self.provider_name],
                            "manifest": manifest.to_dict(),
                        }, sort_keys=True).encode())
                    except KeyError:
                        manifest, fetched = self._load_persisted_object(object_name)
                data_name = self.data_name(self.repo_node, object_name)
                packets = make_segmented_data_packets(
                    data_name,
                    fetched,
                    signing_identity=self.provider_name,
                    max_segment_size=6000,
                    freshness_ms=60000,
                )
                self._persist_packets(manifest, packets)
                self._serve_packets(data_name, packets)
                return ServiceResponse(True, json.dumps({
                    "dataName": data_name,
                    "forwardingHints": [self.provider_name],
                    "manifest": manifest.to_dict(),
                }, sort_keys=True).encode())
            if operation == "MANIFEST":
                object_name = str(request["objectName"])
                manifest = self._load_manifest(object_name)
                return ServiceResponse(True, manifest.to_bytes())
            if operation == "INVENTORY":
                inventory = self._sqlite_inventory()
                inventory.update(self._store.inventory())
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
            if operation == "CATALOG_LOOKUP":
                object_name = str(request["objectName"])
                return ServiceResponse(
                    True,
                    json.dumps(self._catalog_lookup(object_name), sort_keys=True).encode(),
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
            self.service_name,
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
                    self._peer_catalog_epochs[peer] = int(
                        delta.get("catalogEpoch", since_epoch))
                except Exception as exc:  # noqa: BLE001
                    print(
                        f"repo catalog sync warning self={self.repo_node} "
                        f"peer={peer}: {exc}",
                        flush=True,
                    )
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
        self.provider.add_handler(self.service_name, self._handle)
        self.provider.set_ack_handler(self.service_name, self._ack)
        self._start_catalog_sync()
        return self.provider.run(self.service_name)

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
        manifest = self._store.put(
            object_name=object_name,
            payload=payload_bytes,
            object_type=object_type,
            policy=PlacementPolicy(replication_factor=1),
            policy_epoch=policy_epoch,
        )
        self._persist_object(manifest, payload_bytes)
        self._serve_object(
            self.data_name(self.repo_node, manifest.object_name),
            payload_bytes,
        )
        return manifest


class NetworkDistributedRepoClient:
    """NDNSF client for a shared-service-name DistributedRepo cluster."""

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
        max_store_batch_wire_bytes: int = 5000,
        pull_store_threshold_bytes: int = 65536,
    ) -> None:
        self.user = user
        self.service_name = service_name
        self.upload_prefix = upload_prefix.rstrip("/")
        self.ack_timeout_ms = ack_timeout_ms
        self.timeout_ms = timeout_ms
        self.max_segment_payload = max(512, max_segment_payload)
        self._placement_cache: list[str] = []
        self.verbose = verbose
        self.max_store_batch_wire_bytes = max(4096, max_store_batch_wire_bytes)
        self.pull_store_threshold_bytes = max(0, pull_store_threshold_bytes)

    def _log(self, message: str) -> None:
        if self.verbose:
            print(message, flush=True)

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
        response = self.user.request_service(
            self.service_name,
            encode_repo_request("CAPABILITY"),
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms if timeout_ms is not None else self.timeout_ms,
            strategy="first-responding",
        )
        if not response.status:
            raise RuntimeError(response.error)
        return json.loads(response.payload.decode())

    @staticmethod
    def _parse_ack_payload(payload: bytes) -> dict[str, str]:
        fields: dict[str, str] = {}
        for item in payload.decode(errors="replace").split(";"):
            if not item or "=" not in item:
                continue
            key, value = item.split("=", 1)
            fields[key] = value
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
            interest_lifetime_ms=1000,
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
        if len(self._placement_cache) >= replication_factor:
            cached = self._placement_cache[:replication_factor]
            self._log(f"repo select cache object={object_name} selected={cached}")
            return cached

        selected_repo_nodes: list[str] = []
        select_user = ServiceUser(
            group=self.user.group,
            controller=self.user.controller,
            user=self.user.user,
            trust_schema=self.user.trust_schema,
            permission_wait_ms=6000,
            adaptive_admission=False,
        )

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

        response = select_user.request_service_select(
            self.service_name,
            encode_repo_request("CAPABILITY", objectName=object_name),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        if selected_repo_nodes:
            self._placement_cache = list(selected_repo_nodes)
        return selected_repo_nodes[:replication_factor]

    def _request_specific_repo(
        self,
        *,
        repo_node: str,
        payload: bytes,
        timeout_ms: int | None = None,
        isolated_runtime: bool = False,
    ) -> ServiceResponse:
        def selector(candidates: list[AckCandidate]) -> list[str]:
            for candidate in candidates:
                fields = self._parse_ack_payload(candidate.payload)
                if fields.get("repoNode") == repo_node:
                    return [candidate.provider_name]
            return []

        request_user = self.user
        if isolated_runtime:
            request_user = ServiceUser(
                group=self.user.group,
                controller=self.user.controller,
                user=self.user.user,
                trust_schema=self.user.trust_schema,
                permission_wait_ms=6000,
                adaptive_admission=False,
            )

        response = request_user.request_service_select(
            self.service_name,
            payload,
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms or self.timeout_ms,
            request_strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        if self.verbose and response.payload:
            self._log(
                f"repo specific response repo={repo_node} bytes={len(response.payload)}"
            )
        return response

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
    ) -> RepoObjectManifest:
        selected_repo_nodes: list[str] = []

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

        packets: list[DataPacket] = []
        if operation == "STORE_PACKETS":
            packets = make_segmented_data_packets(
                packet_data_name or self.data_name("", object_name),
                payload,
                signing_identity=self.user.user,
                max_segment_size=6000,
                freshness_ms=60000,
            )

        manifest = manifest_override or RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=hashlib.sha256(payload).hexdigest(),
            size=len(payload),
                segment_count=len(packets) if packets else 1,
                replication_factor=replication_factor,
                replica_nodes=tuple(replica_nodes),
                replica_data_names=(
                    (packet_data_name,) if packet_data_name else ()
                ),
                policy_epoch=policy_epoch,
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
        response = self.user.request_service_select(
            self.service_name,
            encode_repo_request(
                operation,
                manifest=manifest.to_dict(),
                **({
                    "payloadB64": base64.b64encode(payload).decode()
                } if operation == "STORE" else {}),
                **packet_fields,
            ),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        )
        if not response.status:
            raise RuntimeError(response.error)
        if selected_repo_nodes:
            manifest = RepoObjectManifest(
                object_name=manifest.object_name,
                object_type=manifest.object_type,
                sha256=manifest.sha256,
                size=manifest.size,
                segment_count=manifest.segment_count,
                replication_factor=manifest.replication_factor,
                replica_nodes=tuple(selected_repo_nodes),
                replica_data_names=manifest.replica_data_names,
                policy_epoch=manifest.policy_epoch,
            )
        return manifest

    def store(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
    ) -> RepoObjectManifest:
        if len(payload) <= self.max_segment_payload:
            return self._store_once(
                object_name=object_name,
                payload=payload,
                object_type=object_type,
                replication_factor=replication_factor,
                replica_nodes=replica_nodes,
                policy_epoch=policy_epoch,
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
    ) -> RepoObjectManifest:
        object_name = self._require_publisher_object_name(object_name)

        def select_replicas_once() -> list[str]:
            if len(self._placement_cache) >= replication_factor:
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
            select_user = ServiceUser(
                group=self.user.group,
                controller=self.user.controller,
                user=self.user.user,
                trust_schema=self.user.trust_schema,
                permission_wait_ms=6000,
                adaptive_admission=False,
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
            response = select_user.request_service_select(
                self.service_name,
                encode_repo_request("CAPABILITY", objectName=object_name),
                selector,
                ack_timeout_ms=self.ack_timeout_ms,
                timeout_ms=self.timeout_ms,
                request_strategy="all-selected",
            )
            if not response.status:
                raise RuntimeError(response.error)
            self._log(
                f"repo select done object={object_name} selected={selected_repo_nodes}",
            )
            if selected_repo_nodes:
                self._placement_cache = list(selected_repo_nodes)
            return selected_repo_nodes

        last_error = ""
        for attempt in range(3):
            try:
                selected = list(replica_nodes) if replica_nodes else select_replicas_once()
                if len(selected) < replication_factor:
                    raise RuntimeError(
                        f"repo store selected {len(selected)} replicas, "
                        f"need {replication_factor}")
                selected = selected[:replication_factor]
                use_pull_store = len(payload) >= self.pull_store_threshold_bytes
                data_names = tuple(
                    self._upload_data_name(repo_node, object_name)
                    if use_pull_store else
                    self.data_name(repo_node, object_name)
                    for repo_node in selected
                )
                segment_locations: list[dict] = []
                packet_user = ServiceUser(
                    group=self.user.group,
                    controller=self.user.controller,
                    user=self.user.user,
                    trust_schema=self.user.trust_schema,
                    permission_wait_ms=6000,
                    adaptive_admission=False,
                )
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
                        response = packet_user.request_service(
                            self.service_name,
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
                        )
                        if not response.status:
                            raise RuntimeError(response.error)
                        continue

                    for packet in packets:
                        self._log(
                            f"repo store packet target={repo_node} "
                            f"segment={packet.segment} name={packet.name}",
                        )
                        response = packet_user.request_service(
                            self.service_name,
                            encode_repo_request(
                                "STORE_PACKET",
                                manifest=target_manifest.to_dict(),
                                packet=self._packet_to_request(packet),
                            ),
                            ack_timeout_ms=self.ack_timeout_ms,
                            timeout_ms=self.timeout_ms,
                            strategy="first-responding",
                        )
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
                return RepoObjectManifest(
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
        for packet in packets:
            if not str(packet.name).startswith(self.publisher_namespace + "/"):
                raise ValueError(
                    "signed Data packet names must be under the publisher namespace: "
                    f"{self.publisher_namespace}/..."
                )
        data_name = data_name or self._packet_data_name(packets)
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
        manifest = RepoObjectManifest(
            object_name=object_name,
            object_type=object_type,
            sha256=object_sha256,
            size=object_size,
            segment_count=len(packets),
            replication_factor=replication_factor,
            replica_nodes=tuple(selected),
            replica_data_names=tuple(data_name for _ in selected),
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
                segment_locations=manifest.segment_locations,
                policy_epoch=policy_epoch,
            )
            for batch in self._packet_batches(packets):
                self._request_specific_repo(
                    repo_node=repo_node,
                    payload=encode_repo_request(
                        "STORE_PACKET_BATCH",
                        manifest=target_manifest.to_dict(),
                        packets=[self._packet_to_request(packet) for packet in batch],
                    ),
                    timeout_ms=max(self.timeout_ms, 60000),
                    isolated_runtime=True,
                )
        # Give repo-side local prefix registrations for the stored packet name
        # a short moment to settle before an immediate fetch follows.
        time.sleep(0.2)
        return manifest

    def manifest(self, object_name: str) -> RepoObjectManifest:
        def selector(candidates: list[AckCandidate]) -> list[str]:
            for candidate in candidates:
                fields = self._parse_ack_payload(candidate.payload)
                if fields.get("hasManifest") == "1":
                    return [candidate.provider_name]
            return []

        response = self.user.request_service_select(
            self.service_name,
            encode_repo_request("MANIFEST", objectName=object_name),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="first-responding",
        )
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

        response = self.user.request_service_select(
            self.service_name,
            encode_repo_request("INVENTORY"),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        )
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

        response = self.user.request_service_select(
            self.service_name,
            encode_repo_request("DELETE", objectName=object_name),
            selector,
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=self.timeout_ms,
            request_strategy="all-selected",
        )
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
                for location in locations:
                    start = int(location.get("start", 0))
                    end = int(location.get("end", start))
                    covered_segments.update(range(start, end + 1))
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
                                interest_lifetime_ms=1000,
                                hint_ranges=first_hint_ranges,
                            )
                        except Exception:
                            payload = fetch_known_segmented_object_with_segment_hints(
                                versioned_name,
                                manifest.segment_count,
                                timeout_ms=max(self.timeout_ms, 30000),
                                interest_lifetime_ms=1000,
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
                                interest_lifetime_ms=1000,
                                hint_ranges=first_hint_ranges,
                            )
                        except Exception:
                            payload = fetch_segmented_object_with_segment_hints(
                                data_name,
                                timeout_ms=max(self.timeout_ms, 30000),
                                interest_lifetime_ms=1000,
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
        for repo_node, data_name in zip(manifest.replica_nodes, data_names):
            try:
                forwarding_hints = [] if data_name.startswith(
                    repo_node.rstrip("/") + "/"
                ) else [repo_node]
                payload = fetch_segmented_object(
                    data_name,
                    timeout_ms=max(self.timeout_ms, 30000),
                    interest_lifetime_ms=1000,
                    init_cwnd=8.0,
                    forwarding_hints=forwarding_hints,
                )
                break
            except Exception as exc:  # noqa: BLE001
                last_error = exc
        else:
            raise RuntimeError(f"no repo replica could serve {object_name}: {last_error}")
        if len(payload) != manifest.size:
            raise RuntimeError(f"repo object size mismatch: {object_name}")
        if hashlib.sha256(payload).hexdigest() != manifest.sha256:
            raise RuntimeError(f"repo object hash mismatch: {object_name}")
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
        from .app import APPDeployment

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
    score = (
        capability.free_bytes / (1024.0 * 1024.0) +
        1000.0 * capability.availability_score -
        1000.0 * capability.recent_load
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
