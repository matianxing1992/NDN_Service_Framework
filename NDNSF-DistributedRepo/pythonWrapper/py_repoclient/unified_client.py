"""Typed host-side helpers for the unified Repo client (Spec 240).

The native module is authoritative: these helpers only translate dictionaries into
small immutable records so callers do not index strings by hand. They never retry,
re-sign or second-guess a native decision.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Sequence

def _native():
    """Resolve the extension lazily: importing it at module scope can register the
    same pybind types twice when a host has both the build copy and an installed
    package copy in the same process."""
    try:
        import _py_repoclient as module  # build tree: top-level extension
    except ImportError:
        from . import _py_repoclient as module  # installed package layout
    return module


@dataclass(frozen=True)
class RepoReference:
    logical_name: str
    content_digest: str
    size_bytes: int
    format_version: str = "artifact-manifest-v2"
    digest_algorithm: str = "sha256"
    root_manifest_name: str = ""
    publisher_identity: str = ""
    policy_epoch: str = ""

    def to_native(self) -> dict:
        return {
            "logical_name": self.logical_name,
            "content_digest": self.content_digest,
            "size_bytes": self.size_bytes,
            "format_version": self.format_version,
            "digest_algorithm": self.digest_algorithm,
            "root_manifest_name": self.root_manifest_name,
            "publisher_identity": self.publisher_identity,
            "policy_epoch": self.policy_epoch,
        }


@dataclass(frozen=True)
class RepoPublishResult:
    state: str
    reference: RepoReference | None
    receipt_ids: Sequence[str] = ()
    operation_reference: str | None = None
    deduplicated: bool = False
    resumed: bool = False

    @property
    def committed(self) -> bool:
        return self.state == "COMMITTED"


@dataclass(frozen=True)
class RepoFetchResult:
    bytes_written: int
    digest_verified: bool
    content_digest: str = ""
    digest_algorithm: str = "sha256"
    attempted_sources: Sequence[str] = ()


@dataclass(frozen=True)
class RepoInspection:
    availability: str
    stored_bytes: int = 0
    lifecycle_state: str = ""

    @property
    def available(self) -> bool:
        return self.availability == "AVAILABLE"


def _reference_dict(reference: RepoReference | dict) -> dict:
    return reference.to_native() if isinstance(reference, RepoReference) else dict(reference)


def publish_file(
    durable_root: str,
    owner_id: str,
    source_path: str,
    reference: RepoReference | dict,
    timeout_ms: int = 60000,
) -> RepoPublishResult:
    result = _native().unified_publish_file(
        durable_root, owner_id, source_path, _reference_dict(reference), timeout_ms
    )
    native_reference = result.get("reference")
    return RepoPublishResult(
        state=result["state"],
        reference=(RepoReference(
            logical_name=native_reference["logical_name"],
            content_digest=native_reference["content_digest"],
            size_bytes=int(native_reference["size_bytes"]),
            format_version=native_reference.get("format_version", "artifact-manifest-v2"),
            digest_algorithm=native_reference.get("digest_algorithm", "sha256"),
            root_manifest_name=native_reference.get("root_manifest_name", ""),
            publisher_identity=native_reference.get("publisher_identity", ""),
            policy_epoch=native_reference.get("policy_epoch", ""),
        ) if native_reference else None),
        receipt_ids=tuple(result.get("receipt_ids", ())),
        operation_reference=result.get("operation_reference"),
        deduplicated=bool(result.get("deduplicated")),
        resumed=bool(result.get("resumed")),
    )


def fetch_file(
    durable_root: str,
    owner_id: str,
    reference: RepoReference | dict,
    target_path: str,
    timeout_ms: int = 60000,
) -> RepoFetchResult:
    result = _native().unified_fetch_file(
        durable_root, owner_id, _reference_dict(reference), target_path, timeout_ms
    )
    return RepoFetchResult(
        bytes_written=int(result.get("bytes_written", 0)),
        digest_verified=bool(result.get("digest_verified")),
        content_digest=result.get("content_digest", ""),
        digest_algorithm=result.get("digest_algorithm", "sha256"),
        attempted_sources=tuple(result.get("attempted_sources", ())),
    )


def inspect_object(
    durable_root: str,
    owner_id: str,
    reference: RepoReference | dict,
    timeout_ms: int = 60000,
) -> RepoInspection:
    result = _native().unified_inspect_object(
        durable_root, owner_id, _reference_dict(reference), timeout_ms
    )
    return RepoInspection(
        availability=result.get("availability", "UNKNOWN"),
        stored_bytes=int(result.get("stored_bytes", 0)),
        lifecycle_state=result.get("lifecycle_state", ""),
    )
