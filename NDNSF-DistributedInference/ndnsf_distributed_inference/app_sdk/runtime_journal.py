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

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

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
        self._journal_size = self.path.stat().st_size
        self._records_cache = self._load_records(tolerate_torn_tail=True)
        self._journal_envelopes: dict[str, dict[str, Any]] = {}
        self._rebuild_envelope_index()

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

    def append(self, kind: str, payload: dict[str, Any]) -> dict[str, Any]:
        return self.append_many(((kind, payload),))[0]

    def append_many(
        self, entries: Iterable[tuple[str, dict[str, Any]]]
    ) -> tuple[dict[str, Any], ...]:
        """Append an ordered state transition batch with one durable flush."""
        entries = tuple(entries)
        if not entries:
            return ()
        with self._exclusive_lock():
            self._refresh_records_if_changed()
            return self._append_many_locked(entries)

    def _append_many_locked(
        self, entries: Iterable[tuple[str, dict[str, Any]]]
    ) -> tuple[dict[str, Any], ...]:
        """Append entries while the caller owns the journal lock."""
        envelopes = []
        wires = []
        for kind, payload in entries:
            record = {
                "schema": self.SCHEMA,
                "kind": kind,
                "timestampMs": int(time.time() * 1000),
                "payload": payload,
            }
            body = json.dumps(record, sort_keys=True, separators=(",", ":"))
            envelope = {
                **record,
                "checksum": hashlib.sha256(body.encode()).hexdigest(),
            }
            envelopes.append(envelope)
            wires.append((json.dumps(
                envelope, sort_keys=True, separators=(",", ":")) + "\n").encode())
        if not envelopes:
            return ()
        if len(envelopes) == 1:
            wire = wires[0]
        else:
            batch = {
                "schema": self.SCHEMA,
                "kind": "journal-transaction",
                "timestampMs": int(time.time() * 1000),
                "payload": {"records": envelopes},
            }
            batch_body = json.dumps(
                batch, sort_keys=True, separators=(",", ":"))
            batch_envelope = {
                **batch,
                "checksum": hashlib.sha256(batch_body.encode()).hexdigest(),
            }
            wire = (json.dumps(
                batch_envelope, sort_keys=True, separators=(",", ":")) +
                "\n").encode()
        with self.path.open("ab") as output:
            if self._usage_bytes + len(wire) > self.quota_bytes:
                raise RuntimeJournalQuotaError("journal quota exceeded")
            output.write(wire)
            output.flush()
            os.fsync(output.fileno())
            self._records_cache = (*self._records_cache, *envelopes)
            for envelope in envelopes:
                self._index_envelope_record(envelope)
            self._journal_size += len(wire)
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

    def _load_records(self, *, tolerate_torn_tail: bool = True) -> tuple[dict[str, Any], ...]:
        records = []
        lines = self.path.read_bytes().splitlines()
        for index, line in enumerate(lines):
            try: item = json.loads(line)
            except Exception:
                if tolerate_torn_tail and index == len(lines)-1: break
                raise ValueError("corrupt runtime journal")
            checksum = item.pop("checksum", "")
            body = json.dumps(item, sort_keys=True, separators=(",", ":"))
            if not hmac.compare_digest(checksum, hashlib.sha256(body.encode()).hexdigest()):
                raise ValueError("runtime journal checksum mismatch")
            if item.get("schema") != self.SCHEMA:
                raise RuntimeJournalVersionError(
                    "unsupported runtime journal schema")
            verified = {**item, "checksum": checksum}
            if item.get("kind") != "journal-transaction":
                records.append(verified)
                continue
            nested = item.get("payload", {}).get("records", ())
            if not isinstance(nested, list):
                raise ValueError("runtime journal transaction payload mismatch")
            for logical in nested:
                if not isinstance(logical, dict):
                    raise ValueError("runtime journal transaction record mismatch")
                logical = dict(logical)
                logical_checksum = logical.pop("checksum", "")
                logical_body = json.dumps(
                    logical, sort_keys=True, separators=(",", ":"))
                if not hmac.compare_digest(
                        logical_checksum,
                        hashlib.sha256(logical_body.encode()).hexdigest()):
                    raise ValueError("runtime journal transaction checksum mismatch")
                if logical.get("schema") != self.SCHEMA:
                    raise RuntimeJournalVersionError(
                        "unsupported runtime journal transaction schema")
                records.append({**logical, "checksum": logical_checksum})
        return tuple(records)

    def _refresh_records_if_changed(self, *, tolerate_torn_tail: bool = True) -> None:
        observed_size = self.path.stat().st_size
        if observed_size == self._journal_size:
            return
        previous_size = self._journal_size
        self._records_cache = self._load_records(
            tolerate_torn_tail=tolerate_torn_tail)
        self._rebuild_envelope_index()
        self._journal_size = observed_size
        self._usage_bytes += observed_size - previous_size

    def _index_envelope_record(self, record: dict[str, Any]) -> None:
        payload = record.get("payload", {})
        request_id = str(payload.get("request_id", ""))
        if record.get("kind") == "protected-envelope" and request_id:
            self._journal_envelopes[request_id] = payload
        elif record.get("kind") == "protected-envelope-tombstone" and request_id:
            self._journal_envelopes.pop(request_id, None)

    def _rebuild_envelope_index(self) -> None:
        self._journal_envelopes = {}
        for record in self._records_cache:
            self._index_envelope_record(record)

    def prepare_envelope(
        self, request_id: str, payload: bytes, *, expires_at_ms: int,
    ) -> PreparedEnvelope:
        """Encrypt without persistence so metadata can bind the exact digest."""
        if not request_id or "/" in request_id or request_id in {".", ".."}:
            raise ValueError("invalid request spool identity")
        active_key = self._required_key_ring().active
        nonce = secrets.token_bytes(12)
        metadata = {
            "schema": "ndnsf-di-protected-request-v3",
            "requestId": request_id,
            "expiresAtMs": int(expires_at_ms),
            "keyId": active_key.key_id,
        }
        aad = json.dumps(
            metadata, sort_keys=True, separators=(",", ":")).encode()
        ciphertext = AESGCM(active_key.key_bytes).encrypt(
            nonce, bytes(payload), aad)
        body = {
            **metadata,
            "nonce": base64.b64encode(nonce).decode(),
            "ciphertext": base64.b64encode(ciphertext).decode(),
        }
        encoded = json.dumps(body, sort_keys=True).encode("utf-8")
        return PreparedEnvelope(
            request_id=request_id,
            expires_at_ms=int(expires_at_ms),
            encoded=encoded,
            wire_digest="sha256:" + hashlib.sha256(encoded).hexdigest(),
        )

    def commit_prepared_envelope(
        self,
        prepared: PreparedEnvelope,
        entries: Iterable[tuple[str, dict[str, Any]]],
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
        ))
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
        try:
            body = json.loads(self._encoded_envelope(request_id))
        except KeyError:
            raise
        except Exception as exc:
            raise ValueError("request envelope integrity failure") from exc
        if int(body["expiresAtMs"]) <= int(at_ms or time.time()*1000):
            raise ValueError("request envelope expired")
        key_ring = self._required_key_ring()
        if body.get("schema") == "ndnsf-di-protected-request-v1":
            mac = body.pop("mac", "")
            wire = json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
            for item in key_ring.keys:
                if hmac.compare_digest(
                        mac,
                        hmac.new(item.key_bytes, wire, hashlib.sha256).hexdigest()):
                    return base64.b64decode(body["payload"])
            raise ValueError("request envelope integrity failure")
        if body.get("schema") not in {
                "ndnsf-di-protected-request-v2",
                "ndnsf-di-protected-request-v3"}:
            raise ValueError("request envelope schema mismatch")
        metadata = {
            "schema": body["schema"],
            "requestId": body["requestId"],
            "expiresAtMs": int(body["expiresAtMs"]),
        }
        if body["schema"] == "ndnsf-di-protected-request-v3":
            metadata["keyId"] = str(body["keyId"])
            candidate_keys = (key_ring.find(str(body["keyId"])),)
        else:
            candidate_keys = key_ring.keys
        aad = json.dumps(metadata, sort_keys=True, separators=(",", ":")).encode()
        for item in candidate_keys:
            try:
                return AESGCM(item.key_bytes).decrypt(
                    base64.b64decode(body["nonce"]),
                    base64.b64decode(body["ciphertext"]),
                    aad,
                )
            except Exception:
                continue
        raise ValueError("request envelope integrity failure")

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
                self._rebuild_envelope_index()
                self._journal_size = len(wire)
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
