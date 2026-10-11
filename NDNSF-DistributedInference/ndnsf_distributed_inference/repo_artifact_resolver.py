"""DI adapter over the T008 thin native DistributedRepo binding.

This module owns no Repo transport, location, receipt, placement, or storage
behavior. Every fetch is delegated to the native ArtifactRepositoryApi.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
from typing import Any

from .app_sdk.contracts import ArtifactReference, DIRepoArtifactBinding

_ARTIFACT_SLOT_USES = {
    "model": "model",
    "checkpoint": "checkpoint",
    "dataset": "dataset",
    "input": "input",
    "output": "output",
    "runtime": "runtime",
    "runner": "runtime",
}


def validate_binding_scope(
    binding: DIRepoArtifactBinding | dict,
    *,
    role: str,
    artifact_slot: str = "",
) -> DIRepoArtifactBinding:
    if isinstance(binding, dict):
        binding = DIRepoArtifactBinding.from_dict(binding)
    elif not isinstance(binding, DIRepoArtifactBinding):
        raise TypeError("Repo artifact scope requires a DIRepoArtifactBinding")
    if binding.role != str(role):
        raise ValueError("DI Repo binding role does not match its enclosing role")
    expected_use = _ARTIFACT_SLOT_USES.get(str(artifact_slot))
    if expected_use is not None and binding.use != expected_use:
        raise ValueError("DI Repo binding use does not match its artifact slot")
    return binding


def _native_handle(binding: DIRepoArtifactBinding):
    try:
        from py_repoclient.artifact_api import ArtifactHandle
    except ImportError as exc:
        raise RuntimeError(
            "DI_REPO_NATIVE_BINDING_UNAVAILABLE: install the T008 pybind wrapper"
        ) from exc

    handle = ArtifactHandle.from_json(binding.repo_handle_json)
    reference = handle.reference
    native_digest = f"{reference.digest_algorithm}:{reference.content_digest}"
    projection = binding.artifact_reference
    if (native_digest != projection.digest
            or int(reference.size_bytes) != projection.size_bytes):
        raise ValueError("DI Repo binding digest/size does not match native handle")
    return handle


def binding_from_native_handle(
    handle: Any,
    artifact_reference: ArtifactReference,
    *,
    role: str,
    use: str,
) -> DIRepoArtifactBinding:
    """Bind an already-published native handle to DI-owned role metadata."""

    if not callable(getattr(handle, "to_json", None)):
        raise TypeError("binding requires a native ArtifactHandle")
    binding = DIRepoArtifactBinding(
        repo_handle_json=str(handle.to_json()),
        artifact_reference=artifact_reference,
        role=role,
        use=use,
    )
    _native_handle(binding)
    return binding


def reject_legacy_repo_manifest(operation: str) -> None:
    replacement = (
        "publish or archive through the native ArtifactRepositoryApi, then "
        "supply a version-1 DIRepoArtifactBinding"
    )
    try:
        from py_repoclient.compat import raise_repo_compatibility_error
    except ImportError as exc:
        raise RuntimeError(
            f"SPEC192_COMPATIBILITY_V1: {operation} is retired; {replacement}"
        ) from exc
    raise_repo_compatibility_error(operation, replacement)


class DIRepoArtifactResolver:
    """Resolve DI bindings by calling the supplied T008 native Repo client."""

    def __init__(self, repo_client: Any):
        try:
            from py_repoclient import ArtifactRepositoryApi, RepoClient
        except ImportError as exc:
            raise RuntimeError(
                "DI_REPO_NATIVE_BINDING_UNAVAILABLE: install the T008 pybind wrapper"
            ) from exc

        if type(repo_client) is RepoClient:
            if type(repo_client.artifact_api) is not ArtifactRepositoryApi:
                raise TypeError("T008 RepoClient does not own its native ArtifactRepositoryApi")
        elif type(repo_client) is ArtifactRepositoryApi:
            pass
        else:
            raise TypeError(
                "DI Repo resolver requires an exact T008 RepoClient or "
                "ArtifactRepositoryApi instance"
            )
        self._client = repo_client

    def fetch_bytes(
        self,
        binding: DIRepoArtifactBinding | dict,
        *,
        timeout_ms: int = 30000,
    ) -> bytes:
        if isinstance(binding, dict):
            binding = DIRepoArtifactBinding.from_dict(binding)
        elif not isinstance(binding, DIRepoArtifactBinding):
            raise TypeError("Repo artifact resolution requires a DIRepoArtifactBinding")
        if type(timeout_ms) is not int or timeout_ms <= 0:
            raise ValueError("Repo artifact fetch timeout must be positive")

        handle = _native_handle(binding)
        with tempfile.TemporaryDirectory(prefix="ndnsf-di-repo-fetch-") as temp:
            destination = Path(temp) / "artifact.bin"
            self._client.fetch_file(handle, destination, timeout_ms=timeout_ms)
            payload = destination.read_bytes()

        projection = binding.artifact_reference
        digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        if len(payload) != projection.size_bytes or digest != projection.digest:
            raise ValueError("native Repo fetch does not match DI artifact projection")
        return payload


def require_di_repo_resolver(resolver: Any) -> DIRepoArtifactResolver | None:
    if resolver is not None and type(resolver) is not DIRepoArtifactResolver:
        raise TypeError(
            "Repo artifact resolution requires the DIRepoArtifactResolver "
            "over the T008 native Repo API"
        )
    return resolver


__all__ = [
    "DIRepoArtifactResolver",
    "binding_from_native_handle",
    "reject_legacy_repo_manifest",
    "require_di_repo_resolver",
    "validate_binding_scope",
]
