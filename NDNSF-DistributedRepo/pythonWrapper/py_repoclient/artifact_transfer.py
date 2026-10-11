"""Compatibility names for retired Python-owned transfer sessions."""

from .compat import raise_repo_compatibility_error, retired_repo_feature

ArtifactStoreAssignment = retired_repo_feature(
    "ArtifactStoreAssignment", "use native SelectedRepoTarget and SelectedRepoSet"
)
ArtifactStoreOffer = retired_repo_feature(
    "ArtifactStoreOffer", "use ArtifactRepositoryApi.discover_repo_candidates"
)
ReplicaTaskControlSnapshot = retired_repo_feature(
    "ReplicaTaskControlSnapshot", "use native PublishOperationStatus"
)
PendingReplicaTaskCollaboration = retired_repo_feature(
    "PendingReplicaTaskCollaboration", "use native ArtifactRepositoryApi operations"
)
ReplicaTaskCollaborationClient = retired_repo_feature(
    "ReplicaTaskCollaborationClient", "use ArtifactRepositoryApi.make_persistent"
)
PendingReplicaLeaseCollaboration = retired_repo_feature(
    "PendingReplicaLeaseCollaboration", "use native ArtifactRepositoryApi operations"
)
ReplicaLeaseCollaborationClient = retired_repo_feature(
    "ReplicaLeaseCollaborationClient", "use ArtifactRepositoryApi.make_persistent"
)


def _retired(operation: str, *args, **kwargs):
    raise_repo_compatibility_error(
        operation, "use native ArtifactRepositoryApi and its bound value types"
    )


def encode_upload_lease_ack(*args, **kwargs):
    return _retired("encode_upload_lease_ack", *args, **kwargs)


def decode_upload_lease_assignment(*args, **kwargs):
    return _retired("decode_upload_lease_assignment", *args, **kwargs)


def encode_store_offer_ack(*args, **kwargs):
    return _retired("encode_store_offer_ack", *args, **kwargs)


def decode_store_offer_ack(*args, **kwargs):
    return _retired("decode_store_offer_ack", *args, **kwargs)


def decode_store_assignment(*args, **kwargs):
    return _retired("decode_store_assignment", *args, **kwargs)


__all__ = [
    "ArtifactStoreAssignment",
    "ArtifactStoreOffer",
    "PendingReplicaLeaseCollaboration",
    "PendingReplicaTaskCollaboration",
    "ReplicaLeaseCollaborationClient",
    "ReplicaTaskCollaborationClient",
    "ReplicaTaskControlSnapshot",
    "decode_store_assignment",
    "decode_store_offer_ack",
    "decode_upload_lease_assignment",
    "encode_store_offer_ack",
    "encode_upload_lease_ack",
]
