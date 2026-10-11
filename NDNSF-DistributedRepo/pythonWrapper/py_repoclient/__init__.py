"""Python API for the C++ Distributed Repo and artifact repository."""

from __future__ import annotations

from . import _py_repoclient as _native
from .compat import RepoCompatibilityError, retired_repo_feature

# Keep the native value and protocol bindings as the source of truth. The
# Python layer below adds Path conversion and retains selected legacy imports
# only to raise the versioned compatibility error.
for _name in dir(_native):
    if not _name.startswith("_") and _name != "NativeArtifactRepositoryApi":
        globals()[_name] = getattr(_native, _name)

from .artifact_api import (
    ArtifactApiBackend,
    ArtifactApiError,
    ArtifactCancellationToken,
    ArtifactCapabilityNegotiation,
    ArtifactCapabilityRejection,
    ArtifactCapabilityRequirements,
    ArtifactCommitReceipt,
    ArtifactDescriptor,
    ArtifactErrorCode,
    ArtifactFetchAttempt,
    ArtifactFetchDriver,
    ArtifactFetchResult,
    ArtifactFetchSession,
    ArtifactHandle,
    ArtifactProgress,
    ArtifactPublishDriver,
    ArtifactPublishResult,
    ArtifactReplicaCommit,
    ArtifactReplicaResult,
    ArtifactRepositoryApi,
    ArtifactSessionStatus,
    ArtifactUploadSession,
    AvailabilityLocationReport,
    AvailabilityReport,
    InAppRepoConfig,
    LocalArtifactCommit,
    PersistentRepoClientConfig,
    PublishOperationHandle,
    PublishOperationStatus,
    RepoCandidate,
    RepoCandidateSet,
    SelectedRepoBinding,
    SelectedRepoSet,
    SelectedRepoTarget,
    WriteConsistency,
    negotiate_artifact_capabilities,
)
from .artifact_lifecycle import (
    AuthenticatedReplicaReceipt,
    ArtifactReplicaSession,
    AtomicArtifactDestination,
    HmacReceiptAuthenticator,
    resolve_active_artifact,
    retrieve_to_atomic_destination,
)
from .artifact_transfer import (
    ArtifactStoreAssignment,
    ArtifactStoreOffer,
    PendingReplicaLeaseCollaboration,
    PendingReplicaTaskCollaboration,
    ReplicaLeaseCollaborationClient,
    ReplicaTaskCollaborationClient,
    ReplicaTaskControlSnapshot,
    decode_store_assignment,
    decode_store_offer_ack,
    decode_upload_lease_assignment,
    encode_store_offer_ack,
    encode_upload_lease_ack,
)
from .local_artifact_backend import FilesystemArtifactApiBackend
from .network_artifact_backend import (
    ArtifactControlMetrics,
    CollaborationArtifactApiBackend,
    install_artifact_collaboration_service,
)
from .orchestration import NetworkDistributedRepoClient, RepoNodeApp
from .persistence import (
    ArtifactCapacityStatus,
    ArtifactFinalizationRecord,
    ArtifactStorageIdentity,
    ArtifactTransferSessionRecord,
    FilesystemCasPayloadStore,
    LifecycleTransitionError,
    MetadataStore,
    PayloadStore,
    PersistenceOwnershipError,
    RepoLifecycleEvent,
    SqliteRepositoryPersistence,
)
from .service_names import (
    DEFAULT_REPO_SERVICE_ROOT,
    canonical_repo_operation,
    is_internal_repo_service,
    repo_service_for_operation,
    repo_versioned_services,
)
from .repo_client import RepoClient

RepoDataPlaneProducer = retired_repo_feature(
    "RepoDataPlaneProducer",
    "use the C++ Repo server; Python callbacks are not invoked on the Face thread",
)

# These import names belonged to the Python-owned discovery and selection
# implementation. Keep imports stable while making calls fail explicitly.
for _name in (
    "artifact_capability_from_ack",
    "capability_from_ack",
    "ready_capability_from_ack",
    "discovery_record_from_ack",
):
    globals()[_name] = retired_repo_feature(
        _name,
        "use ArtifactRepositoryApi candidate discovery and native selection",
    )

__all__ = sorted(
    name
    for name in globals()
    if not name.startswith("_") and name != "retired_repo_feature"
)
