"""Authoritative metadata and streaming artifact payload persistence."""

from __future__ import annotations

from dataclasses import dataclass
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import threading
import time
from typing import Any, Iterable, Protocol


REPO_PERSISTENCE_OWNED = "repo-persistence-owned"
REPO_PERSISTENCE_CLOSED = "repo-persistence-closed"
REPO_LIFECYCLE_INVALID_STATE = "repo-lifecycle-invalid-state"
REPO_LIFECYCLE_STATE_CONFLICT = "repo-lifecycle-state-conflict"
REPO_LIFECYCLE_IDENTITY_CONFLICT = "repo-lifecycle-identity-conflict"
REPO_LIFECYCLE_EVENT_CONFLICT = "repo-lifecycle-event-conflict"
REPO_LIFECYCLE_ILLEGAL_TRANSITION = "repo-lifecycle-illegal-transition"
REPO_ARTIFACT_INVALID_IDENTITY = "repo-artifact-invalid-identity"
REPO_ARTIFACT_RANGE_OUT_OF_BOUNDS = "repo-artifact-range-out-of-bounds"
REPO_ARTIFACT_INCOMPLETE = "repo-artifact-incomplete"
REPO_ARTIFACT_DIGEST_MISMATCH = "repo-artifact-digest-mismatch"
REPO_ARTIFACT_WRITES_DISABLED = "repo-artifact-writes-disabled"

ARTIFACT_LIFECYCLE_STATES = frozenset({
    "ABSENT",
    "QUEUED",
    "RESERVED",
    "RECEIVING",
    "VERIFIED",
    "COMMITTED",
    "ACTIVE",
    "FAILED",
    "EXPIRED",
})

ARTIFACT_LIFECYCLE_TRANSITIONS = {
    "ABSENT": frozenset({"QUEUED", "RESERVED"}),
    "QUEUED": frozenset({"RECEIVING", "FAILED", "EXPIRED"}),
    "RESERVED": frozenset({"RECEIVING", "FAILED", "EXPIRED"}),
    "RECEIVING": frozenset({"VERIFIED", "FAILED", "EXPIRED"}),
    "VERIFIED": frozenset({"COMMITTED", "FAILED", "EXPIRED"}),
    "COMMITTED": frozenset({"ACTIVE"}),
    "ACTIVE": frozenset(),
    "FAILED": frozenset(),
    "EXPIRED": frozenset(),
}


class PersistenceOwnershipError(RuntimeError):
    """Raised when a second authority tries to own one backend."""


class LifecycleTransitionError(ValueError):
    """Raised after a rejected lifecycle event is durably journaled."""


@dataclass(frozen=True)
class RepoLifecycleEvent:
    event_id: str
    operation_id: str
    artifact_digest: str
    generation: int
    from_state: str
    to_state: str
    event_time_ms: int
    accepted: bool
    detail: dict
    error: str = ""
    sequence: int = 0


@dataclass(frozen=True)
class ArtifactTransferSessionRecord:
    operation_id: str
    artifact_digest: str
    generation: int
    identity: dict
    lease: dict
    state: str
    preserves_progress: bool
    verified_chunks: int
    newly_verified_bytes: int
    avoided_retransmission_bytes: int
    updated_at_ms: int


@dataclass(frozen=True)
class ArtifactFinalizationRecord:
    operation_id: str
    artifact_digest: str
    generation: int
    phase: str
    detail: dict
    updated_at_ms: int
    error: str = ""


@dataclass(frozen=True)
class ArtifactCapacityStatus:
    capacity_bytes: int
    committed_bytes: int
    reserved_bytes: int
    available_bytes: int


class PayloadStore(Protocol):
    """Persistence role owning artifact payload bytes."""

    @property
    def backend_kind(self) -> str:
        ...


@dataclass(frozen=True)
class ArtifactStorageIdentity:
    """Exact byte identity used by a generation-scoped payload session."""

    content_digest: str
    size_bytes: int
    generation: int
    digest_algorithm: str = "sha256"
    format_version: str = "artifact-manifest-v2"

    def __post_init__(self) -> None:
        digest = str(self.content_digest).strip().lower()
        if (self.format_version != "artifact-manifest-v2"
                or self.digest_algorithm != "sha256" or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)
                or int(self.size_bytes) < 0 or int(self.generation) <= 0):
            raise ValueError(
                f"{REPO_ARTIFACT_INVALID_IDENTITY}: artifact-manifest-v2, "
                "sha256, size, and positive generation are required"
            )
        object.__setattr__(self, "content_digest", digest)
        object.__setattr__(self, "size_bytes", int(self.size_bytes))
        object.__setattr__(self, "generation", int(self.generation))


