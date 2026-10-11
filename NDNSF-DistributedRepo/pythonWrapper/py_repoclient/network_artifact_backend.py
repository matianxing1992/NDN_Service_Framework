"""Compatibility names for retired Python-owned Repo server behavior."""

from .compat import raise_repo_compatibility_error, retired_repo_feature

ArtifactControlMetrics = retired_repo_feature(
    "ArtifactControlMetrics", "use native PublishOperationStatus and ArtifactFetchResult"
)
CollaborationArtifactApiBackend = retired_repo_feature(
    "CollaborationArtifactApiBackend",
    "use ArtifactRepositoryApi.make_persistent with an authorized ServiceUser",
)


def install_artifact_collaboration_service(*args, **kwargs):
    raise_repo_compatibility_error(
        "install_artifact_collaboration_service",
        "start the standalone C++ DistributedRepoServer",
    )


__all__ = [
    "ArtifactControlMetrics",
    "CollaborationArtifactApiBackend",
    "install_artifact_collaboration_service",
]
