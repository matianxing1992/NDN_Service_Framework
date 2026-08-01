"""Crash-safe single-replica filesystem backend for the public artifact API.

This backend is intended for local applications, tests, and development.  It
uses the same immutable ArtifactReference and public result/error contracts as
network backends, but it does not claim NDNSF collaboration or remote
durability.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import threading
import time

from .artifact_api import (
    ArtifactApiError,
    ArtifactCancellationToken,
    ArtifactControlMode,
    ArtifactDescriptor,
    ArtifactErrorCode,
    ArtifactFetchResult,
    ArtifactProgress,
    ArtifactPublishResult,
    ArtifactReplicaResult,
    ArtifactSessionStatus,
)


_COPY_BYTES = 1024 * 1024


def _atomic_json(path: Path, value: dict) -> None:
    temporary = Path(str(path) + ".tmp")
    temporary.write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(_COPY_BYTES)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


class _LocalPublishDriver:
    def __init__(self, backend, descriptor, operation_id, emit):
        self.backend = backend
        self.descriptor = descriptor
        self.operation_id = operation_id
        self.emit = emit
        self.state = "OPEN"
        self.sequence = 0
        self.received = 0
        self.started = time.monotonic()
        self.resumed = False
        self.deduplicated = self.backend._object_path(
            descriptor.reference.content_digest
        ).is_file()

    @property
    def reference(self):
        return self.descriptor.reference

    @property
    def staging(self) -> Path:
        return self.backend.sessions / f"{self.operation_id}.upload.part"

    def _emit(self, phase: str, committed: bool = False) -> None:
        self.sequence += 1
        verified = self.received
        self.emit(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.reference,
            phase=phase,
            received_bytes=self.received,
            verified_bytes=verified,
            committed_bytes=verified if committed else 0,
            total_bytes=int(self.reference.size_bytes),
            selected_replicas=1,
            committed_replicas=1 if committed else 0,
            retransmitted_bytes=0,
            sequence=self.sequence,
            timestamp_ms=time.time_ns() // 1_000_000,
        ))

    def transfer(
        self, source: Path, cancellation: ArtifactCancellationToken
    ) -> None:
        if self.deduplicated:
            self.received = int(self.reference.size_bytes)
            self.state = "VERIFIED"
            self._emit("deduplicated")
            return
        self.staging.parent.mkdir(parents=True, exist_ok=True)
        offset = self.staging.stat().st_size if (
            self.descriptor.resume and self.staging.is_file()
        ) else 0
        if offset > int(self.reference.size_bytes):
            self.staging.unlink(missing_ok=True)
            offset = 0
        if offset:
            with source.open("rb") as expected, self.staging.open("rb") as prior:
                remaining = offset
                while remaining:
                    left = expected.read(min(_COPY_BYTES, remaining))
                    right = prior.read(len(left))
                    if left != right:
                        self.staging.unlink(missing_ok=True)
                        offset = 0
                        break
                    remaining -= len(left)
            self.resumed = offset > 0
        mode = "ab" if offset else "wb"
        self.received = offset
        with source.open("rb") as source_stream, self.staging.open(mode) as target:
            source_stream.seek(offset)
            while True:
                cancellation.raise_if_cancelled(
                    self.operation_id, self.reference
                )
                block = source_stream.read(_COPY_BYTES)
                if not block:
                    break
                target.write(block)
                target.flush()
                os.fsync(target.fileno())
                self.received += len(block)
                self._emit("transfer")
        if (
            self.received != int(self.reference.size_bytes)
            or _sha256(self.staging) != self.reference.content_digest
        ):
            raise ArtifactApiError(
                ArtifactErrorCode.CONTENT_DIGEST_MISMATCH,
                "local staging payload failed immutable identity validation",
                operation_id=self.operation_id,
                artifact=self.reference,
            )
        self.state = "VERIFIED"

    def status(self) -> ArtifactSessionStatus:
        return ArtifactSessionStatus(
            self.operation_id,
            "PUBLISH",
            self.state,
            self.reference,
        )

    def commit(self) -> ArtifactPublishResult:
        with self.backend.lock:
            destination = self.backend._object_path(
                self.reference.content_digest
            )
            if not destination.is_file():
                if self.state != "VERIFIED" or not self.staging.is_file():
                    raise ArtifactApiError(
                        ArtifactErrorCode.RECOVERY_REQUIRED,
                        "local upload is not verified",
                        operation_id=self.operation_id,
                        artifact=self.reference,
                    )
                destination.parent.mkdir(parents=True, exist_ok=True)
                os.replace(self.staging, destination)
            elif _sha256(destination) != self.reference.content_digest:
                raise ArtifactApiError(
                    ArtifactErrorCode.RECOVERY_REQUIRED,
                    "committed local object failed identity validation",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
            receipt_id = "local-receipt-" + hashlib.sha256(
                (
                    self.operation_id
                    + "\0"
                    + self.reference.content_digest
                ).encode()
            ).hexdigest()
            _atomic_json(
                self.backend._metadata_path(self.reference.content_digest),
                {
                    "operationId": self.operation_id,
                    "artifact": self.reference.to_dict(),
                    "receiptId": receipt_id,
                    "state": "COMMITTED",
                },
            )
        self.received = int(self.reference.size_bytes)
        self._emit("commit", committed=True)
        self.state = "COMMITTED"
        return ArtifactPublishResult(
            reference=self.reference,
            operation_id=self.operation_id,
            requested_replicas=1,
            achieved_replicas=1,
            replicas=(
                ArtifactReplicaResult(
                    repo_node=self.backend.repo_node,
                    state="COMMITTED",
                    receipt_id=receipt_id,
                ),
            ),
            deduplicated=self.deduplicated,
            resumed=self.resumed,
            total_duration_ms=(time.monotonic() - self.started) * 1000,
        )

    def abort(self, preserve_progress: bool) -> ArtifactSessionStatus:
        if not preserve_progress:
            self.staging.unlink(missing_ok=True)
        self.state = "CANCELLED"
        return self.status()


class _LocalFetchDriver:
    def __init__(
        self,
        backend,
        reference,
        destination,
        operation_id,
        *,
        resume,
        verify,
        replace,
        emit,
    ):
        self.backend = backend
        self.reference = reference
        self.destination = destination
        self.operation_id = operation_id
        self.resume = resume
        self.verify = verify
        self.replace = replace
        self.emit = emit
        self.state = "OPEN"
        self.sequence = 0
        self.reused = 0
        self.transferred = 0
        self.started = time.monotonic()

    @property
    def staging(self) -> Path:
        return Path(str(self.destination) + f".{self.operation_id}.part")

    def _emit(self) -> None:
        self.sequence += 1
        complete = self.reused + self.transferred
        self.emit(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.reference,
            phase="deduplicated" if self.reused else "transfer",
            received_bytes=complete,
            verified_bytes=complete,
            committed_bytes=complete,
            total_bytes=int(self.reference.size_bytes),
            selected_replicas=1,
            committed_replicas=1,
            retransmitted_bytes=0,
            sequence=self.sequence,
            timestamp_ms=time.time_ns() // 1_000_000,
        ))

    def transfer(self, cancellation: ArtifactCancellationToken) -> None:
        source = self.backend._object_path(self.reference.content_digest)
        if not source.is_file() or _sha256(source) != self.reference.content_digest:
            raise ArtifactApiError(
                ArtifactErrorCode.RECOVERY_REQUIRED,
                "committed local artifact is unavailable or invalid",
                operation_id=self.operation_id,
                artifact=self.reference,
            )
        if self.destination.exists():
            if (
                self.destination.is_file()
                and self.destination.stat().st_size
                == int(self.reference.size_bytes)
                and _sha256(self.destination) == self.reference.content_digest
            ):
                self.reused = int(self.reference.size_bytes)
                self.state = "VERIFIED"
                self._emit()
                return
            if not self.replace:
                raise ArtifactApiError(
                    ArtifactErrorCode.DESTINATION_CONFLICT,
                    "destination exists with a different immutable identity",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
        self.staging.parent.mkdir(parents=True, exist_ok=True)
        offset = self.staging.stat().st_size if (
            self.resume and self.staging.is_file()
        ) else 0
        if offset > int(self.reference.size_bytes):
            self.staging.unlink(missing_ok=True)
            offset = 0
        if offset:
            with source.open("rb") as expected, self.staging.open("rb") as prior:
                remaining = offset
                while remaining:
                    left = expected.read(min(_COPY_BYTES, remaining))
                    right = prior.read(len(left))
                    if left != right:
                        self.staging.unlink(missing_ok=True)
                        offset = 0
                        break
                    remaining -= len(left)
        mode = "ab" if offset else "wb"
        self.reused = offset
        with source.open("rb") as source_stream, self.staging.open(mode) as target:
            source_stream.seek(offset)
            while True:
                cancellation.raise_if_cancelled(
                    self.operation_id, self.reference
                )
                block = source_stream.read(_COPY_BYTES)
                if not block:
                    break
                target.write(block)
                target.flush()
                os.fsync(target.fileno())
                self.transferred += len(block)
                self._emit()
        if (
            self.staging.stat().st_size != int(self.reference.size_bytes)
            or (self.verify and _sha256(self.staging)
                != self.reference.content_digest)
        ):
            raise ArtifactApiError(
                ArtifactErrorCode.CONTENT_DIGEST_MISMATCH,
                "retrieved local artifact failed verification",
                operation_id=self.operation_id,
                artifact=self.reference,
            )
        self.state = "VERIFIED"

    def status(self) -> ArtifactSessionStatus:
        return ArtifactSessionStatus(
            self.operation_id,
            "FETCH",
            self.state,
            self.reference,
        )

    def commit(self) -> ArtifactFetchResult:
        if not self.reused:
            if self.state != "VERIFIED":
                raise ArtifactApiError(
                    ArtifactErrorCode.RECOVERY_REQUIRED,
                    "local fetch is not verified",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
            os.replace(self.staging, self.destination)
        self.state = "COMMITTED"
        return ArtifactFetchResult(
            reference=self.reference,
            operation_id=self.operation_id,
            destination=self.destination,
            reused_bytes=self.reused,
            transferred_bytes=self.transferred,
            source_replicas=(self.backend.repo_node,),
            total_duration_ms=(time.monotonic() - self.started) * 1000,
        )

    def abort(self, preserve_progress: bool) -> ArtifactSessionStatus:
        if not preserve_progress:
            self.staging.unlink(missing_ok=True)
        self.state = "CANCELLED"
        return self.status()


class FilesystemArtifactApiBackend:
    """Public single-replica backend for local trusted-process use."""

    def __init__(
        self,
        root: Path,
        *,
        repo_node: str = "/local/filesystem-repo",
    ) -> None:
        self.root = Path(root)
        self.repo_node = str(repo_node)
        self.objects = self.root / "objects"
        self.metadata = self.root / "metadata"
        self.sessions = self.root / "sessions"
        for path in (self.objects, self.metadata, self.sessions):
            path.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()

    def _object_path(self, digest: str) -> Path:
        return self.objects / digest[:2] / digest

    def _metadata_path(self, digest: str) -> Path:
        return self.metadata / f"{digest}.json"

    def begin_publish(
        self, descriptor: ArtifactDescriptor, operation_id: str, emit_progress
    ) -> _LocalPublishDriver:
        if descriptor.requested_replicas != 1:
            raise ArtifactApiError(
                ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
                "filesystem backend supports exactly one local replica",
                operation_id=operation_id,
                artifact=descriptor.reference,
            )
        if descriptor.control.mode != ArtifactControlMode.COLLABORATION:
            raise ArtifactApiError(
                ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
                "filesystem backend does not support Targeted network control",
                operation_id=operation_id,
                artifact=descriptor.reference,
            )
        return _LocalPublishDriver(
            self, descriptor, operation_id, emit_progress
        )

    def begin_fetch(
        self,
        reference,
        destination,
        operation_id,
        *,
        resume,
        verify,
        replace,
        timeout_ms,
        control,
        emit_progress,
    ) -> _LocalFetchDriver:
        del timeout_ms
        if control.mode != ArtifactControlMode.COLLABORATION:
            raise ArtifactApiError(
                ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
                "filesystem backend does not support Targeted network control",
                operation_id=operation_id,
                artifact=reference,
            )
        return _LocalFetchDriver(
            self,
            reference,
            Path(destination),
            operation_id,
            resume=bool(resume),
            verify=bool(verify),
            replace=bool(replace),
            emit=emit_progress,
        )


__all__ = ["FilesystemArtifactApiBackend"]
