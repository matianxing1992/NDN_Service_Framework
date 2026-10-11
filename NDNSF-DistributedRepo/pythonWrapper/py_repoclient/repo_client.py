"""Compatibility client that delegates artifact operations to the native API."""

from __future__ import annotations

from pathlib import Path
from typing import Union

from .artifact_api import (
    ArtifactHandle,
    ArtifactRepositoryApi,
    PersistentRepoClientConfig,
)
from .compat import raise_repo_compatibility_error
from .service_names import DEFAULT_REPO_SERVICE_ROOT


_NATIVE_API_METHODS = frozenset({
    "check_availability",
    "get_publish_status",
    "get_receipt",
})

_RETIRED_METHODS = frozenset({
    "artifact_capabilities",
    "capability",
    "delete",
    "fetch",
    "fetch_file_async",
    "fetch_object",
    "get",
    "get_object",
    "insert",
    "inventory",
    "list",
    "lookup_local_commit",
    "publisher_namespace",
    "publisher_object_name",
    "publish_file_async",
    "remove",
    "store",
    "_require_publisher_object_name",
    "_service_for",
})

_RETIRED_REPLACEMENT = (
    "use ArtifactRepositoryApi with a native ArtifactHandle and explicit "
    "SelectedRepoSet, or the C++ DistributedRepoServer"
)


def _service_prefix(repo_service_name: str) -> str:
    root = str(repo_service_name).strip().rstrip("/")
    if not root or not root.startswith("/"):
        raise ValueError("repo_service_name must be an absolute NDN name")
    if root.endswith("/Object/v1"):
        return root
    return f"{root}/Object/v1"


class RepoClient:
    """Persistent artifact client backed by one native ArtifactRepositoryApi."""

    def __init__(
        self,
        user,
        repo_service_name: str = DEFAULT_REPO_SERVICE_ROOT,
        *,
        ack_timeout_ms: int = 1000,
        timeout_ms: int = 30000,
        artifact_backend=None,
    ) -> None:
        if artifact_backend is not None:
            raise_repo_compatibility_error(
                "artifact_backend",
                "Python-owned artifact backends were removed; pass an NDNSF "
                "ServiceUser and use the native Persistent Repo service",
            )

        self.user = user
        self.repo_service_name = str(repo_service_name)
        self.ack_timeout_ms = int(ack_timeout_ms)
        self.timeout_ms = int(timeout_ms)

        config = PersistentRepoClientConfig()
        config.service_prefix = _service_prefix(self.repo_service_name)
        config.ack_collection_time_ms = self.ack_timeout_ms
        config.request_timeout_ms = self.timeout_ms
        config.operation_wait_timeout_ms = self.timeout_ms
        self._artifact_api = ArtifactRepositoryApi.make_persistent(user, config)

    @property
    def artifact_api(self) -> ArtifactRepositoryApi:
        return self._artifact_api

    def configure_artifact_backend(self, *args, **kwargs) -> None:
        raise_repo_compatibility_error(
            "configure_artifact_backend",
            "construct RepoClient with an NDNSF ServiceUser and the C++ Repo service",
        )

    def publish_file(
        self,
        path: Union[str, Path],
        reference=None,
        selected=None,
        timeout_ms: int | None = None,
        **legacy_options,
    ):
        if legacy_options or reference is None:
            raise_repo_compatibility_error(
                "publish_file legacy options",
                "pass a native ArtifactReference and explicit SelectedRepoSet",
            )
        if selected is None:
            raise TypeError(
                "Persistent RepoClient.publish_file requires an explicit "
                "native SelectedRepoSet"
            )
        return self._artifact_api.publish_file(
            path,
            reference,
            selected,
            self.timeout_ms if timeout_ms is None else int(timeout_ms),
        )

    def fetch_file(
        self,
        reference_or_handle,
        destination: Union[str, Path],
        timeout_ms: int | None = None,
        **legacy_options,
    ):
        if legacy_options:
            raise_repo_compatibility_error(
                "fetch_file legacy options",
                "use the native ArtifactHandle fetch and its timeout_ms argument",
            )
        if not isinstance(reference_or_handle, ArtifactHandle):
            raise_repo_compatibility_error(
                "fetch_file by reference",
                "pass a native ArtifactHandle from a committed Repo publication",
            )
        return self._artifact_api.fetch_file(
            reference_or_handle,
            destination,
            self.timeout_ms if timeout_ms is None else int(timeout_ms),
        )

    def begin_publish_file(
        self,
        path: Union[str, Path],
        reference,
        selected,
        idempotency_key: str,
        timeout_ms: int | None = None,
    ):
        return self._artifact_api.begin_publish_file(
            path,
            reference,
            selected,
            idempotency_key,
            self.timeout_ms if timeout_ms is None else int(timeout_ms),
        )

    def begin_archive(
        self,
        in_app_source: ArtifactRepositoryApi,
        handle: ArtifactHandle,
        selected,
        idempotency_key: str,
        timeout_ms: int | None = None,
    ):
        return self._artifact_api.begin_archive(
            in_app_source,
            handle,
            selected,
            idempotency_key,
            self.timeout_ms if timeout_ms is None else int(timeout_ms),
        )

    def discover_repo_candidates(
        self, requirements, timeout_ms: int | None = None
    ):
        return self._artifact_api.discover_repo_candidates(
            requirements,
            self.timeout_ms if timeout_ms is None else int(timeout_ms),
        )

    def archive(
        self,
        in_app_source: ArtifactRepositoryApi,
        handle: ArtifactHandle,
        selected,
        timeout_ms: int | None = None,
    ):
        return self._artifact_api.archive(
            in_app_source,
            handle,
            selected,
            self.timeout_ms if timeout_ms is None else int(timeout_ms),
        )

    @staticmethod
    def make_manifest(*args, **kwargs):
        raise_repo_compatibility_error(
            "make_manifest",
            "use native ArtifactReference and ArtifactRepositoryApi",
        )

    def __getattr__(self, name: str):
        if name in _NATIVE_API_METHODS:
            return getattr(self._artifact_api, name)
        if name in _RETIRED_METHODS or name in {"begin_upload", "begin_fetch"}:
            replacement = _RETIRED_REPLACEMENT
            if name == "lookup_local_commit":
                replacement = (
                    "use ArtifactRepositoryApi.make_in_app(...).lookup_local_commit"
                )
            raise_repo_compatibility_error(name, replacement)
        raise AttributeError(name)


__all__ = ["RepoClient"]
