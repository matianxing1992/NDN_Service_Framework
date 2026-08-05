"""Stable public API for trusted large-artifact publication and retrieval.

The facade owns argument validation, immutable identity construction,
idempotency, progress monotonicity, cancellation, and stable result/error
shapes.  A transport backend owns NDNSF collaboration and the NDN data plane;
applications never call repository control operations or inspect client
private fields.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from enum import Enum
import functools
import hashlib
from pathlib import Path
import queue
import threading
import time
from typing import Callable, Iterable, Optional, Protocol, Sequence, Union

from ._py_repoclient import ArtifactCapability, ArtifactReference


class ArtifactErrorCode(str, Enum):
    INVALID_ARGUMENT = "INVALID_ARGUMENT"
    UNSUPPORTED_CAPABILITY = "UNSUPPORTED_CAPABILITY"
    AUTHORIZATION_FAILED = "AUTHORIZATION_FAILED"
    TRUST_VALIDATION_FAILED = "TRUST_VALIDATION_FAILED"
    MANIFEST_INVALID = "MANIFEST_INVALID"
    CONTENT_DIGEST_MISMATCH = "CONTENT_DIGEST_MISMATCH"
    LEASE_EXPIRED = "LEASE_EXPIRED"
    CAPACITY_UNAVAILABLE = "CAPACITY_UNAVAILABLE"
    TRANSFER_TIMEOUT = "TRANSFER_TIMEOUT"
    CANCELLED = "CANCELLED"
    REPLICA_COMMIT_FAILED = "REPLICA_COMMIT_FAILED"
    DURABILITY_NOT_ACHIEVED = "DURABILITY_NOT_ACHIEVED"
    DESTINATION_CONFLICT = "DESTINATION_CONFLICT"
    RECOVERY_REQUIRED = "RECOVERY_REQUIRED"
    INTERNAL_ERROR = "INTERNAL_ERROR"


class ArtifactControlMode(str, Enum):
    """Public selection of the generic NDNSF collaboration control path."""

    COLLABORATION = "collaboration"
    TARGETED = "targeted"


@dataclass(frozen=True)
class ArtifactControlOptions:
    mode: ArtifactControlMode = ArtifactControlMode.COLLABORATION
    targeted_provider: str = ""

    def __post_init__(self) -> None:
        if (
            self.mode == ArtifactControlMode.TARGETED
            and not self.targeted_provider.strip()
        ):
            raise ValueError(
                "targeted artifact control requires targeted_provider"
            )
        if (
            self.mode == ArtifactControlMode.COLLABORATION
            and self.targeted_provider
        ):
            raise ValueError(
                "targeted_provider is valid only for targeted control"
            )


@dataclass(frozen=True)
class ArtifactCapabilityRequirements:
    """Exact v2 geometry and durability features required from every replica."""

    root_signature_algorithm: str = "ed25519"
    chunk_bytes: int = 1024 * 1024
    root_encoded_bytes: int = 64 * 1024
    page_encoded_bytes: int = 1024 * 1024
    page_entries: int = 4096
    manifest_depth: int = 8
    require_resume: bool = True
    require_replica_receipts: bool = True

    def __post_init__(self) -> None:
        if (
            not self.root_signature_algorithm.strip()
            or self.chunk_bytes <= 0
            or self.root_encoded_bytes <= 0
            or self.page_encoded_bytes <= 0
            or self.page_entries <= 0
            or self.manifest_depth <= 0
        ):
            raise ValueError(
                "artifact capability requirements must be positive and explicit"
            )


@dataclass(frozen=True)
class ArtifactCapabilityRejection:
    repo_node: str
    reasons: tuple[str, ...]


@dataclass(frozen=True)
class ArtifactCapabilityNegotiation:
    artifact: ArtifactReference
    requested_replicas: int
    eligible: tuple[ArtifactCapability, ...]
    rejected: tuple[ArtifactCapabilityRejection, ...]


def negotiate_artifact_capabilities(
    capabilities: Iterable[ArtifactCapability],
    artifact: ArtifactReference,
    *,
    requested_replicas: int,
    requirements: ArtifactCapabilityRequirements = (
        ArtifactCapabilityRequirements()
    ),
) -> ArtifactCapabilityNegotiation:
    """Fail-closed v2 capability filter; placement may rank the eligible set."""

    requested = int(requested_replicas)
    if requested <= 0:
        raise ArtifactApiError(
            ArtifactErrorCode.INVALID_ARGUMENT,
            "requested_replicas must be positive",
            artifact=artifact,
        )
    if artifact.format_version != "artifact-manifest-v2":
        raise ArtifactApiError(
            ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
            "public artifact negotiation requires artifact-manifest-v2; "
            "use the explicit exact-packet-v1 API for legacy packet wires",
            artifact=artifact,
        )

    eligible = []
    rejected = []
    observed_nodes = set()
    for capability in tuple(capabilities):
        repo_node = str(capability.repo_node)
        if repo_node in observed_nodes:
            rejected.append(ArtifactCapabilityRejection(
                repo_node, ("duplicate-repo-node",)
            ))
            continue
        observed_nodes.add(repo_node)
        try:
            reasons = tuple(capability.incompatibilities(
                artifact,
                root_signature_algorithm=(
                    requirements.root_signature_algorithm
                ),
                chunk_bytes=requirements.chunk_bytes,
                root_encoded_bytes=requirements.root_encoded_bytes,
                page_encoded_bytes=requirements.page_encoded_bytes,
                page_entries=requirements.page_entries,
                manifest_depth=requirements.manifest_depth,
                require_resume=requirements.require_resume,
                require_replica_receipts=(
                    requirements.require_replica_receipts
                ),
            ))
        except Exception:
            reasons = ("invalid-capability",)
        if reasons:
            rejected.append(
                ArtifactCapabilityRejection(repo_node, reasons)
            )
        else:
            eligible.append(capability)

    if len(eligible) < requested:
        code = (
            ArtifactErrorCode.UNSUPPORTED_CAPABILITY
            if not eligible
            else ArtifactErrorCode.DURABILITY_NOT_ACHIEVED
        )
        raise ArtifactApiError(
            code,
            "eligible repository capabilities cannot satisfy requested "
            f"durability ({len(eligible)}/{requested})",
            artifact=artifact,
            achieved_replicas=len(eligible),
        )
    return ArtifactCapabilityNegotiation(
        artifact=artifact,
        requested_replicas=requested,
        eligible=tuple(eligible),
        rejected=tuple(rejected),
    )


@dataclass(frozen=True)
class ArtifactDescriptor:
    reference: ArtifactReference
    requested_replicas: int
    idempotency_key: str
    verification: str = "signed-manifest"
    resume: bool = True
    timeout_ms: int = 60_000
    control: ArtifactControlOptions = field(
        default_factory=ArtifactControlOptions
    )

    def __post_init__(self) -> None:
        if self.requested_replicas <= 0:
            raise ValueError("requested_replicas must be positive")
        if not self.idempotency_key.strip():
            raise ValueError("idempotency_key must not be empty")
        if self.verification != "signed-manifest":
            raise ValueError(
                "artifact-manifest-v2 requires signed-manifest verification"
            )
        if self.timeout_ms <= 0:
            raise ValueError("timeout_ms must be positive")


@dataclass(frozen=True)
class ArtifactProgress:
    operation_id: str
    artifact: ArtifactReference
    phase: str
    received_bytes: int
    verified_bytes: int
    committed_bytes: int
    total_bytes: int
    selected_replicas: int
    committed_replicas: int
    retransmitted_bytes: int
    sequence: int
    timestamp_ms: int

    def __post_init__(self) -> None:
        counters = (
            self.received_bytes,
            self.verified_bytes,
            self.committed_bytes,
            self.total_bytes,
            self.selected_replicas,
            self.committed_replicas,
            self.retransmitted_bytes,
            self.sequence,
            self.timestamp_ms,
        )
        if (
            not self.operation_id
            or not self.phase
            or min(counters) < 0
            or self.verified_bytes > self.received_bytes
            or self.committed_bytes > self.verified_bytes
            or self.received_bytes > self.total_bytes
            or self.committed_replicas > self.selected_replicas
        ):
            raise ValueError("artifact progress is invalid or unbounded")


@dataclass(frozen=True)
class ArtifactReplicaResult:
    repo_node: str
    state: str
    receipt_id: str = ""
    error_code: str = ""


@dataclass(frozen=True)
class ArtifactPublishResult:
    reference: ArtifactReference
    operation_id: str
    requested_replicas: int
    achieved_replicas: int
    replicas: tuple[ArtifactReplicaResult, ...]
    deduplicated: bool = False
    resumed: bool = False
    total_duration_ms: float = 0.0
    phase_durations_ms: dict[str, float] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if (
            not self.operation_id
            or self.requested_replicas <= 0
            or self.achieved_replicas < 0
            or self.achieved_replicas > self.requested_replicas
            or self.total_duration_ms < 0
            or any(value < 0 for value in self.phase_durations_ms.values())
        ):
            raise ValueError("artifact publication result is invalid")
        committed = {
            result.receipt_id
            for result in self.replicas
            if result.state == "COMMITTED" and result.receipt_id
        }
        if len(committed) != self.achieved_replicas:
            raise ValueError(
                "achieved durability must equal distinct committed receipts"
            )


@dataclass(frozen=True)
class ArtifactFetchResult:
    reference: ArtifactReference
    operation_id: str
    destination: Path
    reused_bytes: int
    transferred_bytes: int
    source_replicas: tuple[str, ...]
    total_duration_ms: float = 0.0
    phase_durations_ms: dict[str, float] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if (
            not self.operation_id
            or self.reused_bytes < 0
            or self.transferred_bytes < 0
            or self.total_duration_ms < 0
            or self.reused_bytes + self.transferred_bytes
            != int(self.reference.size_bytes)
            or any(value < 0 for value in self.phase_durations_ms.values())
        ):
            raise ValueError("artifact retrieval result is invalid")


@dataclass(frozen=True)
class ArtifactSessionStatus:
    operation_id: str
    direction: str
    state: str
    artifact: ArtifactReference
    progress: Optional[ArtifactProgress] = None
    error_code: str = ""


class ArtifactApiError(RuntimeError):
    """Bounded, stable public error without peer-controlled raw diagnostics."""

    def __init__(
        self,
        code: Union[ArtifactErrorCode, str],
        message: str,
        *,
        operation_id: str = "",
        artifact: Optional[ArtifactReference] = None,
        achieved_replicas: int = 0,
    ) -> None:
        self.code = (
            code if isinstance(code, ArtifactErrorCode)
            else ArtifactErrorCode(str(code))
        )
        self.operation_id = str(operation_id)
        self.artifact = artifact
        self.achieved_replicas = max(0, int(achieved_replicas))
        bounded = " ".join(str(message).split())[:512]
        super().__init__(f"{self.code.value}: {bounded}")


class ArtifactCancellationToken:
    def __init__(self) -> None:
        self._event = threading.Event()

    def cancel(self) -> None:
        self._event.set()

    @property
    def cancelled(self) -> bool:
        return self._event.is_set()

    def raise_if_cancelled(
        self,
        operation_id: str,
        artifact: Optional[ArtifactReference] = None,
    ) -> None:
        if self.cancelled:
            raise ArtifactApiError(
                ArtifactErrorCode.CANCELLED,
                "artifact operation was cancelled",
                operation_id=operation_id,
                artifact=artifact,
            )


ProgressObserver = Callable[[ArtifactProgress], None]


class ArtifactPublishDriver(Protocol):
    def transfer(
        self, path: Path, cancellation: ArtifactCancellationToken
    ) -> None: ...

    def status(self) -> ArtifactSessionStatus: ...

    def commit(self) -> ArtifactPublishResult: ...

    def abort(self, preserve_progress: bool) -> ArtifactSessionStatus: ...


class ArtifactFetchDriver(Protocol):
    def transfer(self, cancellation: ArtifactCancellationToken) -> None: ...

    def status(self) -> ArtifactSessionStatus: ...

    def commit(self) -> ArtifactFetchResult: ...

    def abort(self, preserve_progress: bool) -> ArtifactSessionStatus: ...


class ArtifactApiBackend(Protocol):
    def begin_publish(
        self,
        descriptor: ArtifactDescriptor,
        operation_id: str,
        emit_progress: ProgressObserver,
    ) -> ArtifactPublishDriver: ...

    def begin_fetch(
        self,
        reference: ArtifactReference,
        destination: Path,
        operation_id: str,
        *,
        resume: bool,
        verify: bool,
        replace: bool,
        timeout_ms: int,
        control: ArtifactControlOptions,
        emit_progress: ProgressObserver,
    ) -> ArtifactFetchDriver: ...


class _BoundedProgressDispatcher:
    def __init__(self, observer: Optional[ProgressObserver]) -> None:
        self._observer = observer
        self._queue: queue.Queue[Optional[ArtifactProgress]] = queue.Queue(1)
        self._thread: Optional[threading.Thread] = None
        if observer is not None:
            self._thread = threading.Thread(
                target=self._run,
                name="artifact-progress-observer",
                daemon=True,
            )
            self._thread.start()

    def _run(self) -> None:
        while True:
            item = self._queue.get()
            if item is None:
                return
            try:
                assert self._observer is not None
                self._observer(item)
            except BaseException:
                # Observers are advisory and cannot fail or stop the transfer.
                pass

    def submit(self, progress: ArtifactProgress) -> None:
        if self._thread is None:
            return
        try:
            self._queue.put_nowait(progress)
        except queue.Full:
            try:
                self._queue.get_nowait()
            except queue.Empty:
                pass
            try:
                self._queue.put_nowait(progress)
            except queue.Full:
                pass

    def close(self) -> None:
        if self._thread is None:
            return
        try:
            # Preserve the newest queued observation. Waiting is strictly
            # bounded and only until the worker accepts that item; the callback
            # itself remains unable to block the transfer engine.
            self._queue.put(None, timeout=0.05)
        except queue.Full:
            return
        self._thread.join(timeout=0.1)


class _ProgressGuard:
    def __init__(
        self,
        operation_id: str,
        artifact: ArtifactReference,
        observer: Optional[ProgressObserver],
    ) -> None:
        self.operation_id = operation_id
        self.artifact = artifact
        self.latest: Optional[ArtifactProgress] = None
        self.dispatcher = _BoundedProgressDispatcher(observer)
        self._lock = threading.Lock()

    def emit(self, progress: ArtifactProgress) -> None:
        with self._lock:
            if (
                progress.operation_id != self.operation_id
                or not _same_reference(progress.artifact, self.artifact)
            ):
                raise ArtifactApiError(
                    ArtifactErrorCode.INTERNAL_ERROR,
                    "backend emitted progress for a different operation",
                    operation_id=self.operation_id,
                    artifact=self.artifact,
                )
            previous = self.latest
            if previous is not None and (
                progress.sequence <= previous.sequence
                or progress.timestamp_ms < previous.timestamp_ms
                or progress.received_bytes < previous.received_bytes
                or progress.verified_bytes < previous.verified_bytes
                or progress.committed_bytes < previous.committed_bytes
                or progress.selected_replicas < previous.selected_replicas
                or progress.committed_replicas < previous.committed_replicas
                or progress.retransmitted_bytes < previous.retransmitted_bytes
            ):
                raise ArtifactApiError(
                    ArtifactErrorCode.INTERNAL_ERROR,
                    "backend progress is not monotonic",
                    operation_id=self.operation_id,
                    artifact=self.artifact,
                )
            self.latest = progress
        self.dispatcher.submit(progress)

    def close(self) -> None:
        self.dispatcher.close()


class ArtifactUploadSession:
    def __init__(
        self,
        descriptor: ArtifactDescriptor,
        operation_id: str,
        driver: ArtifactPublishDriver,
        guard: _ProgressGuard,
        cancellation: ArtifactCancellationToken,
    ) -> None:
        self.descriptor = descriptor
        self.operation_id = operation_id
        self._driver = driver
        self._guard = guard
        self._cancellation = cancellation
        self._transferred = False
        self._terminal = False
        self._result: Optional[ArtifactPublishResult] = None
        self._terminal_status: Optional[ArtifactSessionStatus] = None
        self._lock = threading.RLock()

    def upload_file(self, path: Union[str, Path]) -> ArtifactSessionStatus:
        with self._lock:
            if self._terminal:
                raise ArtifactApiError(
                    ArtifactErrorCode.RECOVERY_REQUIRED,
                    "upload session is already terminal",
                    operation_id=self.operation_id,
                    artifact=self.descriptor.reference,
                )
            source = _validated_source(path, self.descriptor.reference)
            self._cancellation.raise_if_cancelled(
                self.operation_id, self.descriptor.reference
            )
        try:
            self._driver.transfer(source, self._cancellation)
        except Exception as error:
            raise _as_api_error(
                error, self.operation_id, self.descriptor.reference
            )
        with self._lock:
            if self._terminal:
                raise ArtifactApiError(
                    ArtifactErrorCode.CANCELLED,
                    "upload was cancelled while transferring",
                    operation_id=self.operation_id,
                    artifact=self.descriptor.reference,
                )
            self._transferred = True
            return self.status()

    def status(self) -> ArtifactSessionStatus:
        try:
            return self._driver.status()
        except Exception as error:
            raise _as_api_error(
                error, self.operation_id, self.descriptor.reference
            )

    def commit(self) -> ArtifactPublishResult:
        with self._lock:
            if self._result is not None:
                return self._result
            if not self._transferred:
                raise ArtifactApiError(
                    ArtifactErrorCode.RECOVERY_REQUIRED,
                    "upload must complete before commit",
                    operation_id=self.operation_id,
                    artifact=self.descriptor.reference,
                )
            try:
                result = self._driver.commit()
            except Exception as error:
                raise _as_api_error(
                    error, self.operation_id, self.descriptor.reference
                )
            if (
                result.operation_id != self.operation_id
                or not _same_reference(
                    result.reference, self.descriptor.reference
                )
            ):
                raise ArtifactApiError(
                    ArtifactErrorCode.INTERNAL_ERROR,
                    "backend returned a result for a different operation",
                    operation_id=self.operation_id,
                    artifact=self.descriptor.reference,
                )
            self._terminal = True
            self._result = result
            self._terminal_status = ArtifactSessionStatus(
                self.operation_id,
                "PUBLISH",
                "COMMITTED",
                self.descriptor.reference,
                self._guard.latest,
            )
            self._guard.close()
            return result

    def abort(self, preserve_progress: bool = True) -> ArtifactSessionStatus:
        # Signal first so a concurrent transfer can leave its blocking NDN
        # operation at the next packet boundary; do not wait on the session
        # mutex before publishing cancellation.
        self._cancellation.cancel()
        with self._lock:
            if self._terminal_status is not None:
                return self._terminal_status
            self._terminal = True
        try:
            status = self._driver.abort(bool(preserve_progress))
        except Exception as error:
            with self._lock:
                self._terminal = True
            raise _as_api_error(
                error, self.operation_id, self.descriptor.reference
            )
        with self._lock:
            self._terminal_status = status
            self._guard.close()
            return status


class ArtifactFetchSession:
    def __init__(
        self,
        reference: ArtifactReference,
        destination: Path,
        operation_id: str,
        driver: ArtifactFetchDriver,
        guard: _ProgressGuard,
        cancellation: ArtifactCancellationToken,
    ) -> None:
        self.reference = reference
        self.destination = destination
        self.operation_id = operation_id
        self._driver = driver
        self._guard = guard
        self._cancellation = cancellation
        self._transferred = False
        self._terminal = False
        self._result: Optional[ArtifactFetchResult] = None
        self._terminal_status: Optional[ArtifactSessionStatus] = None
        self._lock = threading.RLock()

    def transfer(self) -> ArtifactSessionStatus:
        with self._lock:
            if self._terminal:
                raise ArtifactApiError(
                    ArtifactErrorCode.RECOVERY_REQUIRED,
                    "fetch session is already terminal",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
            self._cancellation.raise_if_cancelled(
                self.operation_id, self.reference
            )
        try:
            self._driver.transfer(self._cancellation)
        except Exception as error:
            raise _as_api_error(
                error, self.operation_id, self.reference
            )
        with self._lock:
            if self._terminal:
                raise ArtifactApiError(
                    ArtifactErrorCode.CANCELLED,
                    "fetch was cancelled while transferring",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
            self._transferred = True
            return self.status()

    def status(self) -> ArtifactSessionStatus:
        try:
            return self._driver.status()
        except Exception as error:
            raise _as_api_error(error, self.operation_id, self.reference)

    def commit(self) -> ArtifactFetchResult:
        with self._lock:
            if self._result is not None:
                return self._result
            if not self._transferred:
                raise ArtifactApiError(
                    ArtifactErrorCode.RECOVERY_REQUIRED,
                    "fetch must complete before finalize",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
            try:
                result = self._driver.commit()
            except Exception as error:
                raise _as_api_error(
                    error, self.operation_id, self.reference
                )
            if (
                result.operation_id != self.operation_id
                or not _same_reference(result.reference, self.reference)
                or result.destination != self.destination
            ):
                raise ArtifactApiError(
                    ArtifactErrorCode.INTERNAL_ERROR,
                    "backend returned a result for a different fetch",
                    operation_id=self.operation_id,
                    artifact=self.reference,
                )
            self._terminal = True
            self._result = result
            self._terminal_status = ArtifactSessionStatus(
                self.operation_id,
                "FETCH",
                "COMMITTED",
                self.reference,
                self._guard.latest,
            )
            self._guard.close()
            return result

    def abort(self, preserve_progress: bool = True) -> ArtifactSessionStatus:
        self._cancellation.cancel()
        with self._lock:
            if self._terminal_status is not None:
                return self._terminal_status
            self._terminal = True
        try:
            status = self._driver.abort(bool(preserve_progress))
        except Exception as error:
            with self._lock:
                self._terminal = True
            raise _as_api_error(error, self.operation_id, self.reference)
        with self._lock:
            self._terminal_status = status
            self._guard.close()
            return status


class ArtifactRepositoryApi:
    def __init__(
        self,
        backend: Optional[ArtifactApiBackend],
        *,
        publisher_identity: str,
        default_timeout_ms: int = 60_000,
    ) -> None:
        self.backend = backend
        self.publisher_identity = publisher_identity.rstrip("/")
        self.default_timeout_ms = int(default_timeout_ms)

    def _require_backend(self) -> ArtifactApiBackend:
        if self.backend is None:
            raise ArtifactApiError(
                ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
                "no artifact-manifest-v2 backend is configured",
            )
        return self.backend

    def begin_upload(
        self,
        descriptor: ArtifactDescriptor,
        *,
        on_progress: Optional[ProgressObserver] = None,
        cancellation: Optional[ArtifactCancellationToken] = None,
    ) -> ArtifactUploadSession:
        operation_id = _operation_id(
            "publish", descriptor.idempotency_key, descriptor.reference
        )
        guard = _ProgressGuard(
            operation_id, descriptor.reference, on_progress
        )
        token = cancellation or ArtifactCancellationToken()
        try:
            driver = self._require_backend().begin_publish(
                descriptor, operation_id, guard.emit
            )
        except Exception as error:
            guard.close()
            raise _as_api_error(error, operation_id, descriptor.reference)
        return ArtifactUploadSession(
            descriptor, operation_id, driver, guard, token
        )

    def publish_file(
        self,
        path: Union[str, Path],
        *,
        name: str,
        expected_sha256: str,
        replicas: int = 1,
        verification: str = "signed-manifest",
        resume: bool = True,
        on_progress: Optional[ProgressObserver] = None,
        idempotency_key: str = "",
        policy_epoch: str = "default",
        timeout_ms: Optional[int] = None,
        control: ArtifactControlOptions = ArtifactControlOptions(),
        cancellation: Optional[ArtifactCancellationToken] = None,
    ) -> ArtifactPublishResult:
        source = Path(path)
        size, digest = _file_identity(source)
        expected = expected_sha256.strip().lower()
        if digest != expected:
            raise ArtifactApiError(
                ArtifactErrorCode.CONTENT_DIGEST_MISMATCH,
                "source file does not match expected_sha256",
            )
        reference = _make_reference(
            name=name,
            digest=digest,
            size=size,
            publisher_identity=self.publisher_identity,
            policy_epoch=policy_epoch,
        )
        key = idempotency_key.strip() or (
            f"publish:{reference.logical_name}:{digest}"
        )
        descriptor = ArtifactDescriptor(
            reference=reference,
            requested_replicas=int(replicas),
            idempotency_key=key,
            verification=verification,
            resume=bool(resume),
            timeout_ms=int(timeout_ms or self.default_timeout_ms),
            control=control,
        )
        session = self.begin_upload(
            descriptor,
            on_progress=on_progress,
            cancellation=cancellation,
        )
        try:
            session.upload_file(source)
            return session.commit()
        except Exception as error:
            if not session._terminal:
                try:
                    session.abort(preserve_progress=bool(resume))
                except Exception:
                    pass
            raise _as_api_error(error, session.operation_id, reference)

    async def publish_file_async(self, *args, **kwargs) -> ArtifactPublishResult:
        token = kwargs.get("cancellation") or ArtifactCancellationToken()
        kwargs["cancellation"] = token
        call = functools.partial(self.publish_file, *args, **kwargs)
        future = asyncio.get_running_loop().run_in_executor(None, call)
        try:
            return await future
        except asyncio.CancelledError:
            token.cancel()
            raise

    def begin_fetch(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        *,
        resume: bool = True,
        verify: bool = True,
        replace: bool = False,
        on_progress: Optional[ProgressObserver] = None,
        idempotency_key: str = "",
        timeout_ms: Optional[int] = None,
        control: ArtifactControlOptions = ArtifactControlOptions(),
        cancellation: Optional[ArtifactCancellationToken] = None,
    ) -> ArtifactFetchSession:
        destination_path = Path(destination)
        key = idempotency_key.strip() or (
            f"fetch:{reference.content_digest}:"
            f"{destination_path.absolute()}"
        )
        operation_id = _operation_id("fetch", key, reference)
        guard = _ProgressGuard(operation_id, reference, on_progress)
        token = cancellation or ArtifactCancellationToken()
        try:
            driver = self._require_backend().begin_fetch(
                reference,
                destination_path,
                operation_id,
                resume=bool(resume),
                verify=bool(verify),
                replace=bool(replace),
                timeout_ms=int(timeout_ms or self.default_timeout_ms),
                control=control,
                emit_progress=guard.emit,
            )
        except Exception as error:
            guard.close()
            raise _as_api_error(error, operation_id, reference)
        return ArtifactFetchSession(
            reference,
            destination_path,
            operation_id,
            driver,
            guard,
            token,
        )

    def fetch_file(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        **kwargs,
    ) -> ArtifactFetchResult:
        session = self.begin_fetch(reference, destination, **kwargs)
        resume = bool(kwargs.get("resume", True))
        try:
            session.transfer()
            return session.commit()
        except Exception as error:
            if not session._terminal:
                try:
                    session.abort(preserve_progress=resume)
                except Exception:
                    pass
            raise _as_api_error(error, session.operation_id, reference)

    async def fetch_file_async(self, *args, **kwargs) -> ArtifactFetchResult:
        token = kwargs.get("cancellation") or ArtifactCancellationToken()
        kwargs["cancellation"] = token
        call = functools.partial(self.fetch_file, *args, **kwargs)
        future = asyncio.get_running_loop().run_in_executor(None, call)
        try:
            return await future
        except asyncio.CancelledError:
            token.cancel()
            raise


def _same_reference(left: ArtifactReference, right: ArtifactReference) -> bool:
    fields = (
        "logical_name",
        "digest_algorithm",
        "content_digest",
        "size_bytes",
        "format_version",
        "root_manifest_name",
        "publisher_identity",
        "policy_epoch",
    )
    return all(getattr(left, field) == getattr(right, field) for field in fields)


def _as_api_error(
    error: BaseException,
    operation_id: str,
    artifact: Optional[ArtifactReference],
) -> ArtifactApiError:
    if isinstance(error, ArtifactApiError):
        return error
    if isinstance(error, TimeoutError):
        code = ArtifactErrorCode.TRANSFER_TIMEOUT
        message = "artifact transfer timed out"
    else:
        code = ArtifactErrorCode.INTERNAL_ERROR
        message = "artifact backend failed"
    return ArtifactApiError(
        code,
        message,
        operation_id=operation_id,
        artifact=artifact,
    )


def _file_identity(path: Path) -> tuple[int, str]:
    if not path.is_file():
        raise ArtifactApiError(
            ArtifactErrorCode.INVALID_ARGUMENT,
            "artifact source must be an existing regular file",
        )
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            digest.update(block)
    return size, digest.hexdigest()


def _validated_source(path: Union[str, Path], reference: ArtifactReference) -> Path:
    source = Path(path)
    size, digest = _file_identity(source)
    if size != int(reference.size_bytes) or digest != reference.content_digest:
        raise ArtifactApiError(
            ArtifactErrorCode.CONTENT_DIGEST_MISMATCH,
            "upload source no longer matches immutable artifact identity",
            artifact=reference,
        )
    return source


def _make_reference(
    *,
    name: str,
    digest: str,
    size: int,
    publisher_identity: str,
    policy_epoch: str,
) -> ArtifactReference:
    logical_name = str(name).strip()
    if not logical_name.startswith("/"):
        raise ArtifactApiError(
            ArtifactErrorCode.INVALID_ARGUMENT,
            "artifact logical name must be an absolute NDN name",
        )
    if not publisher_identity.startswith("/"):
        raise ArtifactApiError(
            ArtifactErrorCode.INVALID_ARGUMENT,
            "publisher identity must be an absolute NDN name",
        )
    # Round-trip through the native validator to bind public construction to
    # the same limits and field semantics as wire decoding.
    from ._py_repoclient import artifact_reference_from_dict

    return artifact_reference_from_dict({
        "logicalName": logical_name.rstrip("/"),
        "digestAlgorithm": "sha256",
        "contentDigest": digest,
        "sizeBytes": int(size),
        "formatVersion": "artifact-manifest-v2",
        "rootManifestName": f"{logical_name.rstrip('/')}/root",
        "publisherIdentity": publisher_identity,
        "policyEpoch": str(policy_epoch).strip(),
    })


def _operation_id(
    direction: str, idempotency_key: str, reference: ArtifactReference
) -> str:
    canonical = "\0".join((
        direction,
        idempotency_key.strip(),
        reference.logical_name,
        reference.content_digest,
        str(reference.size_bytes),
        reference.root_manifest_name,
        reference.policy_epoch,
    ))
    return "artifact-" + hashlib.sha256(canonical.encode()).hexdigest()


__all__ = [
    "ArtifactApiBackend",
    "ArtifactApiError",
    "ArtifactCancellationToken",
    "ArtifactCapabilityNegotiation",
    "ArtifactCapabilityRejection",
    "ArtifactCapabilityRequirements",
    "ArtifactControlMode",
    "ArtifactControlOptions",
    "ArtifactDescriptor",
    "ArtifactErrorCode",
    "ArtifactFetchDriver",
    "ArtifactFetchResult",
    "ArtifactFetchSession",
    "ArtifactProgress",
    "ArtifactPublishDriver",
    "ArtifactPublishResult",
    "ArtifactReplicaResult",
    "ArtifactRepositoryApi",
    "ArtifactSessionStatus",
    "ArtifactUploadSession",
    "negotiate_artifact_capabilities",
]
