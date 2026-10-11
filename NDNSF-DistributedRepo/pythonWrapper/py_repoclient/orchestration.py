"""Compatibility imports for the retired Python-owned Repo runtime."""

from .compat import RepoCompatibilityError, raise_repo_compatibility_error, retired_repo_feature

_REPLACEMENT = "use the standalone C++ DistributedRepoServer and native ArtifactRepositoryApi"

_RETIRED_TYPES = {
    "DistributedRepo",
    "ExactPacketRepositoryApi",
    "LocalDistributedRepo",
    "NetworkDistributedRepoClient",
    "PlacementPolicy",
    "RepoIncompleteWriteError",
    "RepoNodeApp",
    "RepoObjectManifest",
    "RepoOperationMetrics",
    "RepoPlacement",
    "RepoRepairAction",
    "RepoWriteIntent",
    "RepoWriteReceipt",
    "StorageCapability",
}

_RETIRED_FUNCTIONS = {
    "configure_repo_object_class_policies",
    "decode_repo_request",
    "encode_repo_request",
    "large_data_reference_from_repo_manifest",
    "normalize_repo_mode",
    "normalize_repo_operation_state",
    "normalize_write_consistency",
    "repo_artifact_reference",
    "repo_manifest_from_artifact_reference",
    "repo_manifest_from_large_data_reference",
    "repo_object_class_policy",
    "required_write_acks",
    "select_replicas",
    "validate_write_receipts",
}

_RETIRED_CONSTANTS = {
    "CATALOG_MERGE_MAX_PULL_BYTES",
    "REPO_METRIC_PHASES",
    "REPO_OPERATION_ID_MAX_BYTES",
    "REPO_OPERATION_STATES",
    "REPO_REASON_GENERATION_CONFLICT",
    "REPO_REASON_INTEGRITY_FAILURE",
    "REPO_REASON_OPERATION_CONFLICT",
    "REPO_REASON_OVERLOADED",
    "REPO_REASON_REPAIR_UNAVAILABLE",
    "REPO_REASON_WRITE_INCOMPLETE",
    "REPO_REJECTION_REASONS",
}


def __getattr__(name: str):
    if name in _RETIRED_TYPES:
        value = retired_repo_feature(name, _REPLACEMENT)
    elif name in _RETIRED_FUNCTIONS:
        def value(*args, **kwargs):
            raise_repo_compatibility_error(name, _REPLACEMENT)
    elif name in _RETIRED_CONSTANTS:
        raise_repo_compatibility_error(name, _REPLACEMENT)
    else:
        raise AttributeError(name)
    globals()[name] = value
    return value


__all__ = sorted(_RETIRED_TYPES | _RETIRED_FUNCTIONS | _RETIRED_CONSTANTS)
