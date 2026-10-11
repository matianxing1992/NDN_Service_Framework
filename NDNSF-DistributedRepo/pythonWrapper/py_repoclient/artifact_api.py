"""Thin Python projection of the native artifact repository API."""

from __future__ import annotations

from pathlib import Path
from typing import Union

from ._py_repoclient import (
    ArtifactCapabilityRequirements,
    ArtifactCommitReceipt,
    ArtifactFetchAttempt,
    ArtifactFetchError as _NativeArtifactFetchError,
    ArtifactFetchResult,
    ArtifactHandle,
    ArtifactLocalCommitRecord,
    ArtifactPublishResult,
    ArtifactReplicaCommit,
    ArtifactReplicaResult,
    AvailabilityLocationReport,
    AvailabilityReport,
    InAppRepoConfig,
    NativeArtifactRepositoryApi,
    PersistentRepoClientConfig,
    PublishOperationHandle,
    PublishOperationStatus,
    RepoCandidate,
    RepoCandidateSet,
    RepoWriteConsistency,
    SelectedRepoSet,
    SelectedRepoTarget,
)
from .compat import (
    RepoCompatibilityError,
    raise_repo_compatibility_error,
    retired_repo_feature,
)


class ArtifactApiError(RuntimeError):
    """Native fetch failure with its structured result preserved."""

    def __init__(self, message: str, result: ArtifactFetchResult | None = None) -> None:
        self.result = result
        super().__init__(message)


