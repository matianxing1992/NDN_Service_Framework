"""Python API for the C++ Distributed Repo and artifact repository."""

from __future__ import annotations

from . import _py_repoclient as _native
from .compat import RepoCompatibilityError

# Keep the native value and protocol bindings as the source of truth. The
# Python layer below adds Path conversion for installed native operations.
for _name in dir(_native):
    if not _name.startswith("_") and _name != "NativeArtifactRepositoryApi":
        globals()[_name] = getattr(_native, _name)

from .artifact_api import (
    ArtifactApiError,
    ArtifactCapabilityRequirements,
    ArtifactCommitReceipt,
    ArtifactFetchAttempt,
    ArtifactFetchResult,
    ArtifactHandle,
    ArtifactPublishResult,
    ArtifactReplicaCommit,
    ArtifactReplicaResult,
    ArtifactRepositoryApi,
    AvailabilityLocationReport,
    AvailabilityReport,
    InAppRepoConfig,
    PersistentRepoClientConfig,
    PublishOperationHandle,
    PublishOperationStatus,
    RepoCandidate,
    RepoCandidateSet,
    SelectedRepoSet,
    SelectedRepoTarget,
)
from .service_names import (
    DEFAULT_REPO_SERVICE_ROOT,
    canonical_repo_operation,
    is_internal_repo_service,
    repo_service_for_operation,
    repo_versioned_services,
)
from .repo_client import RepoClient

__all__ = sorted(name for name in globals() if not name.startswith("_"))
