"""Owner-only append journal and integrity-protected request spool."""

from __future__ import annotations

import base64
import fcntl
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Iterable, Protocol


from ..core.contracts import (
    LifecycleEventV1,
    TERMINAL_LIFECYCLE_EVENT_TYPES,
)


_ENVELOPE_SYNC_POOL = ThreadPoolExecutor(
    max_workers=2, thread_name_prefix="ndnsf-di-journal-sync")


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(
        str(path), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


class RuntimeJournalError(RuntimeError):
    """Base class for fail-closed APP persistence errors."""


class RuntimeJournalVersionError(RuntimeJournalError, ValueError):
    pass


class RuntimeJournalLockError(RuntimeJournalError):
    pass


class RuntimeJournalQuotaError(RuntimeJournalError, OSError):
    pass


class RuntimeJournalKeyError(RuntimeJournalError, ValueError):
    pass


class RuntimeJournalUnsafeRootError(RuntimeJournalError, ValueError):
    pass


@dataclass(frozen=True)
class RequestEnvelopeKey:
    """One owner-supplied request-envelope key and its non-secret identity."""

    key_id: str
    key_bytes: bytes

    def __post_init__(self) -> None:
        if not self.key_id or "/" in self.key_id or "\\" in self.key_id:
            raise RuntimeJournalKeyError("invalid request-envelope key identity")
        if len(self.key_bytes) != 32:
            raise RuntimeJournalKeyError(
                "request-envelope key must contain exactly 32 bytes")


@dataclass(frozen=True)
class RequestEnvelopeKeyRing:
    """Active encryption key plus bounded previous decryption keys."""

    active: RequestEnvelopeKey
    previous: tuple[RequestEnvelopeKey, ...] = ()

    def __post_init__(self) -> None:
        identities = [item.key_id for item in self.keys]
        if len(identities) != len(set(identities)):
            raise RuntimeJournalKeyError(
                "request-envelope key identities must be unique")

    @property
    def keys(self) -> tuple[RequestEnvelopeKey, ...]:
        return (self.active, *self.previous)

    def find(self, key_id: str) -> RequestEnvelopeKey:
        for item in self.keys:
            if hmac.compare_digest(item.key_id, key_id):
                return item
        raise RuntimeJournalKeyError(
            f"request-envelope key is unavailable: {key_id}")


@dataclass(frozen=True)
class PreparedEnvelope:
    """Encrypted envelope bytes awaiting one grouped authoritative commit."""

    request_id: str
    expires_at_ms: int
    encoded: bytes
    wire_digest: str

    def __post_init__(self) -> None:
        if (not self.request_id or "/" in self.request_id or
                self.request_id in {".", ".."} or
                self.expires_at_ms <= 0 or not self.encoded or
                not self.wire_digest.startswith("sha256:")):
            raise ValueError("invalid prepared request envelope")


class RequestEnvelopeKeyProvider(Protocol):
    """Owner-controlled key source; RuntimeJournal never persists key bytes."""

    def key_ring(self, identity: str) -> RequestEnvelopeKeyRing:
        ...


class StaticRequestEnvelopeKeyProvider:
    """Explicit injected provider, useful for secret managers and tests."""

    def __init__(self, active: RequestEnvelopeKey, *,
                 previous: Iterable[RequestEnvelopeKey] = ()) -> None:
        self._ring = RequestEnvelopeKeyRing(active, tuple(previous))

    def key_ring(self, identity: str) -> RequestEnvelopeKeyRing:
        if not identity:
            raise RuntimeJournalKeyError(
                "request-envelope key owner identity is required")
        return self._ring


class FileRequestEnvelopeKeyProvider(StaticRequestEnvelopeKeyProvider):
    """Load owner-managed raw 32-byte keys from protected external files."""

    def __init__(self, active_path: str | Path, *,
                 previous_paths: Iterable[str | Path] = ()) -> None:
        active = self._load_key(Path(active_path))
        previous = tuple(self._load_key(Path(path))
                         for path in previous_paths)
        super().__init__(active, previous=previous)

    @staticmethod
    def _load_key(path: Path) -> RequestEnvelopeKey:
        if path.is_symlink():
            raise RuntimeJournalKeyError(
                "request-envelope key file cannot be a symlink")
        try:
            stat = path.stat()
            if stat.st_uid != os.geteuid():
                raise RuntimeJournalKeyError(
                    "request-envelope key file must be owner-controlled")
            if stat.st_mode & 0o077:
                raise RuntimeJournalKeyError(
                    "request-envelope key file must be owner-only")
            key_bytes = path.read_bytes()
        except RuntimeJournalKeyError:
            raise
        except OSError as exc:
            raise RuntimeJournalKeyError(
                "request-envelope key file is unavailable") from exc
        key_id = "sha256:" + hashlib.sha256(key_bytes).hexdigest()
        return RequestEnvelopeKey(key_id, key_bytes)


def _is_volatile_state_root(path: Path) -> bool:
    resolved = path.expanduser().resolve(strict=False)
    for candidate in (Path("/tmp"), Path("/run"), Path("/dev/shm")):
        try:
            resolved.relative_to(candidate)
            return True
        except ValueError:
            continue
    return False


class RuntimeJournal:
    SCHEMA = "ndnsf-di-app-runtime-journal-v1"
    LIFECYCLE_KIND = "spec168-lifecycle-event-v1"

    def __init__(
        self,
        state_root: str | Path,
        identity: str,
        *,
        quota_bytes: int = 64 << 20,
        envelope_key_provider: RequestEnvelopeKeyProvider | None = None,
        test_only_allow_ephemeral_state_root: bool = False,
    ):
        if not identity or identity in {".", ".."} or "/" in identity or "\\" in identity:
            raise ValueError("invalid journal identity namespace")
        root = Path(state_root)
        self.identity = identity
        self._append_poisoned = False
        if (_is_volatile_state_root(root) and
                not test_only_allow_ephemeral_state_root):
            raise RuntimeJournalUnsafeRootError(
                "volatile runtime journal state root requires the named "
                "test-only override")
        try:
            root_created = not root.exists()
            if root.is_symlink():
                raise RuntimeJournalUnsafeRootError(
                    "journal root cannot be a symlink")
            root.mkdir(parents=True, exist_ok=True, mode=0o700)
            if root_created:
                _fsync_directory(root.parent)
            if root.stat().st_uid != os.geteuid():
                raise RuntimeJournalUnsafeRootError(
                    "journal root must be owned by the current identity")
            self.root = root / identity
            identity_created = not self.root.exists()
            if self.root.exists() and self.root.is_symlink():
                raise RuntimeJournalUnsafeRootError(
                    "journal identity cannot be a symlink")
            self.root.mkdir(mode=0o700, exist_ok=True)
            os.chmod(self.root, 0o700)
            self.path = self.root / "journal.jsonl"
            self.lock_path = self.root / "journal.lock"
            self.spool = self.root / "requests"
            spool_created = not self.spool.exists()
            self.spool.mkdir(mode=0o700, exist_ok=True)
            self.quota_bytes = int(quota_bytes)
            self._key_ring = (
                envelope_key_provider.key_ring(identity)
                if envelope_key_provider is not None else None
            )
            journal_created = not self.path.exists()
            lock_created = not self.lock_path.exists()
            self.path.touch(mode=0o600, exist_ok=True)
            self.lock_path.touch(mode=0o600, exist_ok=True)
            os.chmod(self.path, 0o600)
            os.chmod(self.lock_path, 0o600)
            if any((spool_created, journal_created, lock_created)):
                _fsync_directory(self.root)
            if identity_created:
                _fsync_directory(root)
        except RuntimeJournalError:
            raise
        except OSError as exc:
            raise RuntimeJournalUnsafeRootError(
                "runtime journal state root is not safely writable") from exc
        self._usage_bytes = self._scan_usage_bytes()
        (self._records_cache, self._journal_size,
         self._journal_valid_size, self._journal_needs_newline,
         self._journal_identity) = self._load_records()
        self._journal_envelopes: dict[str, dict[str, Any]] = {}
        self._rebuild_indexes()

    @classmethod
    def for_test(
        cls,
        state_root: str | Path,
        identity: str,
        *,
        quota_bytes: int = 64 << 20,
    ) -> "RuntimeJournal":
        """Create an explicitly ephemeral MiniNDN/unit-test journal.

        The deterministic test key is never written and this constructor must
        not be used by production APP entry points.
        """
        key = hashlib.sha256(
            f"ndnsf-di-test-only-envelope:{identity}".encode("utf-8")
        ).digest()
        return cls(
            state_root,
            identity,
            quota_bytes=quota_bytes,
            envelope_key_provider=StaticRequestEnvelopeKeyProvider(
                RequestEnvelopeKey("test-only-v1", key)),
            test_only_allow_ephemeral_state_root=True,
        )

    def _required_key_ring(self) -> RequestEnvelopeKeyRing:
        if self._key_ring is None:
            raise RuntimeJournalKeyError(
                "request-envelope key provider is required")
        return self._key_ring

    @property
    def has_envelope_key(self) -> bool:
        return self._key_ring is not None

    def authentication_key_ring(self, purpose: str) -> tuple[bytes, ...]:
        """Derive domain-separated authentication keys without reusing AES keys.

        The active request-envelope key produces the first key and retained
        previous keys follow in verification order.  Callers can therefore
        authenticate new local state with the active owner key while still
        verifying state written before a bounded key rotation.  Raw envelope
        keys are never returned.
        """
        if (not purpose or len(purpose) > 128
                or any(ord(char) < 0x21 or ord(char) > 0x7e
                       for char in purpose)):
            raise RuntimeJournalKeyError(
                "authentication key purpose must be visible ASCII")
        context = (
            b"ndnsf-di-runtime-authentication-subkey-v1\x00"
            + purpose.encode("ascii")
            + b"\x00"
            + self.identity.encode("utf-8")
        )
        return tuple(
            hmac.new(item.key_bytes, context, hashlib.sha256).digest()
            for item in self._required_key_ring().keys
        )

    def append(self, kind: str, payload: dict[str, Any]) -> dict[str, Any]:
        return self.append_many(((kind, payload),))[0]

    def append_many(
        self, entries: Iterable[tuple[str, dict[str, Any]]], *,
        _request_fence: tuple[str, int, tuple[int, str] | None] | None = None,
    ) -> tuple[dict[str, Any], ...]:
        """Append an ordered state transition batch with one durable flush."""
        entries = tuple(entries)
        if not entries and _request_fence is None:
            return ()
        with self._exclusive_lock():
            self._refresh_records_if_changed()
            if _request_fence is not None:
                request_id, sequence, identity = _request_fence
                committed_sequence = self._request_event_sequences.get(request_id, 0)
                committed_identity = self._request_identities.get(request_id)
                if identity is None and (committed_sequence or committed_identity is not None):
                    raise ValueError(f"request_id already exists: {request_id}")
                if (sequence, identity) != (committed_sequence, committed_identity):
                    raise RuntimeJournalError("REQUEST_STATE_CHANGED: reopen the journal")
            return self._append_many_locked(entries)

    def _append_many_locked(
        self, entries: Iterable[tuple[str, dict[str, Any]]]
    ) -> tuple[dict[str, Any], ...]:
        """Append entries while the caller owns the journal lock."""
        from ndnsf import _ndnsf

        if self._append_poisoned:
            raise OSError("runtime journal unavailable after failed rollback")
        envelopes = [
            {
                "schema": self.SCHEMA,
                "kind": kind,
                "timestampMs": int(time.time() * 1000),
                "payload": payload,
            }
            for kind, payload in entries
        ]
        if not envelopes:
            return ()
        wire, checksums = _ndnsf._frame_di_journal_records(
            [(json.dumps(record["kind"]), json.dumps(record["payload"], sort_keys=True,
                                        separators=(",", ":")), record["timestampMs"])
             for record in envelopes],
            int(time.time() * 1000),
        )
        for record, checksum in zip(envelopes, checksums):
            record["checksum"] = checksum
        tail_size = self._journal_size - self._journal_valid_size
        if self._journal_needs_newline:
            wire = b"\n" + wire
        if self._usage_bytes - tail_size + len(wire) > self.quota_bytes:
            raise RuntimeJournalQuotaError("journal quota exceeded")
        fd = os.open(self.path, os.O_RDWR | getattr(os, "O_NOFOLLOW", 0))
        try:
            if tail_size:
                if self._file_identity(os.fstat(fd)) != self._journal_identity:
                    raise OSError("runtime journal changed before tail repair")
                _ndnsf._truncate_di_journal_tail(fd, self._journal_size, self._journal_valid_size)
                self._journal_size = self._journal_valid_size
                self._usage_bytes -= tail_size
            _ndnsf._append_di_journal_wire(fd, self._journal_size, wire)
            self._journal_identity = self._file_identity(os.fstat(fd))
        except _ndnsf._JournalRollbackError:
            self._append_poisoned = True
            raise
        finally:
            os.close(fd)
        self._records_cache = (*self._records_cache, *envelopes)
        for envelope in envelopes:
            self._index_record(envelope)
        self._journal_size += len(wire)
        self._journal_valid_size = self._journal_size
        self._journal_needs_newline = False
        self._usage_bytes += len(wire)
        return tuple(envelopes)

    def lifecycle_records(self) -> tuple[dict[str, Any], ...]:
        """Return immutable Spec 168 acceptance decisions in journal order."""
        return tuple(
            dict(record["payload"])
            for record in self.records()
            if record.get("kind") == self.LIFECYCLE_KIND
        )

    def append_lifecycle_event(self, event: LifecycleEventV1) -> dict[str, Any]:
        """Atomically validate and retain one canonical lifecycle event.

        Invalid/stale observations remain visible as rejected evidence but do
        not become inputs to later acceptance decisions.
        """
        if not isinstance(event, LifecycleEventV1):
            raise TypeError("event must be LifecycleEventV1")
        with self._exclusive_lock():
            self._refresh_records_if_changed()
            existing = tuple(
                record["payload"] for record in self._records_cache
                if record.get("kind") == self.LIFECYCLE_KIND
            )
            rejection_code = self._lifecycle_rejection(existing, event)
            decision = {
                "event": event.to_dict(),
                "accepted": rejection_code is None,
                "rejection_code": rejection_code,
            }
            self._append_many_locked(((self.LIFECYCLE_KIND, decision),))
            return decision

    @staticmethod
    def _lifecycle_rejection(
        records: tuple[dict[str, Any], ...], event: LifecycleEventV1,
    ) -> str | None:
        accepted = []
        for record in records:
            if not record.get("accepted"):
                continue
            try:
                accepted.append(LifecycleEventV1.from_dict(record["event"]))
            except (KeyError, TypeError, ValueError):
                return "JOURNAL_LIFECYCLE_CORRUPT"

        if any(item.event_id == event.event_id for item in accepted):
            return "DUPLICATE_EVENT_ID"
        if not event.authenticated:
            return "UNAUTHENTICATED_EVENT"

        same_request = [item for item in accepted
                        if item.request_id == event.request_id]
        if same_request:
            max_attempt = max(item.attempt_epoch for item in same_request)
            if event.attempt_epoch < max_attempt:
                return "STALE_ATTEMPT"
            if (event.attempt_epoch > max_attempt
                    and event.event_type != "REQUEST_CREATED"):
                return "ATTEMPT_NOT_OPENED"
        same_attempt = [
            item for item in same_request
            if item.attempt_epoch == event.attempt_epoch
        ]
        if any(item.experiment_id != event.experiment_id
               for item in same_attempt):
            return "EXPERIMENT_BINDING_MISMATCH"

        terminals = [item for item in same_attempt
                     if item.event_type in TERMINAL_LIFECYCLE_EVENT_TYPES]
        if terminals:
            if event.event_type in TERMINAL_LIFECYCLE_EVENT_TYPES:
                return "TERMINAL_ALREADY_SET"
            return "REQUEST_ALREADY_TERMINAL"

        plans = [item.plan_digest for item in same_attempt
                 if item.event_type == "PLAN_COMMITTED"]
        committed_plan = plans[0] if plans else None
        if committed_plan is not None and event.plan_digest != committed_plan:
            return "PLAN_BINDING_MISMATCH"

        assignments = {
            item.role: (item.provider, item.provider_boot_epoch)
            for item in same_attempt if item.event_type == "ROLE_ASSIGNED"
        }
        role_scoped = {
            "ARTIFACT_FETCHED", "VERIFIED_DISK", "HOST_RESIDENT",
            "GPU_RESIDENT", "LOCAL_READY", "DEPENDENCY_INPUT_ACCEPTED",
            "STAGE_EXECUTING", "STAGE_OUTPUT_PUBLISHED", "STAGE_COMPLETED",
        }
        if event.event_type in role_scoped and event.role not in assignments:
            return "ROLE_NOT_ASSIGNED"
        if event.role in assignments:
            expected = assignments[event.role]
            if (event.provider, event.provider_boot_epoch) != expected:
                return "PROVIDER_ROLE_BINDING_MISMATCH"

        if event.operation_id is not None:
            progress = [
                item for item in same_attempt
                if item.operation_id == event.operation_id
            ]
            if progress:
                max_epoch = max(item.epoch for item in progress)
                if event.epoch < max_epoch:
                    return "STALE_OPERATION_EPOCH"
                if event.epoch == max_epoch and event.sequence <= max(
                        item.sequence for item in progress
                        if item.epoch == max_epoch):
                    return "NON_MONOTONIC_PROGRESS"
        return None

    def records(self, *, tolerate_torn_tail: bool = True) -> tuple[dict[str, Any], ...]:
        self._refresh_records_if_changed(tolerate_torn_tail=tolerate_torn_tail)
        return self._records_cache

    @staticmethod
    def _file_identity(stat: os.stat_result) -> tuple[int, int, int, int, int]:
        return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)

    def _load_records(
        self, *, tolerate_torn_tail: bool = True,
    ) -> tuple[tuple[dict[str, Any], ...], int, int, bool, tuple[int, int, int, int, int]]:
        from ndnsf import _ndnsf

        def verify_record(record, scope):
            unsigned = dict(record)
            checksum = unsigned.pop("checksum", "")
            fingerprint = checksum if isinstance(checksum, str) and checksum.isascii() else ""
            reason = _ndnsf._validate_di_journal_record(
                json.dumps(unsigned, sort_keys=True, separators=(",", ":")),
                fingerprint,
                json.dumps(unsigned.get("schema")),
            )
            if reason == "checksum":
                raise ValueError(f"{scope} checksum mismatch")
            if reason == "schema":
                raise RuntimeJournalVersionError(f"unsupported {scope} schema")

        records = []
        with self.path.open("rb") as source:
            identity = self._file_identity(os.fstat(source.fileno()))
            wire = source.read()
        lines = wire.splitlines(keepends=True)
        valid_size = 0
        for index, line in enumerate(lines):
            try:
                item = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                if tolerate_torn_tail and index == len(lines) - 1 and not line.endswith(b"\n"):
                    break
                raise ValueError("corrupt runtime journal") from exc
            if not isinstance(item, dict):
                raise ValueError("runtime journal record must be an object")
            valid_size += len(line)
            verify_record(item, "runtime journal")
            if item.get("kind") != "journal-transaction":
                records.append(item)
                continue
            payload = item.get("payload")
            nested = payload.get("records") if isinstance(payload, dict) else None
            if not isinstance(nested, list):
                raise ValueError("runtime journal transaction payload mismatch")
            for logical in nested:
                if not isinstance(logical, dict):
                    raise ValueError("runtime journal transaction record mismatch")
                verify_record(logical, "runtime journal transaction")
                if logical.get("kind") == "journal-transaction":
                    raise ValueError("nested runtime journal transaction")
            records.extend(nested)
        needs_newline = valid_size > 0 and wire[valid_size - 1] != ord("\n")
        return tuple(records), len(wire), valid_size, needs_newline, identity

    def _refresh_records_if_changed(self, *, tolerate_torn_tail: bool = True) -> None:
        if self._file_identity(self.path.stat()) == self._journal_identity:
            if not tolerate_torn_tail and self._journal_valid_size != self._journal_size:
                raise ValueError("corrupt runtime journal")
            return
        previous_size = self._journal_size
        (self._records_cache, self._journal_size,
         self._journal_valid_size, self._journal_needs_newline,
         self._journal_identity) = self._load_records(
             tolerate_torn_tail=tolerate_torn_tail)
        self._rebuild_indexes()
        self._usage_bytes += self._journal_size - previous_size

    def _index_record(self, record: dict[str, Any]) -> None:
        payload = record.get("payload", {})
        request_id = str(payload.get("request_id", ""))
        kind = record["kind"]
        if kind == "protected-envelope" and request_id:
            self._journal_envelopes[request_id] = payload
        elif kind == "protected-envelope-tombstone" and request_id:
            self._journal_envelopes.pop(request_id, None)
        elif request_id and kind in ("request-handle", "request-handle-update"):
            self._request_identities[request_id] = (
                payload.get("attempt_epoch", 1), payload.get("envelope_digest"))
        elif kind == "request-event" and "requestId" in payload:
            self._request_event_sequences[str(payload["requestId"])] = payload.get("sequence")

    def _rebuild_indexes(self) -> None:
        self._journal_envelopes = {}
        self._request_identities = {}
        self._request_event_sequences = {}
        for record in self._records_cache:
            self._index_record(record)

    def prepare_envelope(
        self, request_id: str, payload: bytes, *, expires_at_ms: int,
    ) -> PreparedEnvelope:
        """Encrypt without persistence so metadata can bind the exact digest."""
        from ndnsf import _ndnsf

        if not request_id or "/" in request_id or request_id in {".", ".."}:
            raise ValueError("invalid request spool identity")
        expiry = int(expires_at_ms)
        if expiry <= 0:
            raise ValueError("invalid prepared request envelope")
        active_key = self._required_key_ring().active
        encoded = _ndnsf._seal_di_journal_envelope(
            bytes(payload), request_id, expiry, active_key.key_id,
            bytes(active_key.key_bytes))
        return PreparedEnvelope(
            request_id=request_id,
            expires_at_ms=expiry,
            encoded=encoded,
            wire_digest="sha256:" + hashlib.sha256(encoded).hexdigest(),
        )

    def commit_prepared_envelope(
        self,
        prepared: PreparedEnvelope,
        entries: Iterable[tuple[str, dict[str, Any]]],
        *,
        _request_fence: tuple[str, int, tuple[int, str] | None] | None = None,
    ) -> tuple[dict[str, Any], ...]:
        """Atomically commit encrypted bytes and their logical references.

        The encrypted envelope is authoritative inside the same checksummed
        journal transaction as its handle/rendezvous records.  The spool copy
        is only a rebuildable compatibility mirror and therefore adds no
        durability barrier.
        """
        if ("sha256:" + hashlib.sha256(prepared.encoded).hexdigest() !=
                prepared.wire_digest):
            raise ValueError("prepared request envelope digest mismatch")
        records = self.append_many((
            ("protected-envelope", {
                "request_id": prepared.request_id,
                "expires_at_ms": prepared.expires_at_ms,
                "wire_digest": prepared.wire_digest,
                "encoded": base64.b64encode(prepared.encoded).decode("ascii"),
            }),
            *tuple(entries),
        ), _request_fence=_request_fence)
        return records

    def _materialize_envelope_mirror(self, prepared: PreparedEnvelope) -> None:
        target = self.spool / f"{prepared.request_id}.json"
        temp = self.spool / (
            f".{prepared.request_id}.{os.getpid()}.{secrets.token_hex(8)}.tmp")
        previous_size = target.stat().st_size if target.exists() else 0
        if (self._usage_bytes - previous_size + len(prepared.encoded) >
                self.quota_bytes):
            return
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(str(temp), flags, 0o600)
            with os.fdopen(fd, "wb") as output:
                output.write(prepared.encoded)
            os.replace(temp, target)
            self._usage_bytes += len(prepared.encoded) - previous_size
        except OSError:
            temp.unlink(missing_ok=True)
            # The journal transaction is the authoritative durable copy.
            return

    def write_envelope(self, request_id: str, payload: bytes, *, expires_at_ms: int) -> str:
        prepared = self.prepare_envelope(
            request_id, payload, expires_at_ms=expires_at_ms)
        target = self.spool / f"{request_id}.json"
        temp = self.spool / (
            f".{request_id}.{os.getpid()}.{secrets.token_hex(8)}.tmp")
        encoded = prepared.encoded
        previous_size = target.stat().st_size if target.exists() else 0
        projected = self._usage_bytes - previous_size + len(encoded)
        if projected > self.quota_bytes:
            raise RuntimeJournalQuotaError("journal quota exceeded")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(str(temp), flags, 0o600)
        file_fd = None
        directory_fd = None
        try:
            with os.fdopen(fd, "wb") as output:
                output.write(encoded)
                output.flush()
                file_fd = os.dup(output.fileno())
            os.replace(temp, target)
            directory_fd = os.open(
                str(self.spool), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            pending = (
                _ENVELOPE_SYNC_POOL.submit(os.fsync, file_fd),
                _ENVELOPE_SYNC_POOL.submit(os.fsync, directory_fd),
            )
            for item in pending:
                item.result()
        except Exception:
            temp.unlink(missing_ok=True)
            raise
        finally:
            if file_fd is not None:
                os.close(file_fd)
            if directory_fd is not None:
                os.close(directory_fd)
        self._usage_bytes = projected
        return prepared.wire_digest

    def _encoded_envelope(self, request_id: str) -> bytes:
        target = self.spool / f"{request_id}.json"
        if target.is_symlink():
            raise ValueError("request envelope cannot be symlinked")
        if target.exists():
            return target.read_bytes()
        record = self._journal_envelopes.get(request_id)
        if record is None:
            raise KeyError("request envelope missing")
        try:
            encoded = base64.b64decode(str(record["encoded"]), validate=True)
        except Exception as exc:
            raise ValueError("protected journal envelope is malformed") from exc
        digest = "sha256:" + hashlib.sha256(encoded).hexdigest()
        if not hmac.compare_digest(digest, str(record.get("wire_digest", ""))):
            raise ValueError("protected journal envelope digest mismatch")
        return encoded

    def read_envelope(self, request_id: str, *, at_ms: int | None = None) -> bytes:
        from ndnsf import _ndnsf

        if not request_id or "/" in request_id or request_id in {".", ".."}:
            raise ValueError("invalid request spool identity")
        try:
            encoded = self._encoded_envelope(request_id)
        except KeyError:
            raise
        except Exception as exc:
            raise ValueError("request envelope integrity failure") from exc
        keys = {item.key_id: bytes(item.key_bytes) for item in self._required_key_ring().keys}
        try:
            return _ndnsf._read_di_journal_envelope(
                encoded, request_id, keys, max(0, int(at_ms or time.time() * 1000)))
        except _ndnsf._JournalEnvelopeKeyUnavailable as exc:
            raise RuntimeJournalKeyError(str(exc)) from exc

    def envelope_digest(self, request_id: str) -> str:
        encoded = self._encoded_envelope(request_id)
        return "sha256:" + hashlib.sha256(encoded).hexdigest()

    def cleanup(self, *, at_ms: int, protected: Iterable[str] = ()) -> tuple[str, ...]:
        keep = set(protected); removed = []
        for path in self.spool.glob("*.json"):
            if path.stem in keep: continue
            try:
                body = json.loads(path.read_text(encoding="utf-8"))
                if int(body.get("expiresAtMs", 0)) > at_ms: continue
            except Exception: pass
            size = path.stat().st_size if path.exists() else 0
            path.unlink(missing_ok=True)
            self._usage_bytes -= size
            removed.append(path.stem)
        return tuple(sorted(removed))

    def compact(self, *, at_ms: int, retention_ms: int) -> dict[str, int]:
        """Rewrite authoritative records and remove requests past retention.

        Compaction is mechanism-owned: unknown record kinds are preserved,
        deployment/operation snapshots retain their latest value, and request
        streams are removed only after their declared envelope expiry plus the
        caller-supplied retention window.
        """
        if retention_ms < 0:
            raise ValueError("retention_ms must be non-negative")
        with self._exclusive_lock():
            self._refresh_records_if_changed(tolerate_torn_tail=False)
            records = list(self._records_cache)
            expired_requests = {
                str(record["payload"].get("request_id", ""))
                for record in records
                if record["kind"] == "request-handle" and
                int(record["payload"].get("expires_at_ms", 0)) +
                int(retention_ms) <= int(at_ms)
            }
            expired_requests.discard("")
            latest_deployment: dict[str, int] = {}
            latest_operation: dict[str, int] = {}
            for index, record in enumerate(records):
                payload = record["payload"]
                if record["kind"] == "deployment-state":
                    latest_deployment[str(payload.get("deploymentId", ""))] = index
                elif record["kind"] == "deployment-operation":
                    operation_id = str(payload.get("operation_id", ""))
                    latest_operation[operation_id] = index

            retained = []
            for index, record in enumerate(records):
                payload = record["payload"]
                request_id = str(
                    payload.get("requestId", payload.get("request_id", "")))
                if request_id in expired_requests:
                    continue
                if (record["kind"] == "deployment-state" and
                        latest_deployment.get(str(
                            payload.get("deploymentId", ""))) != index):
                    continue
                if (record["kind"] == "deployment-operation" and
                        latest_operation.get(str(
                            payload.get("operation_id", ""))) != index):
                    continue
                if (record["kind"] == "protected-envelope" and
                        int(payload.get("expires_at_ms", 0)) +
                        int(retention_ms) <= int(at_ms)):
                    continue
                retained.append(record)

            if len(retained) == len(records):
                return {"retainedRecords": len(retained), "removedRecords": 0}
            wire = b"".join(
                (json.dumps(record, sort_keys=True, separators=(",", ":")) +
                 "\n").encode()
                for record in retained
            )
            temp = self.root / (
                f".journal.{os.getpid()}.{secrets.token_hex(8)}.tmp")
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            flags |= getattr(os, "O_NOFOLLOW", 0)
            fd = os.open(str(temp), flags, 0o600)
            try:
                with os.fdopen(fd, "wb") as output:
                    output.write(wire)
                    output.flush()
                    os.fsync(output.fileno())
                previous_size = self._journal_size
                os.replace(temp, self.path)
                directory_fd = os.open(
                    str(self.root),
                    os.O_RDONLY | getattr(os, "O_DIRECTORY", 0),
                )
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
                self._records_cache = tuple(retained)
                self._rebuild_indexes()
                self._journal_size = len(wire)
                self._journal_valid_size = self._journal_size
                self._journal_needs_newline = False
                self._journal_identity = self._file_identity(self.path.stat())
                self._usage_bytes += len(wire) - previous_size
            except Exception:
                temp.unlink(missing_ok=True)
                raise
            return {
                "retainedRecords": len(retained),
                "removedRecords": len(records) - len(retained),
            }

    def usage_bytes(self) -> int:
        return self._usage_bytes

    def _scan_usage_bytes(self) -> int:
        return sum(
            path.stat().st_size for path in self.root.rglob("*")
            if path.is_file())

    def _exclusive_lock(self):
        return _ExclusiveJournalLock(self.lock_path)


class _ExclusiveJournalLock:
    def __init__(self, path: Path):
        self.path = path
        self.stream = None

    def __enter__(self):
        self.stream = self.path.open("rb")
        try:
            fcntl.flock(
                self.stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except (BlockingIOError, OSError) as exc:
            self.stream.close()
            self.stream = None
            raise RuntimeJournalLockError(
                "runtime journal writer lock is busy") from exc
        return self

    def __exit__(self, exc_type, exc, traceback):
        if self.stream is not None:
            fcntl.flock(self.stream, fcntl.LOCK_UN)
            self.stream.close()
        return False


__all__ = [
    "FileRequestEnvelopeKeyProvider",
    "PreparedEnvelope",
    "RequestEnvelopeKey",
    "RequestEnvelopeKeyProvider",
    "RequestEnvelopeKeyRing",
    "RuntimeJournal",
    "RuntimeJournalError",
    "RuntimeJournalKeyError",
    "RuntimeJournalVersionError",
    "RuntimeJournalLockError",
    "RuntimeJournalQuotaError",
    "RuntimeJournalUnsafeRootError",
    "StaticRequestEnvelopeKeyProvider",
]