class ArtifactRepositoryApi:
    """Own one native In-App store or borrow a retained native ServiceUser."""

    def __init__(self, native_api=None, *args, **kwargs) -> None:
        if args or kwargs or not isinstance(native_api, NativeArtifactRepositoryApi):
            raise_repo_compatibility_error(
                "ArtifactRepositoryApi(backend=...)",
                "use make_in_app(InAppRepoConfig) or make_persistent(ServiceUser)",
            )
        self._native = native_api

    @classmethod
    def make_in_app(
        cls,
        config: InAppRepoConfig | None = None,
        *,
        durable_root: Union[str, Path] | None = None,
        owner_id: str | None = None,
    ) -> "ArtifactRepositoryApi":
        if config is None:
            if durable_root is None:
                raise TypeError("make_in_app requires config or durable_root")
            if not owner_id:
                raise TypeError("make_in_app requires a non-empty owner_id")
            config = InAppRepoConfig()
            config.durable_root = str(durable_root)
            config.owner_id = str(owner_id)
        elif durable_root is not None:
            raise TypeError("pass either config or durable_root, not both")
        else:
            native_config = InAppRepoConfig()
            native_config.durable_root = str(config.durable_root)
            native_config.owner_id = str(config.owner_id)
            if not native_config.owner_id:
                raise TypeError("make_in_app requires a non-empty config.owner_id")
            native_config.limits = config.limits
            native_config.availability_freshness_ms = int(
                config.availability_freshness_ms
            )
            config = native_config
        return cls(NativeArtifactRepositoryApi.make_in_app(config))

    @classmethod
    def make_persistent(
        cls,
        service_user,
        config: PersistentRepoClientConfig | None = None,
        *,
        ack_timeout_ms: int | None = None,
        timeout_ms: int | None = None,
    ) -> "ArtifactRepositoryApi":
        native_service_user = getattr(service_user, "_native", service_user)
        native_config = PersistentRepoClientConfig()
        if config is None:
            ack_timeout_ms = 1000 if ack_timeout_ms is None else ack_timeout_ms
            timeout_ms = 30000 if timeout_ms is None else timeout_ms
        else:
            for name in (
                "limits",
                "service_prefix",
                "provider_scope",
                "ack_collection_time_ms",
                "request_timeout_ms",
                "operation_wait_timeout_ms",
                "fetch_failure_cooldown_ms",
                "max_fetch_health_entries",
            ):
                setattr(native_config, name, getattr(config, name))
        if ack_timeout_ms is not None:
            native_config.ack_collection_time_ms = int(ack_timeout_ms)
        if timeout_ms is not None:
            native_config.request_timeout_ms = int(timeout_ms)
            native_config.operation_wait_timeout_ms = int(timeout_ms)
        return cls(
            NativeArtifactRepositoryApi.make_persistent(
                native_service_user, native_config
            )
        )

    def publish_file(
        self,
        source_path: Union[str, Path],
        reference=None,
        selected: SelectedRepoSet | None = None,
        timeout_ms: int = 30000,
        **legacy_options,
    ) -> ArtifactPublishResult:
        if legacy_options or reference is None:
            raise_repo_compatibility_error(
                "publish_file legacy options",
                "pass a native ArtifactReference to ArtifactRepositoryApi.publish_file",
            )
        path = str(source_path)
        if selected is None:
            return self._call(self._native.publish_file, path, reference)
        return self._call(
            self._native.publish_file, path, reference, selected, int(timeout_ms)
        )

    def begin_publish_file(
        self,
        source_path: Union[str, Path],
        reference,
        selected: SelectedRepoSet,
        idempotency_key: str,
        timeout_ms: int = 30000,
    ) -> PublishOperationHandle:
        return self._call(
            self._native.begin_publish_file,
            str(source_path),
            reference,
            selected,
            str(idempotency_key),
            int(timeout_ms),
        )

    def begin_archive(
        self,
        in_app_source: "ArtifactRepositoryApi",
        handle: ArtifactHandle,
        selected: SelectedRepoSet,
        idempotency_key: str,
        timeout_ms: int = 30000,
    ) -> PublishOperationHandle:
        return self._call(
            self._native.begin_archive,
            self._require_api(in_app_source),
            handle,
            selected,
            str(idempotency_key),
            int(timeout_ms),
        )

    def get_publish_status(
        self, handle: PublishOperationHandle, timeout_ms: int = 0
    ) -> PublishOperationStatus:
        return self._call(
            self._native.get_publish_status, handle, int(timeout_ms)
        )

    def discover_repo_candidates(
        self, requirements: ArtifactCapabilityRequirements, timeout_ms: int = 30000
    ) -> RepoCandidateSet:
        return self._call(
            self._native.discover_repo_candidates, requirements, int(timeout_ms)
        )

    def archive(
        self,
        in_app_source: "ArtifactRepositoryApi",
        handle: ArtifactHandle,
        selected: SelectedRepoSet,
        timeout_ms: int = 30000,
    ) -> ArtifactPublishResult:
        return self._call(
            self._native.archive,
            self._require_api(in_app_source),
            handle,
            selected,
            int(timeout_ms),
        )

    def fetch_file(
        self,
        reference_or_handle,
        destination: Union[str, Path],
        timeout_ms: int = 30000,
        **legacy_options,
    ) -> ArtifactFetchResult | None:
        """Fetch a native reference locally or a Persistent handle remotely."""
        if legacy_options:
            raise_repo_compatibility_error(
                "fetch_file legacy options",
                "use native ArtifactRepositoryApi.fetch_file and its timeout_ms argument",
            )
        if isinstance(reference_or_handle, ArtifactHandle):
            if reference_or_handle.lifecycle_mode == "IN_APP":
                return self._call(
                    self._native.fetch_file,
                    reference_or_handle.reference,
                    str(destination),
                )
            return self._call(
                self._native.fetch_file,
                reference_or_handle,
                str(destination),
                int(timeout_ms),
            )
        return self._call(
            self._native.fetch_file, reference_or_handle, str(destination)
        )

    def configure_artifact_backend(self, *args, **kwargs) -> None:
        raise_repo_compatibility_error(
            "configure_artifact_backend",
            "construct a native ArtifactRepositoryApi with its lifecycle factory",
        )

    def begin_upload(self, *args, **kwargs):
        raise_repo_compatibility_error(
            "begin_upload", "use native ArtifactRepositoryApi.begin_publish_file"
        )

    def begin_fetch(self, *args, **kwargs):
        raise_repo_compatibility_error(
            "begin_fetch", "use native ArtifactRepositoryApi.fetch_file"
        )

    def publish_file_async(self, *args, **kwargs):
        raise_repo_compatibility_error(
            "publish_file_async", "use synchronous native ArtifactRepositoryApi.publish_file"
        )

    def fetch_file_async(self, *args, **kwargs):
        raise_repo_compatibility_error(
            "fetch_file_async", "use synchronous native ArtifactRepositoryApi.fetch_file"
        )

    def lookup_local_commit(self, reference):
        return self._call(self._native.lookup_local_commit, reference)

    def get_receipt(
        self,
        receipt_id: str,
        handle: ArtifactHandle | None = None,
        timeout_ms: int = 0,
    ) -> ArtifactCommitReceipt:
        return self._call(
            self._native.get_receipt,
            str(receipt_id),
            handle,
            int(timeout_ms),
        )

    def check_availability(
        self,
        handle: ArtifactHandle,
        probe_kind: str = "metadata",
        timeout_ms: int = 0,
        max_age_ms: int = 0,
    ) -> AvailabilityReport:
        return self._call(
            self._native.check_availability,
            handle,
            str(probe_kind),
            int(timeout_ms),
            int(max_age_ms),
        )

    @staticmethod
    def _require_api(value: "ArtifactRepositoryApi") -> NativeArtifactRepositoryApi:
        if not isinstance(value, ArtifactRepositoryApi):
            raise TypeError("source must be an ArtifactRepositoryApi")
        return value._native

    @staticmethod
    def _call(function, *args):
        try:
            return function(*args)
        except _NativeArtifactFetchError as error:
            raise ArtifactApiError(str(error), error.result) from error


