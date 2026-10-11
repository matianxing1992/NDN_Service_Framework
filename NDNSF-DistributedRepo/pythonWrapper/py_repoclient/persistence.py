"""Compatibility names for retired Python-owned persistence state."""

from .compat import RepoCompatibilityError, raise_repo_compatibility_error, retired_repo_feature

ArtifactStorageIdentity = retired_repo_feature(
    "ArtifactStorageIdentity", "use native ArtifactReference and ArtifactHandle"
)
ArtifactTransferSessionRecord = retired_repo_feature(
    "ArtifactTransferSessionRecord", "use native PublishOperationStatus"
)
ArtifactFinalizationRecord = retired_repo_feature(
    "ArtifactFinalizationRecord", "use native ArtifactCommitReceipt"
)
ArtifactCapacityStatus = retired_repo_feature(
    "ArtifactCapacityStatus", "use native ArtifactCapability"
)
FilesystemCasPayloadStore = retired_repo_feature(
    "FilesystemCasPayloadStore", "use ArtifactRepositoryApi.make_in_app"
)
MetadataStore = retired_repo_feature(
    "MetadataStore", "use the native Repo server and artifact-v2 catalog"
)
PayloadStore = retired_repo_feature(
    "PayloadStore", "use the native Repo server or ArtifactRepositoryApi"
)
RepoLifecycleEvent = retired_repo_feature(
    "RepoLifecycleEvent", "use native PublishOperationStatus and ArtifactCommitReceipt"
)
SqliteRepositoryPersistence = retired_repo_feature(
    "SqliteRepositoryPersistence", "use the native C++ Repo server"
)


class PersistenceOwnershipError(RepoCompatibilityError):
    def __init__(self, *args, **kwargs):
        super().__init__("PersistenceOwnershipError", "use native Repo ownership")


class LifecycleTransitionError(RepoCompatibilityError):
    def __init__(self, *args, **kwargs):
        super().__init__("LifecycleTransitionError", "use native PublishOperationStatus")


_RETIRED_CONSTANTS = {
    "ARTIFACT_LIFECYCLE_STATES",
    "ARTIFACT_LIFECYCLE_TRANSITIONS",
    "REPO_PERSISTENCE_OWNED",
    "REPO_PERSISTENCE_CLOSED",
    "REPO_LIFECYCLE_INVALID_STATE",
    "REPO_LIFECYCLE_STATE_CONFLICT",
    "REPO_LIFECYCLE_IDENTITY_CONFLICT",
    "REPO_LIFECYCLE_EVENT_CONFLICT",
    "REPO_LIFECYCLE_ILLEGAL_TRANSITION",
    "REPO_ARTIFACT_INVALID_IDENTITY",
    "REPO_ARTIFACT_RANGE_OUT_OF_BOUNDS",
    "REPO_ARTIFACT_INCOMPLETE",
    "REPO_ARTIFACT_DIGEST_MISMATCH",
    "REPO_ARTIFACT_WRITES_DISABLED",
}


def __getattr__(name: str):
    if name in _RETIRED_CONSTANTS:
        raise_repo_compatibility_error(
            name, "use native ArtifactRepositoryApi or the C++ Repo server"
        )
    raise AttributeError(name)


__all__ = [
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
] + sorted(_RETIRED_CONSTANTS)
