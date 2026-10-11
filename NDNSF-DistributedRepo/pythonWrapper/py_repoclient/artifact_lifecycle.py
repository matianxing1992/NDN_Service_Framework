"""Versioned compatibility errors for retired Python lifecycle helpers."""

from .compat import raise_repo_compatibility_error, retired_repo_feature

AuthenticatedReplicaReceipt = retired_repo_feature(
    "AuthenticatedReplicaReceipt", "use native ArtifactCommitReceipt"
)
ArtifactReplicaSession = retired_repo_feature(
    "ArtifactReplicaSession", "use native ArtifactRepositoryApi and ArtifactFetchResult"
)
AtomicArtifactDestination = retired_repo_feature(
    "AtomicArtifactDestination", "use ArtifactRepositoryApi.fetch_file"
)
HmacReceiptAuthenticator = retired_repo_feature(
    "HmacReceiptAuthenticator", "use controller-authenticated native receipts"
)


def resolve_active_artifact(*args, **kwargs):
    raise_repo_compatibility_error(
        "resolve_active_artifact", "use native ArtifactHandle and check_availability"
    )


def retrieve_to_atomic_destination(*args, **kwargs):
    raise_repo_compatibility_error(
        "retrieve_to_atomic_destination", "use ArtifactRepositoryApi.fetch_file"
    )


__all__ = [
    "AuthenticatedReplicaReceipt",
    "ArtifactReplicaSession",
    "AtomicArtifactDestination",
    "HmacReceiptAuthenticator",
    "resolve_active_artifact",
    "retrieve_to_atomic_destination",
]