class FilesystemCasPayloadStore:
    """Bounded-memory artifact-v2 CAS with compact verified-range sidecars."""

    _RANGE_MAGIC = b"RNG2"
    _MAX_RANGES = 1 << 20
    _HASH_BUFFER_BYTES = 256 * 1024

    def __init__(
        self, root_path: str | Path, max_range_bytes: int = 16 * 1024 * 1024
    ) -> None:
        self.root_path = Path(root_path).resolve()
        self.max_range_bytes = int(max_range_bytes)
        if self.max_range_bytes <= 0:
            raise ValueError(
                "repo-artifact-range-limit-invalid: "
                "max_range_bytes must be positive"
            )
        self._lock = threading.RLock()
        (self.root_path / "staging").mkdir(parents=True, exist_ok=True)
        (self.root_path / "payloads" / "sha256").mkdir(
            parents=True, exist_ok=True
        )

    @property
    def backend_kind(self) -> str:
        return "artifact-manifest-v2/filesystem-cas"

    def committed_path(self, identity: ArtifactStorageIdentity) -> Path:
        return (
            self.root_path
            / "payloads"
            / identity.digest_algorithm
            / identity.content_digest[:2]
            / identity.content_digest
        )

    def staging_path(self, identity: ArtifactStorageIdentity) -> Path:
        return (
            self.root_path
            / "staging"
            / f"{identity.content_digest}.{identity.generation}.part"
        )

    def _range_path(self, identity: ArtifactStorageIdentity) -> Path:
        return Path(str(self.staging_path(identity)) + ".ranges")

    def _intent_path(self, identity: ArtifactStorageIdentity) -> Path:
        return Path(str(self.staging_path(identity)) + ".finalize-intent")

    @staticmethod
    def _fsync_directory(path: Path) -> None:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _check_range(
        self, identity: ArtifactStorageIdentity, offset: int, length: int
    ) -> tuple[int, int]:
        offset, length = int(offset), int(length)
        if (offset < 0 or length < 0 or offset > identity.size_bytes
                or length > identity.size_bytes - offset):
            raise ValueError(
                f"{REPO_ARTIFACT_RANGE_OUT_OF_BOUNDS}: "
                f"offset={offset} length={length} size={identity.size_bytes}"
            )
        if length > self.max_range_bytes:
            raise ValueError(
                "repo-artifact-range-too-large: range exceeds bounded I/O limit"
            )
        return offset, length

    @classmethod
    def _decode_ranges(
        cls, path: Path, artifact_size: int
    ) -> list[tuple[int, int]]:
        try:
            encoded = path.read_bytes()
        except FileNotFoundError:
            return []
        maximum = 12 + cls._MAX_RANGES * 16
        if len(encoded) < 12 or len(encoded) > maximum:
            raise RuntimeError("repo-artifact-range-map-invalid: size")
        magic, count = struct.unpack_from(">4sQ", encoded)
        if (magic != cls._RANGE_MAGIC or count > cls._MAX_RANGES
                or len(encoded) != 12 + count * 16):
            raise RuntimeError("repo-artifact-range-map-invalid: header")
        ranges: list[tuple[int, int]] = []
        prior_end = -1
        for index in range(count):
            offset, length = struct.unpack_from(">QQ", encoded, 12 + index * 16)
            if (length == 0 or offset > artifact_size
                    or length > artifact_size - offset
                    or offset <= prior_end):
                raise RuntimeError("repo-artifact-range-map-invalid: ranges")
            ranges.append((offset, length))
            prior_end = offset + length
        return ranges

    @classmethod
    def _encode_ranges(cls, ranges: list[tuple[int, int]]) -> bytes:
        return (
            struct.pack(">4sQ", cls._RANGE_MAGIC, len(ranges))
            + b"".join(struct.pack(">QQ", *item) for item in ranges)
        )

    @classmethod
    def _merge_range(
        cls, ranges: list[tuple[int, int]], added: tuple[int, int]
    ) -> list[tuple[int, int]]:
        if added[1] == 0:
            return ranges
        merged: list[tuple[int, int]] = []
        for offset, length in sorted([*ranges, added]):
            end = offset + length
            if merged and offset <= merged[-1][0] + merged[-1][1]:
                previous_offset, previous_length = merged[-1]
                merged[-1] = (
                    previous_offset,
                    max(previous_offset + previous_length, end) - previous_offset,
                )
            else:
                merged.append((offset, length))
        if len(merged) > cls._MAX_RANGES:
            raise RuntimeError("repo-artifact-range-map-invalid: too many ranges")
        return merged

    @classmethod
    def _atomic_write(cls, path: Path, payload: bytes) -> None:
        temporary = Path(str(path) + ".tmp")
        descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600
        )
        try:
            view = memoryview(payload)
            while view:
                written = os.write(descriptor, view)
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        os.replace(temporary, path)
        cls._fsync_directory(path.parent)

    @classmethod
    def _sha256_file(cls, path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb", buffering=0) as source:
            while True:
                block = source.read(cls._HASH_BUFFER_BYTES)
                if not block:
                    break
                digest.update(block)
        return digest.hexdigest()

    def begin(self, identity: ArtifactStorageIdentity) -> None:
        with self._lock:
            if self.committed_path(identity).is_file():
                return
            path = self.staging_path(identity)
            descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
            try:
                current_size = os.fstat(descriptor).st_size
                if current_size not in (0, identity.size_bytes):
                    raise RuntimeError(
                        "repo-artifact-staging-identity-conflict: size changed"
                    )
                os.ftruncate(descriptor, identity.size_bytes)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            self._fsync_directory(path.parent)

    def write_range(
        self, identity: ArtifactStorageIdentity, offset: int, payload: bytes
    ) -> None:
        payload = bytes(payload)
        offset, _ = self._check_range(identity, offset, len(payload))
        with self._lock:
            descriptor = os.open(self.staging_path(identity), os.O_WRONLY)
            try:
                view = memoryview(payload)
                cursor = offset
                while view:
                    written = os.pwrite(descriptor, view, cursor)
                    view = view[written:]
                    cursor += written
            finally:
                os.close(descriptor)

    def read_range(
        self, identity: ArtifactStorageIdentity, offset: int, length: int
    ) -> bytes:
        offset, length = self._check_range(identity, offset, length)
        with self._lock:
            committed = self.committed_path(identity)
            path = committed if committed.is_file() else self.staging_path(identity)
            descriptor = os.open(path, os.O_RDONLY)
            try:
                result = bytearray()
                cursor = offset
                while len(result) < length:
                    block = os.pread(descriptor, length - len(result), cursor)
                    if not block:
                        raise RuntimeError(
                            "repo-artifact-truncated: payload shorter than declared"
                        )
                    result.extend(block)
                    cursor += len(block)
                return bytes(result)
            finally:
                os.close(descriptor)

    def mark_verified(
        self, identity: ArtifactStorageIdentity, offset: int, length: int
    ) -> None:
        added = self._check_range(identity, offset, length)
        with self._lock:
            path = self._range_path(identity)
            ranges = self._decode_ranges(path, identity.size_bytes)
            self._atomic_write(path, self._encode_ranges(
                self._merge_range(ranges, added)
            ))

    def verified_ranges(
        self, identity: ArtifactStorageIdentity
    ) -> tuple[tuple[int, int], ...]:
        with self._lock:
            if self.committed_path(identity).is_file():
                return () if identity.size_bytes == 0 else ((0, identity.size_bytes),)
            return tuple(self._decode_ranges(
                self._range_path(identity), identity.size_bytes
            ))

    def flush(self, identity: ArtifactStorageIdentity) -> None:
        with self._lock:
            descriptor = os.open(self.staging_path(identity), os.O_RDONLY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)

    def finalize(self, identity: ArtifactStorageIdentity) -> Path:
        with self._lock:
            committed = self.committed_path(identity)
            if committed.is_file():
                if self._sha256_file(committed) != identity.content_digest:
                    raise RuntimeError(
                        "repo-artifact-committed-corrupt: digest mismatch"
                    )
                if committed.stat().st_size != identity.size_bytes:
                    raise RuntimeError(
                        "repo-artifact-committed-corrupt: size mismatch"
                    )
                self.staging_path(identity).unlink(missing_ok=True)
                self._range_path(identity).unlink(missing_ok=True)
                self._intent_path(identity).unlink(missing_ok=True)
                self._fsync_directory(self.staging_path(identity).parent)
                return committed
            staging = self.staging_path(identity)
            ranges = self._decode_ranges(
                self._range_path(identity), identity.size_bytes
            )
            complete = (
                not ranges if identity.size_bytes == 0
                else ranges == [(0, identity.size_bytes)]
            )
            if not complete:
                raise RuntimeError(
                    f"{REPO_ARTIFACT_INCOMPLETE}: verified coverage is incomplete"
                )
            self.flush(identity)
            if self._sha256_file(staging) != identity.content_digest:
                raise RuntimeError(
                    f"{REPO_ARTIFACT_DIGEST_MISMATCH}: full digest mismatch"
                )
            self._atomic_write(
                self._intent_path(identity),
                json.dumps({
                    "contentDigest": identity.content_digest,
                    "generation": identity.generation,
                    "sizeBytes": identity.size_bytes,
                }, sort_keys=True, separators=(",", ":")).encode("utf-8"),
            )
            committed.parent.mkdir(parents=True, exist_ok=True)
            self._fsync_directory(committed.parent.parent)
            os.replace(staging, committed)
            self._fsync_directory(committed.parent)
            self._range_path(identity).unlink(missing_ok=True)
            self._intent_path(identity).unlink(missing_ok=True)
            self._fsync_directory(staging.parent)
            return committed

    def is_committed(self, identity: ArtifactStorageIdentity) -> bool:
        with self._lock:
            return self.committed_path(identity).is_file()

    def verify_committed(self, identity: ArtifactStorageIdentity) -> bool:
        with self._lock:
            path = self.committed_path(identity)
            return (
                path.is_file()
                and path.stat().st_size == identity.size_bytes
                and self._sha256_file(path) == identity.content_digest
            )

    def abort(self, identity: ArtifactStorageIdentity) -> None:
        with self._lock:
            self.staging_path(identity).unlink(missing_ok=True)
            self._range_path(identity).unlink(missing_ok=True)
            self._intent_path(identity).unlink(missing_ok=True)
            self._fsync_directory(self.staging_path(identity).parent)

    def reclaim_unreferenced_finalized(
        self, identity: ArtifactStorageIdentity
    ) -> None:
        """Remove finalized bytes only after metadata authority proves orphaning."""

        with self._lock:
            path = self.committed_path(identity)
            path.unlink(missing_ok=True)
            if path.parent.is_dir():
                self._fsync_directory(path.parent)


class MetadataStore(Protocol):
    """Persistence role owning lifecycle and catalog metadata."""

    def transition(
        self,
        *,
        event_id: str,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        from_state: str,
        to_state: str,
        detail: dict | None = None,
        event_time_ms: int | None = None,
    ) -> RepoLifecycleEvent:
        ...

    def lifecycle_events(self, operation_id: str) -> tuple[RepoLifecycleEvent, ...]:
        ...


class _OwnedSqliteConnection:
    """Connection view whose transaction control always returns to its facade."""

    def __init__(self, authority: "SqliteRepositoryPersistence") -> None:
        self._authority = authority

    def commit(self) -> None:
        self._authority.commit()

    def rollback(self) -> None:
        self._authority.rollback()

    def close(self) -> None:
        self._authority.close()

    def __getattr__(self, name):
        return getattr(self._authority._require_connection(), name)


class _LegacySqlitePayloadStore:
    def __init__(self, authority: "SqliteRepositoryPersistence") -> None:
        self._authority = authority

    @property
    def backend_kind(self) -> str:
        return "exact-packet-v1/sqlite"

    @property
    def connection(self) -> _OwnedSqliteConnection:
        return self._authority.connection


class _SqliteMetadataStore:
    def __init__(self, authority: "SqliteRepositoryPersistence") -> None:
        self._authority = authority

    def transition(self, **kwargs) -> RepoLifecycleEvent:
        return self._authority.transition(**kwargs)

    def lifecycle_events(self, operation_id: str) -> tuple[RepoLifecycleEvent, ...]:
        return self._authority.lifecycle_events(operation_id)


class SqliteRepositoryPersistence:
    """One authoritative facade for one deployed repository database."""

    SCHEMA_GENERATION = 12

    def __init__(
        self,
        database_path: str | Path,
        owner_id: str,
        *,
        capacity_bytes: int | None = None,
        reservation_overhead_bytes: int = 64 * 1024,
        reconcile_on_startup: bool = True,
        artifact_writes_enabled: bool = True,
        max_write_schema_generation: int | None = None,
    ) -> None:
        self.database_path = str(Path(database_path).resolve())
        self.owner_id = str(owner_id).strip()
        if not self.owner_id or len(self.owner_id.encode("utf-8")) > 256:
            raise ValueError("repo-persistence-invalid-owner: owner_id is required")
        self.lock_path = self.database_path + ".authority.lock"
        self.lock = threading.RLock()
        self.capacity_bytes = (
            None if capacity_bytes is None else int(capacity_bytes)
        )
        self.reservation_overhead_bytes = int(reservation_overhead_bytes)
        if (
            self.capacity_bytes is not None and self.capacity_bytes <= 0
        ) or self.reservation_overhead_bytes < 0:
            raise ValueError("repo-capacity-invalid-configuration")
        self._closed = False
        self._lock_file = None
        self._connection: sqlite3.Connection | None = None
        self._artifact_writes_requested = bool(artifact_writes_enabled)
        self._max_write_schema_generation = (
            self.SCHEMA_GENERATION
            if max_write_schema_generation is None
            else int(max_write_schema_generation)
        )
        if self._max_write_schema_generation < 0:
            raise ValueError("repo-schema-invalid-write-generation")
        self._artifact_writes_enabled = False
        self._catalog_has_format_identity = False
        self._migration_diagnostics: dict[str, Any] = {}
        self.connection = _OwnedSqliteConnection(self)
        self.payload_store: PayloadStore = _LegacySqlitePayloadStore(self)
        self.artifact_payload_store = FilesystemCasPayloadStore(
            Path(self.database_path).parent / "artifact-v2"
        )
        self.metadata_store: MetadataStore = _SqliteMetadataStore(self)

        Path(self.database_path).parent.mkdir(parents=True, exist_ok=True)
        try:
            self._acquire_ownership()
            self._connection = sqlite3.connect(
                self.database_path,
                check_same_thread=False,
            )
            self._initialize_lifecycle_schema()
            if reconcile_on_startup and self._artifact_writes_enabled:
                self.reconcile_finalizations()
                self.reconcile_gc_claims()
        except BaseException:
            self.close()
            raise

    def _acquire_ownership(self) -> None:
        lock_file = open(self.lock_path, "a+b", buffering=0)
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            lock_file.close()
            raise PersistenceOwnershipError(
                f"{REPO_PERSISTENCE_OWNED}: {self.database_path}"
            ) from exc
        self._lock_file = lock_file
        lock_file.seek(0)
        lock_file.truncate(0)
        lock_file.write(self.owner_id.encode("utf-8"))
        lock_file.flush()
        os.fsync(lock_file.fileno())

    def _require_connection(self) -> sqlite3.Connection:
        if self._closed or self._connection is None:
            raise RuntimeError(f"{REPO_PERSISTENCE_CLOSED}: {self.database_path}")
        return self._connection

    @property
    def artifact_writes_enabled(self) -> bool:
        return self._artifact_writes_enabled

    def migration_diagnostics(self) -> dict[str, Any]:
        return dict(self._migration_diagnostics)

    def _require_artifact_writes_enabled(self) -> None:
        if not self._artifact_writes_enabled:
            reason = self._migration_diagnostics.get(
                "reason", "operator-disabled"
            )
            raise LifecycleTransitionError(
                f"{REPO_ARTIFACT_WRITES_DISABLED}: {reason}"
            )

    @staticmethod
    def _table_exists(connection: sqlite3.Connection, table: str) -> bool:
        return connection.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            (table,),
        ).fetchone() is not None

    @staticmethod
    def _table_columns(
        connection: sqlite3.Connection, table: str
    ) -> set[str]:
        if not SqliteRepositoryPersistence._table_exists(connection, table):
            return set()
        return {
            str(row[1])
            for row in connection.execute(f"PRAGMA table_info({table})")
        }

    def _stored_schema_generation(
        self, connection: sqlite3.Connection
    ) -> int:
        if self._table_exists(connection, "artifact_schema_state"):
            row = connection.execute(
                "SELECT schema_generation FROM artifact_schema_state "
                "WHERE singleton=1"
            ).fetchone()
            if row is not None:
                return int(row[0])
        if self._table_exists(connection, "artifact_lifecycle_journal"):
            return 11
        return 0

    def _backfill_format_identity(
        self, connection: sqlite3.Connection
    ) -> None:
        receipt_rows = connection.execute(
            "SELECT receipt_id, receipt_json FROM artifact_replica_receipts"
        ).fetchall()
        for receipt_id, receipt_json in receipt_rows:
            receipt = json.loads(str(receipt_json))
            artifact = receipt.get("artifact", {})
            connection.execute(
                "UPDATE artifact_replica_receipts "
                "SET format_version=?, digest_algorithm=? WHERE receipt_id=?",
                (
                    str(artifact.get(
                        "formatVersion", "artifact-manifest-v2"
                    )),
                    str(artifact.get("digestAlgorithm", "sha256")),
                    str(receipt_id),
                ),
            )
        rows = connection.execute(
            "SELECT logical_name, policy_epoch, artifact_json "
            "FROM artifact_active_catalog"
        ).fetchall()
        for logical_name, policy_epoch, artifact_json in rows:
            artifact = json.loads(str(artifact_json))
            connection.execute(
                "UPDATE artifact_active_catalog "
                "SET format_version=?, digest_algorithm=? "
                "WHERE logical_name=? AND policy_epoch=?",
                (
                    str(artifact.get("formatVersion", "artifact-manifest-v2")),
                    str(artifact.get("digestAlgorithm", "sha256")),
                    str(logical_name),
                    str(policy_epoch),
                ),
            )
        connection.execute(
            "UPDATE artifact_gc_claims "
            "SET format_version='artifact-manifest-v2' "
            "WHERE format_version=''"
        )
        connection.execute(
            "UPDATE artifact_gc_claims SET digest_algorithm='sha256' "
            "WHERE digest_algorithm=''"
        )

    def _initialize_lifecycle_schema(self) -> None:
        connection = self._require_connection()
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=NORMAL")
        stored_generation = self._stored_schema_generation(connection)
        self._catalog_has_format_identity = {
            "format_version", "digest_algorithm"
        }.issubset(
            self._table_columns(connection, "artifact_active_catalog")
        )
        write_generation_supported = (
            stored_generation <= self._max_write_schema_generation
            and stored_generation <= self.SCHEMA_GENERATION
        )
        if not self._artifact_writes_requested or not write_generation_supported:
            reason = (
                "operator-disabled"
                if not self._artifact_writes_requested
                else "database-schema-newer-than-write-runtime"
            )
            self._migration_diagnostics = {
                "runtimeSchemaGeneration": self.SCHEMA_GENERATION,
                "databaseSchemaGeneration": stored_generation,
                "maxWriteSchemaGeneration": self._max_write_schema_generation,
                "previousSchemaGeneration": stored_generation,
                "action": "read-only-rollback",
                "writesEnabled": False,
                "reason": reason,
                "destructiveChanges": False,
            }
            return
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_lifecycle_journal (
                sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id TEXT NOT NULL UNIQUE,
                operation_id TEXT NOT NULL,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                from_state TEXT NOT NULL,
                to_state TEXT NOT NULL,
                event_time_ms INTEGER NOT NULL,
                accepted INTEGER NOT NULL,
                detail_json TEXT NOT NULL,
                error TEXT NOT NULL DEFAULT ''
            )
        """)
        connection.execute("""
            CREATE INDEX IF NOT EXISTS idx_artifact_lifecycle_operation
            ON artifact_lifecycle_journal(operation_id, sequence)
        """)
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_replica_receipts (
                receipt_id TEXT PRIMARY KEY,
                operation_id TEXT NOT NULL UNIQUE,
                format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2',
                digest_algorithm TEXT NOT NULL DEFAULT 'sha256',
                artifact_digest TEXT NOT NULL,
                repo_node TEXT NOT NULL,
                generation INTEGER NOT NULL,
                receipt_json TEXT NOT NULL,
                signer_key_id TEXT NOT NULL,
                authentication_algorithm TEXT NOT NULL,
                signature_hex TEXT NOT NULL,
                committed_at_ms INTEGER NOT NULL
            )
        """)
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_active_catalog (
                logical_name TEXT NOT NULL,
                policy_epoch TEXT NOT NULL,
                format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2',
                digest_algorithm TEXT NOT NULL DEFAULT 'sha256',
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                operation_id TEXT NOT NULL,
                artifact_json TEXT NOT NULL,
                activated_at_ms INTEGER NOT NULL,
                PRIMARY KEY (logical_name, policy_epoch)
            )
        """)
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_transfer_sessions (
                operation_id TEXT PRIMARY KEY,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                identity_json TEXT NOT NULL,
                lease_json TEXT NOT NULL,
                state TEXT NOT NULL,
                preserves_progress INTEGER NOT NULL,
                verified_chunks INTEGER NOT NULL,
                newly_verified_bytes INTEGER NOT NULL,
                avoided_retransmission_bytes INTEGER NOT NULL,
                updated_at_ms INTEGER NOT NULL
            )
        """)
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_finalization_journal (
                operation_id TEXT PRIMARY KEY,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                phase TEXT NOT NULL,
                detail_json TEXT NOT NULL,
                updated_at_ms INTEGER NOT NULL,
                error TEXT NOT NULL DEFAULT ''
            )
        """)
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_capacity_reservations (
                operation_id TEXT PRIMARY KEY,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                lease_id TEXT NOT NULL,
                owner_id TEXT NOT NULL,
                reserved_bytes INTEGER NOT NULL,
                actual_bytes INTEGER NOT NULL DEFAULT 0,
                expires_at_ms INTEGER NOT NULL,
                state TEXT NOT NULL,
                updated_at_ms INTEGER NOT NULL
            )
        """)
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_gc_claims (
                format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2',
                digest_algorithm TEXT NOT NULL DEFAULT 'sha256',
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                operation_id TEXT NOT NULL,
                gc_owner TEXT NOT NULL,
                claimed_at_ms INTEGER NOT NULL,
                deadline_ms INTEGER NOT NULL,
                state TEXT NOT NULL,
                PRIMARY KEY (artifact_digest, generation)
            )
        """)
        catalog_columns = self._table_columns(
            connection, "artifact_active_catalog"
        )
        receipt_columns = self._table_columns(
            connection, "artifact_replica_receipts"
        )
        if "format_version" not in receipt_columns:
            connection.execute(
                "ALTER TABLE artifact_replica_receipts ADD COLUMN "
                "format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2'"
            )
        if "digest_algorithm" not in receipt_columns:
            connection.execute(
                "ALTER TABLE artifact_replica_receipts ADD COLUMN "
                "digest_algorithm TEXT NOT NULL DEFAULT 'sha256'"
            )
        if "format_version" not in catalog_columns:
            connection.execute(
                "ALTER TABLE artifact_active_catalog ADD COLUMN "
                "format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2'"
            )
        if "digest_algorithm" not in catalog_columns:
            connection.execute(
                "ALTER TABLE artifact_active_catalog ADD COLUMN "
                "digest_algorithm TEXT NOT NULL DEFAULT 'sha256'"
            )
        gc_columns = self._table_columns(connection, "artifact_gc_claims")
        if "format_version" not in gc_columns:
            connection.execute(
                "ALTER TABLE artifact_gc_claims ADD COLUMN "
                "format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2'"
            )
        if "digest_algorithm" not in gc_columns:
            connection.execute(
                "ALTER TABLE artifact_gc_claims ADD COLUMN "
                "digest_algorithm TEXT NOT NULL DEFAULT 'sha256'"
            )
        self._backfill_format_identity(connection)
        self._catalog_has_format_identity = True
        connection.execute("""
            CREATE TABLE IF NOT EXISTS artifact_schema_state (
                singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                schema_generation INTEGER NOT NULL,
                format_version TEXT NOT NULL,
                migrated_at_ms INTEGER NOT NULL
            )
        """)
        connection.execute(
            "INSERT INTO artifact_schema_state("
            "singleton, schema_generation, format_version, migrated_at_ms"
            ") VALUES(1, ?, 'artifact-manifest-v2', ?) "
            "ON CONFLICT(singleton) DO UPDATE SET "
            "schema_generation=excluded.schema_generation, "
            "format_version=excluded.format_version, "
            "migrated_at_ms=excluded.migrated_at_ms",
            (self.SCHEMA_GENERATION, int(time.time() * 1000)),
        )
        connection.commit()
        self._artifact_writes_enabled = True
        self._migration_diagnostics = {
            "runtimeSchemaGeneration": self.SCHEMA_GENERATION,
            "databaseSchemaGeneration": self.SCHEMA_GENERATION,
            "maxWriteSchemaGeneration": self._max_write_schema_generation,
            "previousSchemaGeneration": stored_generation,
            "action": (
                "initialized"
                if stored_generation == 0
                else "roll-forward"
                if stored_generation < self.SCHEMA_GENERATION
                else "none"
            ),
            "writesEnabled": True,
            "reason": "",
            "destructiveChanges": False,
        }

    def commit(self) -> None:
        with self.lock:
            self._require_connection().commit()

    def rollback(self) -> None:
        with self.lock:
            self._require_connection().rollback()

    @staticmethod
    def _normalize_state(value: str) -> str:
        state = str(value).strip().upper()
        if state not in ARTIFACT_LIFECYCLE_STATES:
            raise LifecycleTransitionError(
                f"{REPO_LIFECYCLE_INVALID_STATE}: {value}"
            )
        return state

    @staticmethod
    def _event_from_row(row: Iterable) -> RepoLifecycleEvent:
        values = tuple(row)
        return RepoLifecycleEvent(
            sequence=int(values[0]),
            event_id=str(values[1]),
            operation_id=str(values[2]),
            artifact_digest=str(values[3]),
            generation=int(values[4]),
            from_state=str(values[5]),
            to_state=str(values[6]),
            event_time_ms=int(values[7]),
            accepted=bool(values[8]),
            detail=json.loads(str(values[9])),
            error=str(values[10]),
        )

    def _insert_event(
        self,
        connection: sqlite3.Connection,
        *,
        event_id: str,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        from_state: str,
        to_state: str,
        event_time_ms: int,
        accepted: bool,
        detail: dict,
        error: str,
    ) -> RepoLifecycleEvent:
        connection.execute("""
            INSERT INTO artifact_lifecycle_journal
              (event_id, operation_id, artifact_digest, generation,
               from_state, to_state, event_time_ms, accepted, detail_json, error)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            event_id,
            operation_id,
            artifact_digest,
            generation,
            from_state,
            to_state,
            event_time_ms,
            1 if accepted else 0,
            json.dumps(detail, sort_keys=True, separators=(",", ":")),
            error,
        ))
        row = connection.execute("""
            SELECT sequence, event_id, operation_id, artifact_digest, generation,
                   from_state, to_state, event_time_ms, accepted, detail_json, error
            FROM artifact_lifecycle_journal WHERE event_id=?
        """, (event_id,)).fetchone()
        assert row is not None
        return self._event_from_row(row)

    def transition(
        self,
        *,
        event_id: str,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        from_state: str,
        to_state: str,
        detail: dict | None = None,
        event_time_ms: int | None = None,
    ) -> RepoLifecycleEvent:
        self._require_artifact_writes_enabled()
        event_id = str(event_id).strip()
        operation_id = str(operation_id).strip()
        artifact_digest = str(artifact_digest).strip().lower()
        generation = int(generation)
        from_state = self._normalize_state(from_state)
        to_state = self._normalize_state(to_state)
        event_time_ms = int(event_time_ms or time.time() * 1000)
        detail = dict(detail or {})
        try:
            detail_json = json.dumps(
                detail, sort_keys=True, separators=(",", ":")
            ).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise LifecycleTransitionError(
                "repo-lifecycle-invalid-event: detail must be canonical JSON"
            ) from exc
        if (not event_id or len(event_id.encode("utf-8")) > 256
                or not operation_id or len(operation_id.encode("utf-8")) > 256
                or len(detail_json) > 16 * 1024
                or len(artifact_digest) != 64
                or any(ch not in "0123456789abcdef" for ch in artifact_digest)
                or generation < 0 or event_time_ms <= 0):
            raise LifecycleTransitionError(
                "repo-lifecycle-invalid-event: exact identity and time are required"
            )

        with self.lock:
            connection = self._require_connection()
            existing_row = connection.execute("""
                SELECT sequence, event_id, operation_id, artifact_digest, generation,
                       from_state, to_state, event_time_ms, accepted, detail_json, error
                FROM artifact_lifecycle_journal WHERE event_id=?
            """, (event_id,)).fetchone()
            if existing_row is not None:
                existing = self._event_from_row(existing_row)
                expected = (
                    operation_id, artifact_digest, generation, from_state, to_state,
                    event_time_ms, detail,
                )
                actual = (
                    existing.operation_id, existing.artifact_digest,
                    existing.generation, existing.from_state, existing.to_state,
                    existing.event_time_ms, existing.detail,
                )
                if actual != expected:
                    raise LifecycleTransitionError(
                        f"{REPO_LIFECYCLE_EVENT_CONFLICT}: {event_id}"
                    )
                if not existing.accepted:
                    raise LifecycleTransitionError(existing.error)
                return existing

            current_row = connection.execute("""
                SELECT artifact_digest, generation, to_state
                FROM artifact_lifecycle_journal
                WHERE operation_id=? AND accepted=1
                ORDER BY sequence DESC LIMIT 1
            """, (operation_id,)).fetchone()
            current_digest = str(current_row[0]) if current_row else artifact_digest
            current_generation = int(current_row[1]) if current_row else generation
            current_state = str(current_row[2]) if current_row else "ABSENT"
            error = ""
            if (current_digest != artifact_digest
                    or current_generation != generation):
                error = (
                    f"{REPO_LIFECYCLE_IDENTITY_CONFLICT}: operation identity "
                    "cannot change digest or generation"
                )
            elif current_state != from_state:
                error = (
                    f"{REPO_LIFECYCLE_STATE_CONFLICT}: expected {from_state}, "
                    f"authoritative state is {current_state}"
                )
            elif to_state not in ARTIFACT_LIFECYCLE_TRANSITIONS[from_state]:
                error = (
                    f"{REPO_LIFECYCLE_ILLEGAL_TRANSITION}: "
                    f"{from_state} -> {to_state}"
                )

            event = self._insert_event(
                connection,
                event_id=event_id,
                operation_id=operation_id,
                artifact_digest=artifact_digest,
                generation=generation,
                from_state=from_state,
                to_state=to_state,
                event_time_ms=event_time_ms,
                accepted=not error,
                detail=detail,
                error=error,
            )
            connection.commit()
            if error:
                raise LifecycleTransitionError(error)
            return event

    def lifecycle_events(self, operation_id: str) -> tuple[RepoLifecycleEvent, ...]:
        with self.lock:
            rows = self._require_connection().execute("""
                SELECT sequence, event_id, operation_id, artifact_digest, generation,
                       from_state, to_state, event_time_ms, accepted, detail_json, error
                FROM artifact_lifecycle_journal
                WHERE operation_id=? ORDER BY sequence
            """, (str(operation_id),)).fetchall()
        return tuple(self._event_from_row(row) for row in rows)

    @staticmethod
    def _canonical_json_object(value: dict[str, Any], field_name: str) -> str:
        try:
            encoded = json.dumps(
                dict(value), sort_keys=True, separators=(",", ":")
            )
        except (TypeError, ValueError) as exc:
            raise LifecycleTransitionError(
                f"repo-artifact-invalid-{field_name}: canonical JSON required"
            ) from exc
        if len(encoded.encode("utf-8")) > 64 * 1024:
            raise LifecycleTransitionError(
                f"repo-artifact-invalid-{field_name}: encoded value is too large"
            )
        return encoded

    @staticmethod
    def _transfer_session_from_row(
        row: Iterable,
    ) -> ArtifactTransferSessionRecord:
        values = tuple(row)
        return ArtifactTransferSessionRecord(
            operation_id=str(values[0]),
            artifact_digest=str(values[1]),
            generation=int(values[2]),
            identity=json.loads(str(values[3])),
            lease=json.loads(str(values[4])),
            state=str(values[5]),
            preserves_progress=bool(values[6]),
            verified_chunks=int(values[7]),
            newly_verified_bytes=int(values[8]),
            avoided_retransmission_bytes=int(values[9]),
            updated_at_ms=int(values[10]),
        )

    def transfer_session(
        self, operation_id: str
    ) -> ArtifactTransferSessionRecord | None:
        with self.lock:
            row = self._require_connection().execute("""
                SELECT operation_id, artifact_digest, generation, identity_json,
                       lease_json, state, preserves_progress, verified_chunks,
                       newly_verified_bytes, avoided_retransmission_bytes,
                       updated_at_ms
                FROM artifact_transfer_sessions WHERE operation_id=?
            """, (str(operation_id).strip(),)).fetchone()
        return None if row is None else self._transfer_session_from_row(row)

    def save_transfer_session(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        identity: dict[str, Any],
        lease: dict[str, Any],
        state: str,
        preserves_progress: bool,
        verified_chunks: int,
        newly_verified_bytes: int,
        avoided_retransmission_bytes: int,
        updated_at_ms: int,
    ) -> ArtifactTransferSessionRecord:
        """Durably save an exact-identity, monotonic resume checkpoint."""

        self._require_artifact_writes_enabled()
        operation_id = str(operation_id).strip()
        artifact_digest = str(artifact_digest).strip().lower()
        generation = int(generation)
        state = str(state).strip().upper()
        verified_chunks = int(verified_chunks)
        newly_verified_bytes = int(newly_verified_bytes)
        avoided_retransmission_bytes = int(avoided_retransmission_bytes)
        updated_at_ms = int(updated_at_ms)
        identity_json = self._canonical_json_object(identity, "resume-identity")
        lease_json = self._canonical_json_object(lease, "resume-lease")
        valid_states = {"OPEN", "CANCELLED", "EXPIRED", "COMPLETED", "FAILED"}
        if (
            not operation_id
            or len(artifact_digest) != 64
            or any(ch not in "0123456789abcdef" for ch in artifact_digest)
            or generation <= 0
            or state not in valid_states
            or min(
                verified_chunks,
                newly_verified_bytes,
                avoided_retransmission_bytes,
                updated_at_ms,
            ) < 0
        ):
            raise LifecycleTransitionError(
                "repo-transfer-session-invalid: bounded exact state required"
            )
        candidate = (
            operation_id,
            artifact_digest,
            generation,
            identity_json,
            lease_json,
            state,
            1 if preserves_progress else 0,
            verified_chunks,
            newly_verified_bytes,
            avoided_retransmission_bytes,
            updated_at_ms,
        )
        allowed = {
            "OPEN": {"OPEN", "CANCELLED", "EXPIRED", "COMPLETED", "FAILED"},
            "CANCELLED": {"CANCELLED", "OPEN", "FAILED"},
            "EXPIRED": {"EXPIRED", "OPEN", "FAILED"},
            "COMPLETED": {"COMPLETED"},
            "FAILED": {"FAILED"},
        }
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                gc_claim = connection.execute("""
                    SELECT state, deadline_ms FROM artifact_gc_claims
                    WHERE format_version='artifact-manifest-v2'
                          AND digest_algorithm='sha256'
                          AND artifact_digest=? AND generation=?
                """, (artifact_digest, generation)).fetchone()
                if (
                    state == "OPEN"
                    and gc_claim is not None
                    and str(gc_claim[0]) in {"CLAIMED", "RECLAIMING"}
                    and int(gc_claim[1]) > updated_at_ms
                ):
                    raise LifecycleTransitionError(
                        "repo-transfer-session-gc-owned"
                    )
                existing = connection.execute("""
                    SELECT operation_id, artifact_digest, generation, identity_json,
                           lease_json, state, preserves_progress, verified_chunks,
                           newly_verified_bytes, avoided_retransmission_bytes,
                           updated_at_ms
                    FROM artifact_transfer_sessions WHERE operation_id=?
                """, (operation_id,)).fetchone()
                if existing is None:
                    connection.execute("""
                        INSERT INTO artifact_transfer_sessions
                          (operation_id, artifact_digest, generation, identity_json,
                           lease_json, state, preserves_progress, verified_chunks,
                           newly_verified_bytes, avoided_retransmission_bytes,
                           updated_at_ms)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """, candidate)
                else:
                    prior = self._transfer_session_from_row(existing)
                    if (
                        prior.artifact_digest != artifact_digest
                        or prior.generation != generation
                        or existing[3] != identity_json
                        or state not in allowed[prior.state]
                        or updated_at_ms < prior.updated_at_ms
                        or newly_verified_bytes < prior.newly_verified_bytes
                        or avoided_retransmission_bytes
                        < prior.avoided_retransmission_bytes
                        or (
                            verified_chunks < prior.verified_chunks
                            and not (state == "FAILED" and not preserves_progress)
                        )
                    ):
                        raise LifecycleTransitionError(
                            "repo-transfer-session-conflict: identity, state, "
                            "or progress is not monotonic"
                        )
                    old_lease = prior.lease
                    new_lease = json.loads(lease_json)
                    lease_changed = old_lease != new_lease
                    renewal = (
                        new_lease.get("operationId") == operation_id
                        and new_lease.get("repoNode") == old_lease.get("repoNode")
                        and new_lease.get("artifact") == old_lease.get("artifact")
                        and int(new_lease.get("issuedAtMs", 0))
                        >= int(old_lease.get("issuedAtMs", 0))
                        and int(new_lease.get("expiresAtMs", 0))
                        > int(old_lease.get("expiresAtMs", 0))
                        and new_lease.get("leaseId") != old_lease.get("leaseId")
                        and new_lease.get("replayId") != old_lease.get("replayId")
                    )
                    if (
                        prior.state == "OPEN"
                        and state == "OPEN"
                        and lease_changed
                        and not renewal
                    ):
                        raise LifecycleTransitionError(
                            "repo-transfer-session-invalid-renewal"
                        )
                    if (
                        prior.state in {"CANCELLED", "EXPIRED"}
                        and state == "OPEN"
                        and not renewal
                    ):
                        raise LifecycleTransitionError(
                            "repo-transfer-session-invalid-renewal"
                        )
                    if (
                        lease_changed
                        and not (
                            state == "OPEN"
                            and prior.state in {
                                "OPEN", "CANCELLED", "EXPIRED"
                            }
                        )
                    ):
                        raise LifecycleTransitionError(
                            "repo-transfer-session-unexpected-lease-change"
                        )
                    connection.execute("""
                        UPDATE artifact_transfer_sessions
                        SET lease_json=?, state=?, preserves_progress=?,
                            verified_chunks=?, newly_verified_bytes=?,
                            avoided_retransmission_bytes=?, updated_at_ms=?
                        WHERE operation_id=?
                    """, (
                        lease_json,
                        state,
                        1 if preserves_progress else 0,
                        verified_chunks,
                        newly_verified_bytes,
                        avoided_retransmission_bytes,
                        updated_at_ms,
                        operation_id,
                    ))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise
        record = self.transfer_session(operation_id)
        assert record is not None
        return record

    @staticmethod
    def _finalization_from_row(row: Iterable) -> ArtifactFinalizationRecord:
        values = tuple(row)
        return ArtifactFinalizationRecord(
            operation_id=str(values[0]),
            artifact_digest=str(values[1]),
            generation=int(values[2]),
            phase=str(values[3]),
            detail=json.loads(str(values[4])),
            updated_at_ms=int(values[5]),
            error=str(values[6]),
        )

    def finalization_record(
        self, operation_id: str
    ) -> ArtifactFinalizationRecord | None:
        with self.lock:
            row = self._require_connection().execute("""
                SELECT operation_id, artifact_digest, generation, phase,
                       detail_json, updated_at_ms, error
                FROM artifact_finalization_journal WHERE operation_id=?
            """, (str(operation_id).strip(),)).fetchone()
        return None if row is None else self._finalization_from_row(row)

    def begin_finalization(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        logical_name: str,
        policy_epoch: str,
        artifact: dict[str, Any],
        receipt_id: str,
        receipt: dict[str, Any],
        repo_node: str,
        signer_key_id: str,
        authentication_algorithm: str,
        signature_hex: str,
        committed_at_ms: int,
    ) -> ArtifactFinalizationRecord:
        """Record all replay material before crossing the payload DB boundary."""

        self._require_artifact_writes_enabled()
        operation_id = str(operation_id).strip()
        artifact_digest = str(artifact_digest).strip().lower()
        generation = int(generation)
        committed_at_ms = int(committed_at_ms)
        detail = {
            "logicalName": str(logical_name).strip(),
            "policyEpoch": str(policy_epoch).strip(),
            "artifact": dict(artifact),
            "receiptId": str(receipt_id).strip(),
            "receipt": dict(receipt),
            "repoNode": str(repo_node).strip(),
            "signerKeyId": str(signer_key_id).strip(),
            "authenticationAlgorithm": str(authentication_algorithm).strip(),
            "signatureHex": str(signature_hex).strip().lower(),
            "committedAtMs": committed_at_ms,
        }
        detail_json = self._canonical_json_object(detail, "finalization")
        required = (
            operation_id,
            detail["logicalName"],
            detail["policyEpoch"],
            detail["receiptId"],
            detail["repoNode"],
            detail["signerKeyId"],
            detail["authenticationAlgorithm"],
            detail["signatureHex"],
        )
        artifact_value = detail["artifact"]
        receipt_value = detail["receipt"]
        if (
            not all(required)
            or len(artifact_digest) != 64
            or any(ch not in "0123456789abcdef" for ch in artifact_digest)
            or generation <= 0
            or committed_at_ms <= 0
            or artifact_value.get("contentDigest") != artifact_digest
            or int(artifact_value.get("sizeBytes", -1)) < 0
            or receipt_value.get("operationId") != operation_id
            or receipt_value.get("artifact") != artifact_value
            or len(detail["signatureHex"]) > 16 * 1024
            or any(
                ch not in "0123456789abcdef"
                for ch in detail["signatureHex"]
            )
        ):
            raise LifecycleTransitionError(
                "repo-finalization-invalid-intent: exact replay material required"
            )
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                gc_claim = connection.execute("""
                    SELECT state, deadline_ms FROM artifact_gc_claims
                    WHERE format_version=? AND digest_algorithm=?
                          AND artifact_digest=? AND generation=?
                """, (
                    str(artifact_value.get(
                        "formatVersion", "artifact-manifest-v2"
                    )),
                    str(artifact_value.get("digestAlgorithm", "sha256")),
                    artifact_digest,
                    generation,
                )).fetchone()
                if (
                    gc_claim is not None
                    and str(gc_claim[0]) in {"CLAIMED", "RECLAIMING"}
                    and int(gc_claim[1]) > committed_at_ms
                ):
                    raise LifecycleTransitionError(
                        "repo-finalization-gc-owned"
                    )
                current = connection.execute("""
                    SELECT artifact_digest, generation, to_state
                    FROM artifact_lifecycle_journal
                    WHERE operation_id=? AND accepted=1
                    ORDER BY sequence DESC LIMIT 1
                """, (operation_id,)).fetchone()
                if current not in {
                    (artifact_digest, generation, "VERIFIED"),
                    (artifact_digest, generation, "COMMITTED"),
                    (artifact_digest, generation, "ACTIVE"),
                }:
                    raise LifecycleTransitionError(
                        "repo-finalization-state-conflict: VERIFIED state required"
                    )
                existing = connection.execute("""
                    SELECT operation_id, artifact_digest, generation, phase,
                           detail_json, updated_at_ms, error
                    FROM artifact_finalization_journal WHERE operation_id=?
                """, (operation_id,)).fetchone()
                if existing is None:
                    connection.execute("""
                        INSERT INTO artifact_finalization_journal
                          (operation_id, artifact_digest, generation, phase,
                           detail_json, updated_at_ms, error)
                        VALUES (?, ?, ?, 'INTENT_RECORDED', ?, ?, '')
                    """, (
                        operation_id,
                        artifact_digest,
                        generation,
                        detail_json,
                        committed_at_ms,
                    ))
                    connection.execute("""
                        UPDATE artifact_capacity_reservations
                        SET state='FINALIZING', updated_at_ms=?
                        WHERE operation_id=? AND artifact_digest=?
                              AND generation=? AND state='ACTIVE'
                    """, (
                        committed_at_ms,
                        operation_id,
                        artifact_digest,
                        generation,
                    ))
                elif (
                    str(existing[1]) != artifact_digest
                    or int(existing[2]) != generation
                    or str(existing[4]) != detail_json
                ):
                    raise LifecycleTransitionError(
                        "repo-finalization-intent-conflict"
                    )
                connection.commit()
            except BaseException:
                connection.rollback()
                raise
        result = self.finalization_record(operation_id)
        assert result is not None
        return result

    def mark_payload_finalized(
        self, operation_id: str, updated_at_ms: int
    ) -> ArtifactFinalizationRecord:
        self._require_artifact_writes_enabled()
        record = self.finalization_record(operation_id)
        if record is None:
            raise LifecycleTransitionError("repo-finalization-intent-missing")
        artifact = record.detail["artifact"]
        identity = ArtifactStorageIdentity(
            content_digest=record.artifact_digest,
            size_bytes=int(artifact["sizeBytes"]),
            generation=record.generation,
            digest_algorithm=str(artifact.get("digestAlgorithm", "sha256")),
        )
        if not self.artifact_payload_store.verify_committed(identity):
            raise LifecycleTransitionError(
                "repo-finalization-payload-not-durable"
            )
        with self.lock:
            connection = self._require_connection()
            if record.phase == "INTENT_RECORDED":
                connection.execute("""
                    UPDATE artifact_finalization_journal
                    SET phase='PAYLOAD_FINALIZED', updated_at_ms=?, error=''
                    WHERE operation_id=? AND phase='INTENT_RECORDED'
                """, (int(updated_at_ms), record.operation_id))
                connection.execute("""
                    UPDATE artifact_capacity_reservations
                    SET actual_bytes=?, updated_at_ms=?
                    WHERE operation_id=?
                """, (
                    int(artifact["sizeBytes"]),
                    int(updated_at_ms),
                    record.operation_id,
                ))
                connection.commit()
            elif record.phase not in {
                "PAYLOAD_FINALIZED", "METADATA_COMMITTED", "ACTIVE"
            }:
                raise LifecycleTransitionError(
                    "repo-finalization-invalid-payload-phase"
                )
        result = self.finalization_record(record.operation_id)
        assert result is not None
        return result

    def commit_finalized_artifact(
        self, operation_id: str
    ) -> ArtifactFinalizationRecord:
        self._require_artifact_writes_enabled()
        record = self.finalization_record(operation_id)
        if record is None or record.phase not in {
            "PAYLOAD_FINALIZED", "METADATA_COMMITTED", "ACTIVE"
        }:
            raise LifecycleTransitionError(
                "repo-finalization-payload-phase-required"
            )
        if record.phase in {"METADATA_COMMITTED", "ACTIVE"}:
            return record
        detail = record.detail
        receipt_json = self._canonical_json_object(
            detail["receipt"], "receipt"
        )
        expected_receipt = (
            detail["receiptId"],
            record.operation_id,
            str(detail["artifact"].get(
                "formatVersion", "artifact-manifest-v2"
            )),
            str(detail["artifact"].get("digestAlgorithm", "sha256")),
            record.artifact_digest,
            detail["repoNode"],
            record.generation,
            receipt_json,
            detail["signerKeyId"],
            detail["authenticationAlgorithm"],
            detail["signatureHex"],
            int(detail["committedAtMs"]),
        )
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                existing = connection.execute("""
                    SELECT receipt_id, operation_id, format_version,
                           digest_algorithm, artifact_digest, repo_node,
                           generation, receipt_json, signer_key_id,
                           authentication_algorithm, signature_hex, committed_at_ms
                    FROM artifact_replica_receipts WHERE operation_id=?
                """, (record.operation_id,)).fetchone()
                if existing is None:
                    connection.execute("""
                        INSERT INTO artifact_replica_receipts
                          (receipt_id, operation_id, format_version,
                           digest_algorithm, artifact_digest, repo_node,
                           generation, receipt_json, signer_key_id,
                           authentication_algorithm, signature_hex, committed_at_ms)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """, expected_receipt)
                elif tuple(existing) != expected_receipt:
                    raise LifecycleTransitionError(
                        "repo-artifact-receipt-conflict"
                    )
                current = connection.execute("""
                    SELECT artifact_digest, generation, to_state
                    FROM artifact_lifecycle_journal
                    WHERE operation_id=? AND accepted=1
                    ORDER BY sequence DESC LIMIT 1
                """, (record.operation_id,)).fetchone()
                if current == (
                    record.artifact_digest, record.generation, "VERIFIED"
                ):
                    self._insert_event(
                        connection,
                        event_id=f"{record.operation_id}:committed",
                        operation_id=record.operation_id,
                        artifact_digest=record.artifact_digest,
                        generation=record.generation,
                        from_state="VERIFIED",
                        to_state="COMMITTED",
                        event_time_ms=int(detail["committedAtMs"]),
                        accepted=True,
                        detail={"receiptId": detail["receiptId"]},
                        error="",
                    )
                elif current not in {
                    (record.artifact_digest, record.generation, "COMMITTED"),
                    (record.artifact_digest, record.generation, "ACTIVE"),
                }:
                    raise LifecycleTransitionError(
                        "repo-finalization-metadata-state-conflict"
                    )
                connection.execute("""
                    UPDATE artifact_finalization_journal
                    SET phase='METADATA_COMMITTED', updated_at_ms=?, error=''
                    WHERE operation_id=?
                """, (
                    max(record.updated_at_ms, int(detail["committedAtMs"])),
                    record.operation_id,
                ))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise
        result = self.finalization_record(record.operation_id)
        assert result is not None
        return result

    def activate_finalized_artifact(
        self, operation_id: str
    ) -> ArtifactFinalizationRecord:
        self._require_artifact_writes_enabled()
        record = self.finalization_record(operation_id)
        if record is None or record.phase not in {
            "METADATA_COMMITTED", "ACTIVE"
        }:
            raise LifecycleTransitionError(
                "repo-finalization-metadata-phase-required"
            )
        if record.phase == "ACTIVE":
            return record
        detail = record.detail
        format_version = str(
            detail["artifact"].get("formatVersion", "artifact-manifest-v2")
        )
        digest_algorithm = str(
            detail["artifact"].get("digestAlgorithm", "sha256")
        )
        artifact_json = self._canonical_json_object(
            detail["artifact"], "artifact"
        )
        expected_catalog = (
            format_version,
            digest_algorithm,
            record.artifact_digest,
            record.generation,
            record.operation_id,
            artifact_json,
            int(detail["committedAtMs"]),
        )
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                existing = connection.execute("""
                    SELECT format_version, digest_algorithm, artifact_digest,
                           generation, operation_id,
                           artifact_json, activated_at_ms
                    FROM artifact_active_catalog
                    WHERE logical_name=? AND policy_epoch=?
                """, (
                    detail["logicalName"], detail["policyEpoch"]
                )).fetchone()
                if existing is None:
                    connection.execute("""
                        INSERT INTO artifact_active_catalog
                          (logical_name, policy_epoch, format_version,
                           digest_algorithm, artifact_digest, generation,
                           operation_id, artifact_json, activated_at_ms)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """, (
                        detail["logicalName"],
                        detail["policyEpoch"],
                        *expected_catalog,
                    ))
                elif tuple(existing) != expected_catalog:
                    raise LifecycleTransitionError(
                        "repo-artifact-catalog-conflict"
                    )
                current = connection.execute("""
                    SELECT artifact_digest, generation, to_state
                    FROM artifact_lifecycle_journal
                    WHERE operation_id=? AND accepted=1
                    ORDER BY sequence DESC LIMIT 1
                """, (record.operation_id,)).fetchone()
                if current == (
                    record.artifact_digest, record.generation, "COMMITTED"
                ):
                    self._insert_event(
                        connection,
                        event_id=f"{record.operation_id}:active",
                        operation_id=record.operation_id,
                        artifact_digest=record.artifact_digest,
                        generation=record.generation,
                        from_state="COMMITTED",
                        to_state="ACTIVE",
                        event_time_ms=int(detail["committedAtMs"]),
                        accepted=True,
                        detail={"receiptId": detail["receiptId"]},
                        error="",
                    )
                elif current != (
                    record.artifact_digest, record.generation, "ACTIVE"
                ):
                    raise LifecycleTransitionError(
                        "repo-finalization-activation-state-conflict"
                    )
                connection.execute("""
                    UPDATE artifact_finalization_journal
                    SET phase='ACTIVE', updated_at_ms=?, error=''
                    WHERE operation_id=?
                """, (
                    max(record.updated_at_ms, int(detail["committedAtMs"])),
                    record.operation_id,
                ))
                connection.execute("""
                    UPDATE artifact_capacity_reservations
                    SET state='COMMITTED', actual_bytes=?, updated_at_ms=?
                    WHERE operation_id=?
                """, (
                    int(detail["artifact"]["sizeBytes"]),
                    int(detail["committedAtMs"]),
                    record.operation_id,
                ))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise
        result = self.finalization_record(record.operation_id)
        assert result is not None
        return result

    def reconcile_finalizations(self) -> tuple[ArtifactFinalizationRecord, ...]:
        """Replay every durable nonterminal intent without filename guessing."""

        self._require_artifact_writes_enabled()
        with self.lock:
            rows = self._require_connection().execute("""
                SELECT operation_id FROM artifact_finalization_journal
                WHERE phase NOT IN ('ACTIVE', 'ROLLED_BACK')
                ORDER BY operation_id
            """).fetchall()
        results: list[ArtifactFinalizationRecord] = []
        for (operation_id,) in rows:
            try:
                record = self.finalization_record(str(operation_id))
                assert record is not None
                if record.phase == "INTENT_RECORDED":
                    artifact = record.detail["artifact"]
                    identity = ArtifactStorageIdentity(
                        content_digest=record.artifact_digest,
                        size_bytes=int(artifact["sizeBytes"]),
                        generation=record.generation,
                        digest_algorithm=str(
                            artifact.get("digestAlgorithm", "sha256")
                        ),
                    )
                    self.artifact_payload_store.finalize(identity)
                    record = self.mark_payload_finalized(
                        record.operation_id,
                        max(record.updated_at_ms, int(time.time() * 1000)),
                    )
                if record.phase == "PAYLOAD_FINALIZED":
                    record = self.commit_finalized_artifact(
                        record.operation_id
                    )
                if record.phase == "METADATA_COMMITTED":
                    record = self.activate_finalized_artifact(
                        record.operation_id
                    )
                results.append(record)
            except BaseException as exc:
                with self.lock:
                    connection = self._require_connection()
                    connection.execute("""
                        UPDATE artifact_finalization_journal
                        SET error=?, updated_at_ms=?
                        WHERE operation_id=?
                    """, (
                        str(exc)[:1024],
                        int(time.time() * 1000),
                        str(operation_id),
                    ))
                    connection.commit()
                failed = self.finalization_record(str(operation_id))
                if failed is not None:
                    results.append(failed)
        return tuple(results)

    def rollback_finalization(
        self, operation_id: str, *, reason: str, now_ms: int
    ) -> ArtifactFinalizationRecord:
        """Fail closed and release an unrecoverable pre-commit intent."""

        self._require_artifact_writes_enabled()
        record = self.finalization_record(operation_id)
        if record is None or record.phase not in {
            "INTENT_RECORDED", "PAYLOAD_FINALIZED"
        }:
            raise LifecycleTransitionError(
                "repo-finalization-rollback-not-allowed"
            )
        artifact = record.detail["artifact"]
        identity = ArtifactStorageIdentity(
            content_digest=record.artifact_digest,
            size_bytes=int(artifact["sizeBytes"]),
            generation=record.generation,
            digest_algorithm=str(artifact.get("digestAlgorithm", "sha256")),
        )
        remove_finalized = False
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                protected = connection.execute("""
                    SELECT 1 FROM artifact_replica_receipts
                    WHERE artifact_digest=? AND generation=?
                    UNION ALL
                    SELECT 1 FROM artifact_active_catalog
                    WHERE artifact_digest=? AND generation=?
                    UNION ALL
                    SELECT 1 FROM artifact_finalization_journal
                    WHERE artifact_digest=? AND generation=?
                          AND operation_id!=?
                          AND phase NOT IN ('ROLLED_BACK')
                    LIMIT 1
                """, (
                    record.artifact_digest,
                    record.generation,
                    record.artifact_digest,
                    record.generation,
                    record.artifact_digest,
                    record.generation,
                    record.operation_id,
                )).fetchone()
                remove_finalized = protected is None
                current = connection.execute("""
                    SELECT to_state FROM artifact_lifecycle_journal
                    WHERE operation_id=? AND accepted=1
                    ORDER BY sequence DESC LIMIT 1
                """, (record.operation_id,)).fetchone()
                if current is not None and str(current[0]) == "VERIFIED":
                    self._insert_event(
                        connection,
                        event_id=f"{record.operation_id}:rollback-failed",
                        operation_id=record.operation_id,
                        artifact_digest=record.artifact_digest,
                        generation=record.generation,
                        from_state="VERIFIED",
                        to_state="FAILED",
                        event_time_ms=int(now_ms),
                        accepted=True,
                        detail={"reason": str(reason)[:1024]},
                        error="",
                    )
                connection.execute("""
                    UPDATE artifact_finalization_journal
                    SET phase='ROLLED_BACK', updated_at_ms=?, error=?
                    WHERE operation_id=?
                """, (
                    int(now_ms), str(reason)[:1024], record.operation_id
                ))
                connection.execute("""
                    UPDATE artifact_transfer_sessions
                    SET state='FAILED', preserves_progress=0,
                        verified_chunks=0, updated_at_ms=?
                    WHERE operation_id=?
                """, (int(now_ms), record.operation_id))
                connection.execute("""
                    UPDATE artifact_capacity_reservations
                    SET state='RELEASED', updated_at_ms=?
                    WHERE operation_id=?
                """, (int(now_ms), record.operation_id))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise
        self.artifact_payload_store.abort(identity)
        if remove_finalized:
            self.artifact_payload_store.reclaim_unreferenced_finalized(identity)
        result = self.finalization_record(record.operation_id)
        assert result is not None
        return result

    def commit_and_activate_artifact(self, **values) -> dict[str, Any]:
        """Compatibility wrapper routed through the recovery journal."""

        self._require_artifact_writes_enabled()
        record = self.begin_finalization(**values)
        artifact = record.detail["artifact"]
        identity = ArtifactStorageIdentity(
            content_digest=record.artifact_digest,
            size_bytes=int(artifact["sizeBytes"]),
            generation=record.generation,
            digest_algorithm=str(artifact.get("digestAlgorithm", "sha256")),
        )
        if not self.artifact_payload_store.verify_committed(identity):
            raise LifecycleTransitionError(
                "repo-finalization-payload-not-durable"
            )
        self.mark_payload_finalized(
            record.operation_id, int(record.detail["committedAtMs"])
        )
        self.commit_finalized_artifact(record.operation_id)
        self.activate_finalized_artifact(record.operation_id)
        return dict(record.detail["receipt"])

    def _commit_and_activate_artifact_legacy_unjournaled(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        logical_name: str,
        policy_epoch: str,
        artifact: dict[str, Any],
        receipt_id: str,
        receipt: dict[str, Any],
        repo_node: str,
        signer_key_id: str,
        authentication_algorithm: str,
        signature_hex: str,
        committed_at_ms: int,
    ) -> dict[str, Any]:
        """Atomically retain one receipt, commit, and activate one replica.

        The payload must already be atomically finalized. The authoritative
        lifecycle journal must be in VERIFIED state. A retry with byte-identical
        values is idempotent; any identity or logical-name conflict fails closed.
        """

        self._require_artifact_writes_enabled()
        operation_id = str(operation_id).strip()
        artifact_digest = str(artifact_digest).strip().lower()
        logical_name = str(logical_name).strip()
        policy_epoch = str(policy_epoch).strip()
        receipt_id = str(receipt_id).strip()
        repo_node = str(repo_node).strip()
        signer_key_id = str(signer_key_id).strip()
        authentication_algorithm = str(authentication_algorithm).strip()
        signature_hex = str(signature_hex).strip().lower()
        generation = int(generation)
        committed_at_ms = int(committed_at_ms)
        artifact_json = self._canonical_json_object(artifact, "artifact")
        format_version = str(
            artifact.get("formatVersion", "artifact-manifest-v2")
        )
        digest_algorithm = str(artifact.get("digestAlgorithm", "sha256"))
        receipt_json = self._canonical_json_object(receipt, "receipt")
        if (
            not all((
                operation_id,
                logical_name,
                policy_epoch,
                receipt_id,
                repo_node,
                signer_key_id,
                authentication_algorithm,
                signature_hex,
            ))
            or len(artifact_digest) != 64
            or any(ch not in "0123456789abcdef" for ch in artifact_digest)
            or generation <= 0
            or committed_at_ms <= 0
            or len(signature_hex) > 16 * 1024
            or any(ch not in "0123456789abcdef" for ch in signature_hex)
        ):
            raise LifecycleTransitionError(
                "repo-artifact-invalid-activation: exact authenticated identity "
                "is required"
            )

        expected_receipt = (
            receipt_id,
            operation_id,
            format_version,
            digest_algorithm,
            artifact_digest,
            repo_node,
            generation,
            receipt_json,
            signer_key_id,
            authentication_algorithm,
            signature_hex,
            committed_at_ms,
        )
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                existing = connection.execute("""
                    SELECT receipt_id, operation_id, format_version,
                           digest_algorithm, artifact_digest, repo_node,
                           generation, receipt_json, signer_key_id,
                           authentication_algorithm, signature_hex, committed_at_ms
                    FROM artifact_replica_receipts WHERE operation_id=?
                """, (operation_id,)).fetchone()
                if existing is not None:
                    if tuple(existing) != expected_receipt:
                        raise LifecycleTransitionError(
                            "repo-artifact-receipt-conflict: operation already "
                            "has a different authenticated receipt"
                        )
                    catalog = connection.execute("""
                        SELECT format_version, digest_algorithm, artifact_digest,
                               generation, operation_id,
                               artifact_json, activated_at_ms
                        FROM artifact_active_catalog
                        WHERE logical_name=? AND policy_epoch=?
                    """, (logical_name, policy_epoch)).fetchone()
                    if catalog != (
                        format_version,
                        digest_algorithm,
                        artifact_digest,
                        generation,
                        operation_id,
                        artifact_json,
                        committed_at_ms,
                    ):
                        raise LifecycleTransitionError(
                            "repo-artifact-catalog-conflict: receipt exists "
                            "without its exact active catalog entry"
                        )
                    connection.commit()
                    return json.loads(receipt_json)

                current = connection.execute("""
                    SELECT artifact_digest, generation, to_state
                    FROM artifact_lifecycle_journal
                    WHERE operation_id=? AND accepted=1
                    ORDER BY sequence DESC LIMIT 1
                """, (operation_id,)).fetchone()
                if current != (artifact_digest, generation, "VERIFIED"):
                    raise LifecycleTransitionError(
                        "repo-artifact-activation-state-conflict: authoritative "
                        "state must be VERIFIED"
                    )
                catalog = connection.execute("""
                    SELECT format_version, digest_algorithm, artifact_digest,
                           generation, operation_id, artifact_json
                    FROM artifact_active_catalog
                    WHERE logical_name=? AND policy_epoch=?
                """, (logical_name, policy_epoch)).fetchone()
                if catalog is not None and catalog != (
                    format_version,
                    digest_algorithm,
                    artifact_digest,
                    generation,
                    operation_id,
                    artifact_json,
                ):
                    raise LifecycleTransitionError(
                        "repo-artifact-catalog-conflict: logical name and policy "
                        "epoch already identify different content"
                    )

                connection.execute("""
                    INSERT INTO artifact_replica_receipts
                      (receipt_id, operation_id, format_version,
                       digest_algorithm, artifact_digest, repo_node,
                       generation, receipt_json, signer_key_id,
                       authentication_algorithm, signature_hex, committed_at_ms)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """, expected_receipt)
                self._insert_event(
                    connection,
                    event_id=f"{operation_id}:committed",
                    operation_id=operation_id,
                    artifact_digest=artifact_digest,
                    generation=generation,
                    from_state="VERIFIED",
                    to_state="COMMITTED",
                    event_time_ms=committed_at_ms,
                    accepted=True,
                    detail={"receiptId": receipt_id},
                    error="",
                )
                self._insert_event(
                    connection,
                    event_id=f"{operation_id}:active",
                    operation_id=operation_id,
                    artifact_digest=artifact_digest,
                    generation=generation,
                    from_state="COMMITTED",
                    to_state="ACTIVE",
                    event_time_ms=committed_at_ms,
                    accepted=True,
                    detail={"receiptId": receipt_id},
                    error="",
                )
                connection.execute("""
                    INSERT INTO artifact_active_catalog
                      (logical_name, policy_epoch, format_version,
                       digest_algorithm, artifact_digest, generation,
                       operation_id, artifact_json, activated_at_ms)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(logical_name, policy_epoch) DO NOTHING
                """, (
                    logical_name,
                    policy_epoch,
                    format_version,
                    digest_algorithm,
                    artifact_digest,
                    generation,
                    operation_id,
                    artifact_json,
                    committed_at_ms,
                ))
                connection.commit()
                return json.loads(receipt_json)
            except BaseException:
                connection.rollback()
                raise

    def capacity_status(self) -> ArtifactCapacityStatus:
        with self.lock:
            connection = self._require_connection()
            catalog_rows = connection.execute("""
                SELECT artifact_digest, generation, artifact_json
                FROM artifact_active_catalog
            """).fetchall()
            committed_by_identity: dict[tuple[str, int], int] = {}
            for digest, generation, artifact_json in catalog_rows:
                artifact = json.loads(str(artifact_json))
                committed_by_identity[(str(digest), int(generation))] = int(
                    artifact["sizeBytes"]
                )
            committed = sum(committed_by_identity.values())
            reserved = int(connection.execute("""
                SELECT COALESCE(SUM(reserved_bytes), 0)
                FROM artifact_capacity_reservations
                WHERE state IN ('ACTIVE', 'FINALIZING')
            """).fetchone()[0])
        filesystem_free = int(
            shutil.disk_usage(Path(self.database_path).parent).free
        )
        if self.capacity_bytes is None:
            capacity = committed + reserved + filesystem_free
            available = filesystem_free
        else:
            capacity = self.capacity_bytes
            available = min(
                filesystem_free, max(0, capacity - committed - reserved)
            )
        return ArtifactCapacityStatus(
            capacity_bytes=capacity,
            committed_bytes=committed,
            reserved_bytes=reserved,
            available_bytes=available,
        )

    def reserve_artifact_capacity(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        lease_id: str,
        reserved_bytes: int,
        expires_at_ms: int,
        now_ms: int,
    ) -> ArtifactCapacityStatus:
        self._require_artifact_writes_enabled()
        operation_id = str(operation_id).strip()
        artifact_digest = str(artifact_digest).strip().lower()
        lease_id = str(lease_id).strip()
        generation = int(generation)
        declared_bytes = int(reserved_bytes)
        deduplicated = False
        if declared_bytes >= 0 and len(artifact_digest) == 64 and generation > 0:
            try:
                deduplicated = self.artifact_payload_store.verify_committed(
                    ArtifactStorageIdentity(
                        content_digest=artifact_digest,
                        size_bytes=declared_bytes,
                        generation=generation,
                    )
                )
            except ValueError:
                deduplicated = False
        reserved_bytes = (
            self.reservation_overhead_bytes
            if deduplicated
            else declared_bytes + self.reservation_overhead_bytes
        )
        expires_at_ms = int(expires_at_ms)
        now_ms = int(now_ms)
        if (
            not operation_id
            or not lease_id
            or len(artifact_digest) != 64
            or generation <= 0
            or declared_bytes < 0
            or reserved_bytes < self.reservation_overhead_bytes
            or now_ms >= expires_at_ms
        ):
            raise LifecycleTransitionError(
                "repo-capacity-invalid-reservation"
            )
        with self.lock:
            connection = self._require_connection()
            existing = connection.execute("""
                SELECT artifact_digest, generation, lease_id, owner_id,
                       reserved_bytes, expires_at_ms, state
                FROM artifact_capacity_reservations WHERE operation_id=?
            """, (operation_id,)).fetchone()
            expected_binding = (
                artifact_digest,
                generation,
                lease_id,
                reserved_bytes,
                expires_at_ms,
            )
            if existing is not None:
                durable_binding = (
                    existing[0],
                    existing[1],
                    existing[2],
                    existing[4],
                    existing[5],
                )
                if durable_binding != expected_binding:
                    raise LifecycleTransitionError(
                        "repo-capacity-reservation-conflict"
                    )
                if str(existing[6]) == "RELEASED":
                    raise LifecycleTransitionError(
                        "repo-capacity-reservation-released"
                    )
                if str(existing[3]) != self.owner_id:
                    connection.execute("""
                        UPDATE artifact_capacity_reservations
                        SET owner_id=?, updated_at_ms=?
                        WHERE operation_id=? AND state='ACTIVE'
                    """, (self.owner_id, now_ms, operation_id))
                    connection.commit()
                return self.capacity_status()
            status = self.capacity_status()
            if reserved_bytes > status.available_bytes:
                raise LifecycleTransitionError(
                    "repo-capacity-insufficient: reservation exceeds safe "
                    "configured or filesystem capacity"
                )
            connection.execute("""
                INSERT INTO artifact_capacity_reservations
                  (operation_id, artifact_digest, generation, lease_id,
                   owner_id, reserved_bytes, actual_bytes, expires_at_ms,
                   state, updated_at_ms)
                VALUES (?, ?, ?, ?, ?, ?, 0, ?, 'ACTIVE', ?)
            """, (
                operation_id,
                artifact_digest,
                generation,
                lease_id,
                self.owner_id,
                reserved_bytes,
                expires_at_ms,
                now_ms,
            ))
            connection.commit()
        return self.capacity_status()

    def release_artifact_capacity(
        self, operation_id: str, now_ms: int
    ) -> None:
        self._require_artifact_writes_enabled()
        with self.lock:
            connection = self._require_connection()
            connection.execute("""
                UPDATE artifact_capacity_reservations
                SET state='RELEASED', updated_at_ms=?
                WHERE operation_id=? AND state IN ('ACTIVE', 'FINALIZING')
            """, (int(now_ms), str(operation_id).strip()))
            connection.commit()

    def renew_artifact_capacity(
        self,
        *,
        operation_id: str,
        lease_id: str,
        expires_at_ms: int,
        now_ms: int,
    ) -> None:
        self._require_artifact_writes_enabled()
        with self.lock:
            connection = self._require_connection()
            row = connection.execute("""
                SELECT lease_id, expires_at_ms, state
                FROM artifact_capacity_reservations WHERE operation_id=?
            """, (str(operation_id).strip(),)).fetchone()
            if row is None:
                return
            if (
                str(row[2]) != "ACTIVE"
                or str(lease_id).strip() == str(row[0])
                or int(expires_at_ms) <= int(row[1])
                or int(now_ms) >= int(expires_at_ms)
            ):
                raise LifecycleTransitionError(
                    "repo-capacity-invalid-renewal"
                )
            connection.execute("""
                UPDATE artifact_capacity_reservations
                SET lease_id=?, expires_at_ms=?, updated_at_ms=?
                WHERE operation_id=? AND state='ACTIVE'
            """, (
                str(lease_id).strip(),
                int(expires_at_ms),
                int(now_ms),
                str(operation_id).strip(),
            ))
            connection.commit()

    def _gc_protection_reason(
        self,
        connection: sqlite3.Connection,
        artifact_digest: str,
        generation: int,
        now_ms: int,
        format_version: str = "artifact-manifest-v2",
        digest_algorithm: str = "sha256",
    ) -> str:
        if connection.execute("""
            SELECT 1 FROM artifact_active_catalog
            WHERE format_version=? AND digest_algorithm=?
                  AND artifact_digest=? AND generation=? LIMIT 1
        """, (
            format_version, digest_algorithm, artifact_digest, generation
        )).fetchone():
            return "active catalog reference"
        if connection.execute("""
            SELECT 1 FROM artifact_replica_receipts
            WHERE format_version=? AND digest_algorithm=?
                  AND artifact_digest=? AND generation=? LIMIT 1
        """, (
            format_version, digest_algorithm, artifact_digest, generation
        )).fetchone():
            return "retained committed receipt"
        finalization = connection.execute("""
            SELECT phase FROM artifact_finalization_journal
            WHERE artifact_digest=? AND generation=? LIMIT 1
        """, (artifact_digest, generation)).fetchone()
        if finalization is not None and str(finalization[0]) != "ROLLED_BACK":
            return "finalization journal"
        reservation = connection.execute("""
            SELECT state, expires_at_ms FROM artifact_capacity_reservations
            WHERE artifact_digest=? AND generation=? LIMIT 1
        """, (artifact_digest, generation)).fetchone()
        if (
            reservation is not None
            and str(reservation[0]) in {"ACTIVE", "FINALIZING", "COMMITTED"}
            and (
                str(reservation[0]) != "ACTIVE"
                or int(reservation[1]) > now_ms
            )
        ):
            return "capacity reservation or committed generation"
        sessions = connection.execute("""
            SELECT state, lease_json FROM artifact_transfer_sessions
            WHERE artifact_digest=? AND generation=?
        """, (artifact_digest, generation)).fetchall()
        for state, lease_json in sessions:
            lease = json.loads(str(lease_json))
            if (
                str(state) == "OPEN"
                and int(lease.get("expiresAtMs", 0)) > now_ms
            ):
                return "active transfer lease"
        return ""

    def claim_garbage_collection(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        gc_owner: str,
        now_ms: int,
        deadline_ms: int,
        format_version: str = "artifact-manifest-v2",
        digest_algorithm: str = "sha256",
    ) -> None:
        self._require_artifact_writes_enabled()
        operation_id = str(operation_id).strip()
        artifact_digest = str(artifact_digest).strip().lower()
        gc_owner = str(gc_owner).strip()
        generation = int(generation)
        now_ms = int(now_ms)
        deadline_ms = int(deadline_ms)
        format_version = str(format_version).strip()
        digest_algorithm = str(digest_algorithm).strip()
        if (
            not operation_id
            or not gc_owner
            or format_version != "artifact-manifest-v2"
            or digest_algorithm != "sha256"
            or generation <= 0
            or len(artifact_digest) != 64
            or deadline_ms <= now_ms
        ):
            raise LifecycleTransitionError("repo-gc-invalid-claim")
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                reason = self._gc_protection_reason(
                    connection,
                    artifact_digest,
                    generation,
                    now_ms,
                    format_version,
                    digest_algorithm,
                )
                if reason:
                    raise LifecycleTransitionError(
                        f"repo-gc-protected: {reason}"
                    )
                existing = connection.execute("""
                    SELECT operation_id, gc_owner, deadline_ms, state
                    FROM artifact_gc_claims
                    WHERE format_version=? AND digest_algorithm=?
                          AND artifact_digest=? AND generation=?
                """, (
                    format_version,
                    digest_algorithm,
                    artifact_digest,
                    generation,
                )).fetchone()
                if (
                    existing is not None
                    and str(existing[3]) in {"CLAIMED", "RECLAIMING"}
                    and int(existing[2]) > now_ms
                    and (
                        str(existing[0]) != operation_id
                        or str(existing[1]) != gc_owner
                    )
                ):
                    raise LifecycleTransitionError(
                        "repo-gc-ownership-conflict"
                    )
                connection.execute("""
                    INSERT INTO artifact_gc_claims
                      (format_version, digest_algorithm, artifact_digest,
                       generation, operation_id, gc_owner, claimed_at_ms,
                       deadline_ms, state)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'CLAIMED')
                    ON CONFLICT(artifact_digest, generation) DO UPDATE SET
                      format_version=excluded.format_version,
                      digest_algorithm=excluded.digest_algorithm,
                      operation_id=excluded.operation_id,
                      gc_owner=excluded.gc_owner,
                      claimed_at_ms=excluded.claimed_at_ms,
                      deadline_ms=excluded.deadline_ms,
                      state='CLAIMED'
                """, (
                    format_version,
                    digest_algorithm,
                    artifact_digest,
                    generation,
                    operation_id,
                    gc_owner,
                    now_ms,
                    deadline_ms,
                ))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise

    def reclaim_temporary_generation(
        self,
        identity: ArtifactStorageIdentity,
        *,
        operation_id: str,
        gc_owner: str,
        now_ms: int,
        crash_injector: Any | None = None,
    ) -> None:
        self._require_artifact_writes_enabled()
        operation_id = str(operation_id).strip()
        gc_owner = str(gc_owner).strip()
        now_ms = int(now_ms)
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                claim = connection.execute("""
                    SELECT operation_id, gc_owner, deadline_ms, state
                    FROM artifact_gc_claims
                    WHERE format_version=? AND digest_algorithm=?
                          AND artifact_digest=? AND generation=?
                """, (
                    identity.format_version,
                    identity.digest_algorithm,
                    identity.content_digest, identity.generation
                )).fetchone()
                if (
                    claim is None
                    or str(claim[0]) != operation_id
                    or str(claim[1]) != gc_owner
                    or int(claim[2]) <= now_ms
                    or str(claim[3]) != "CLAIMED"
                ):
                    raise LifecycleTransitionError(
                        "repo-gc-invalid-ownership"
                    )
                reason = self._gc_protection_reason(
                    connection,
                    identity.content_digest,
                    identity.generation,
                    now_ms,
                    identity.format_version,
                    identity.digest_algorithm,
                )
                if reason:
                    raise LifecycleTransitionError(
                        f"repo-gc-protected: {reason}"
                    )
                connection.execute("""
                    UPDATE artifact_gc_claims SET state='RECLAIMING'
                    WHERE format_version=? AND digest_algorithm=?
                          AND artifact_digest=? AND generation=?
                """, (
                    identity.format_version,
                    identity.digest_algorithm,
                    identity.content_digest,
                    identity.generation,
                ))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise
            self.artifact_payload_store.abort(identity)
            if crash_injector is not None:
                crash_injector("after-payload-reclaim")
            self._complete_gc_metadata(
                identity, operation_id=operation_id, now_ms=now_ms
            )

    def _complete_gc_metadata(
        self,
        identity: ArtifactStorageIdentity,
        *,
        operation_id: str,
        now_ms: int,
    ) -> None:
        with self.lock:
            connection = self._require_connection()
            connection.execute("BEGIN IMMEDIATE")
            try:
                current = connection.execute("""
                    SELECT to_state FROM artifact_lifecycle_journal
                    WHERE operation_id=? AND accepted=1
                    ORDER BY sequence DESC LIMIT 1
                """, (operation_id,)).fetchone()
                current_state = "ABSENT" if current is None else str(current[0])
                session = connection.execute("""
                    SELECT lease_json, state FROM artifact_transfer_sessions
                    WHERE operation_id=?
                """, (operation_id,)).fetchone()
                if (
                    current_state in {
                        "QUEUED", "RESERVED", "RECEIVING", "VERIFIED"
                    }
                    and session is not None
                ):
                    lease = json.loads(str(session[0]))
                    target = (
                        "EXPIRED"
                        if int(lease.get("expiresAtMs", 0)) <= int(now_ms)
                        else "FAILED"
                    )
                    self._insert_event(
                        connection,
                        event_id=f"{operation_id}:gc-{target.lower()}",
                        operation_id=operation_id,
                        artifact_digest=identity.content_digest,
                        generation=identity.generation,
                        from_state=current_state,
                        to_state=target,
                        event_time_ms=int(now_ms),
                        accepted=True,
                        detail={"owner": "garbage-collector"},
                        error="",
                    )
                connection.execute("""
                    UPDATE artifact_transfer_sessions
                    SET state='FAILED', preserves_progress=0,
                        verified_chunks=0, updated_at_ms=?
                    WHERE operation_id=? AND artifact_digest=? AND generation=?
                """, (
                    now_ms,
                    operation_id,
                    identity.content_digest,
                    identity.generation,
                ))
                connection.execute("""
                    UPDATE artifact_capacity_reservations
                    SET state='RELEASED', updated_at_ms=?
                    WHERE operation_id=?
                """, (now_ms, operation_id))
                connection.execute("""
                    UPDATE artifact_gc_claims SET state='RECLAIMED'
                    WHERE format_version=? AND digest_algorithm=?
                          AND artifact_digest=? AND generation=?
                """, (
                    identity.format_version,
                    identity.digest_algorithm,
                    identity.content_digest,
                    identity.generation,
                ))
                connection.commit()
            except BaseException:
                connection.rollback()
                raise

    def reconcile_gc_claims(self) -> tuple[str, ...]:
        self._require_artifact_writes_enabled()
        with self.lock:
            rows = self._require_connection().execute("""
                SELECT c.operation_id, c.format_version, c.digest_algorithm,
                       c.artifact_digest, c.generation, s.lease_json,
                       c.claimed_at_ms
                FROM artifact_gc_claims c
                JOIN artifact_transfer_sessions s
                  ON s.operation_id=c.operation_id
                WHERE c.state='RECLAIMING'
                ORDER BY c.operation_id
            """).fetchall()
        reconciled: list[str] = []
        for (
            operation_id,
            format_version,
            digest_algorithm,
            digest,
            generation,
            lease_json,
            claimed_at_ms,
        ) in rows:
            lease = json.loads(str(lease_json))
            artifact = lease["artifact"]
            identity = ArtifactStorageIdentity(
                content_digest=str(digest),
                size_bytes=int(artifact["sizeBytes"]),
                generation=int(generation),
                digest_algorithm=str(digest_algorithm),
                format_version=str(format_version),
            )
            self.artifact_payload_store.abort(identity)
            self._complete_gc_metadata(
                identity,
                operation_id=str(operation_id),
                now_ms=int(claimed_at_ms),
            )
            reconciled.append(str(operation_id))
        return tuple(reconciled)

    def collect_expired_temporaries(
        self, *, gc_owner: str, now_ms: int, claim_ttl_ms: int = 30000
    ) -> tuple[str, ...]:
        self._require_artifact_writes_enabled()
        now_ms = int(now_ms)
        with self.lock:
            rows = self._require_connection().execute("""
                SELECT operation_id, artifact_digest, generation, lease_json,
                       state
                FROM artifact_transfer_sessions
                WHERE state IN ('OPEN', 'EXPIRED', 'FAILED', 'CANCELLED')
                ORDER BY operation_id
            """).fetchall()
        reclaimed: list[str] = []
        for operation_id, digest, generation, lease_json, state in rows:
            lease = json.loads(str(lease_json))
            if (
                str(state) != "FAILED"
                and int(lease.get("expiresAtMs", 0)) > now_ms
            ):
                continue
            try:
                if str(state) == "OPEN":
                    with self.lock:
                        connection = self._require_connection()
                        connection.execute("""
                            UPDATE artifact_transfer_sessions
                            SET state='EXPIRED', updated_at_ms=?
                            WHERE operation_id=? AND state='OPEN'
                        """, (now_ms, str(operation_id)))
                        connection.commit()
                self.claim_garbage_collection(
                    operation_id=str(operation_id),
                    artifact_digest=str(digest),
                    generation=int(generation),
                    gc_owner=gc_owner,
                    now_ms=now_ms,
                    deadline_ms=now_ms + int(claim_ttl_ms),
                )
                artifact = lease["artifact"]
                self.reclaim_temporary_generation(
                    ArtifactStorageIdentity(
                        content_digest=str(digest),
                        size_bytes=int(artifact["sizeBytes"]),
                        generation=int(generation),
                        digest_algorithm=str(
                            artifact.get("digestAlgorithm", "sha256")
                        ),
                    ),
                    operation_id=str(operation_id),
                    gc_owner=gc_owner,
                    now_ms=now_ms,
                )
                reclaimed.append(str(operation_id))
            except LifecycleTransitionError:
                continue
        return tuple(reclaimed)

    def active_artifact(
        self,
        logical_name: str,
        policy_epoch: str,
        *,
        format_version: str = "artifact-manifest-v2",
        digest_algorithm: str = "sha256",
    ) -> dict[str, Any] | None:
        with self.lock:
            connection = self._require_connection()
            if not self._table_exists(connection, "artifact_active_catalog"):
                return None
            if self._catalog_has_format_identity:
                row = connection.execute("""
                    SELECT artifact_json FROM artifact_active_catalog
                    WHERE logical_name=? AND policy_epoch=?
                          AND format_version=? AND digest_algorithm=?
                """, (
                    str(logical_name),
                    str(policy_epoch),
                    str(format_version),
                    str(digest_algorithm),
                )).fetchone()
            else:
                row = connection.execute("""
                    SELECT artifact_json FROM artifact_active_catalog
                    WHERE logical_name=? AND policy_epoch=?
                """, (str(logical_name), str(policy_epoch))).fetchone()
        if row is None:
            return None
        artifact = json.loads(str(row[0]))
        if (
            str(artifact.get(
                "formatVersion", "artifact-manifest-v2"
            )) != str(format_version)
            or str(artifact.get("digestAlgorithm", "sha256"))
            != str(digest_algorithm)
        ):
            return None
        return artifact

    def authenticated_receipt(
        self, operation_id: str
    ) -> dict[str, Any] | None:
        with self.lock:
            connection = self._require_connection()
            if not self._table_exists(
                connection, "artifact_replica_receipts"
            ):
                return None
            row = connection.execute("""
                SELECT receipt_json, signer_key_id, authentication_algorithm,
                       signature_hex
                FROM artifact_replica_receipts WHERE operation_id=?
            """, (str(operation_id),)).fetchone()
        if row is None:
            return None
        return {
            "receipt": json.loads(str(row[0])),
            "signerKeyId": str(row[1]),
            "authenticationAlgorithm": str(row[2]),
            "signatureHex": str(row[3]),
        }

    def close(self) -> None:
        with self.lock:
            if self._closed:
                return
            self._closed = True
            connection, self._connection = self._connection, None
            try:
                if connection is not None:
                    connection.close()
            finally:
                lock_file, self._lock_file = self._lock_file, None
                if lock_file is not None:
                    try:
                        fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
                    finally:
                        lock_file.close()


__all__ = [
    "ARTIFACT_LIFECYCLE_STATES",
    "ARTIFACT_LIFECYCLE_TRANSITIONS",
    "ArtifactCapacityStatus",
    "ArtifactFinalizationRecord",
    "ArtifactStorageIdentity",
    "ArtifactTransferSessionRecord",
    "FilesystemCasPayloadStore",
    "LifecycleTransitionError",
    "MetadataStore",
    "PayloadStore",
    "PersistenceOwnershipError",
    "RepoLifecycleEvent",
    "SqliteRepositoryPersistence",
]