# Existing value names map directly to native C++ types.
ArtifactDescriptor = retired_repo_feature(
    "ArtifactDescriptor", "use native ArtifactReference"
)
ArtifactApiBackend = retired_repo_feature(
    "ArtifactApiBackend", "use ArtifactRepositoryApi.make_in_app or make_persistent"
)
ArtifactCancellationToken = retired_repo_feature(
    "ArtifactCancellationToken", "use native ArtifactRepositoryApi timeouts"
)
ArtifactCapabilityNegotiation = retired_repo_feature(
    "ArtifactCapabilityNegotiation", "use native ArtifactCapabilityRequirements"
)
ArtifactCapabilityRejection = retired_repo_feature(
    "ArtifactCapabilityRejection", "use native ArtifactCapabilityRequirements"
)
ArtifactControlMode = retired_repo_feature(
    "ArtifactControlMode", "use native PersistentRepoClientConfig"
)
ArtifactControlOptions = retired_repo_feature(
    "ArtifactControlOptions", "use native PersistentRepoClientConfig"
)
ArtifactErrorCode = retired_repo_feature(
    "ArtifactErrorCode", "handle native validation errors and ArtifactApiError"
)
ArtifactFetchDriver = retired_repo_feature(
    "ArtifactFetchDriver", "use ArtifactRepositoryApi.fetch_file"
)
ArtifactFetchSession = retired_repo_feature(
    "ArtifactFetchSession", "use ArtifactRepositoryApi.fetch_file"
)
ArtifactProgress = retired_repo_feature(
    "ArtifactProgress", "use native ArtifactFetchResult or PublishOperationStatus"
)
ArtifactPublishDriver = retired_repo_feature(
    "ArtifactPublishDriver", "use ArtifactRepositoryApi.publish_file"
)
ArtifactSessionStatus = retired_repo_feature(
    "ArtifactSessionStatus", "use native PublishOperationStatus"
)
ArtifactUploadSession = retired_repo_feature(
    "ArtifactUploadSession", "use native ArtifactRepositoryApi.begin_publish_file"
)
SelectedRepoBinding = SelectedRepoTarget
LocalArtifactCommit = ArtifactLocalCommitRecord
WriteConsistency = RepoWriteConsistency


def negotiate_artifact_capabilities(*args, **kwargs):
    raise_repo_compatibility_error(
        "negotiate_artifact_capabilities",
        "use ArtifactRepositoryApi.discover_repo_candidates with native requirements",
    )


__all__ = [
    "ArtifactApiBackend",
    "ArtifactApiError",
    "ArtifactCancellationToken",
    "ArtifactCapabilityNegotiation",
    "ArtifactCapabilityRejection",
    "ArtifactCapabilityRequirements",
    "ArtifactCommitReceipt",
    "ArtifactDescriptor",
    "ArtifactErrorCode",
    "ArtifactFetchAttempt",
    "ArtifactFetchResult",
    "ArtifactHandle",
    "ArtifactLocalCommitRecord",
    "ArtifactProgress",
    "ArtifactPublishDriver",
    "ArtifactPublishResult",
    "ArtifactReplicaCommit",
    "ArtifactReplicaResult",
    "ArtifactRepositoryApi",
    "ArtifactSessionStatus",
    "ArtifactUploadSession",
    "AvailabilityLocationReport",
    "AvailabilityReport",
    "InAppRepoConfig",
    "LocalArtifactCommit",
    "PersistentRepoClientConfig",
    "PublishOperationHandle",
    "PublishOperationStatus",
    "RepoCompatibilityError",
    "RepoCandidate",
    "RepoCandidateSet",
    "SelectedRepoBinding",
    "SelectedRepoSet",
    "SelectedRepoTarget",
    "WriteConsistency",
    "negotiate_artifact_capabilities",
]
