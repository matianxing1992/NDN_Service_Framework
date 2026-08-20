"""Immutable operator-managed pre-split catalog and repository publication."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import time
from types import MappingProxyType
from typing import Any, Callable, Mapping, Sequence

from .contracts import PreSplitArtifactInput, PreSplitCatalogSnapshot
from ..splitter import ModelGraphSnapshot, SplitCandidate, SplitterOutput


def _digest(payload: bytes) -> str:
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _canonical(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode("utf-8")


def _snapshot_values(
    snapshot: PreSplitCatalogSnapshot,
) -> dict[str, Any]:
    return {
        "alias": snapshot.alias,
        "manifest_digest": snapshot.manifest_digest,
        "model_content_digest": snapshot.model_content_digest,
        "semantics_digest": snapshot.semantics_digest,
        "graph_digest": snapshot.graph_digest,
        "candidate_digest": snapshot.candidate_digest,
        "backend": snapshot.backend,
        "precision": snapshot.precision,
        "artifact_data_names": snapshot.artifact_data_names,
        "status": snapshot.status,
        "created_at_ms": snapshot.created_at_ms,
    }


@dataclass(frozen=True)
class PreSplitAuditEvent:
    sequence: int
    event: str
    alias: str
    manifest_digest: str
    at_ms: int
    evidence: Mapping[str, Any]

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "evidence", MappingProxyType(dict(self.evidence)))


@dataclass
class _StagingRecord:
    alias: str
    manifest_digest: str
    object_names: tuple[str, ...]
    staged_at_ms: int


class PreSplitCatalog:
    """Activates a manifest only after every segment is verified and published."""

    def __init__(
        self,
        *,
        repository: Any,
        repository_prefix: str,
        verify_artifact: Callable[[PreSplitArtifactInput, bytes], bool],
        sign_manifest: Callable[[bytes], tuple[str, str]],
        verify_manifest: Callable[[str, bytes, str], bool],
    ) -> None:
        if not repository_prefix.startswith("/"):
            raise ValueError("repository prefix must be an NDN name")
        self._repository = repository
        self._prefix = repository_prefix.rstrip("/")
        self._verify_artifact = verify_artifact
        self._sign_manifest = sign_manifest
        self._verify_manifest = verify_manifest
        self._by_alias: dict[str, PreSplitCatalogSnapshot] = {}
        self._by_digest: dict[str, PreSplitCatalogSnapshot] = {}
        self._staging: dict[str, _StagingRecord] = {}
        self._audit: list[PreSplitAuditEvent] = []

    def register(
        self,
        *,
        alias: str,
        splitter_output: SplitterOutput,
        graph: ModelGraphSnapshot,
        candidate: SplitCandidate,
        artifacts: Sequence[PreSplitArtifactInput],
        backend: str,
        precision: str,
        at_ms: int | None = None,
    ) -> PreSplitCatalogSnapshot:
        if not alias or "/" in alias or not backend or not precision:
            raise ValueError("pre-split registration identity is incomplete")
        if not isinstance(splitter_output, SplitterOutput):
            raise TypeError("registration requires validated SplitterOutput")
        candidate.validate_against(graph)
        self._validate_splitter_output(splitter_output, candidate, artifacts)
        now = int(time.time() * 1000) if at_ms is None else int(at_ms)

        payloads: dict[str, bytes] = {}
        artifact_records: list[dict[str, Any]] = []
        names_by_role: dict[str, list[str]] = {}
        for artifact in sorted(
                artifacts, key=lambda item: (item.role, item.digest)):
            payload = Path(artifact.path).read_bytes()
            if len(payload) != artifact.size_bytes:
                raise ValueError("pre-split artifact size mismatch")
            if _digest(payload) != artifact.digest:
                raise ValueError("pre-split artifact digest mismatch")
            if not self._verify_artifact(artifact, payload):
                raise PermissionError(
                    "pre-split artifact signer/trust verification failed")
            object_name = (
                f"{self._prefix}/segments/{artifact.digest[7:]}")
            payloads[object_name] = payload
            names_by_role.setdefault(artifact.role, []).append(object_name)
            artifact_records.append({
                "role": artifact.role,
                "artifact_name": artifact.artifact_name,
                "digest": artifact.digest,
                "size_bytes": artifact.size_bytes,
                "signer_key_id": artifact.signer_key_id,
                "data_name": object_name,
            })

        manifest_body = {
            "schema": "ndnsf-di-presplit-manifest-v2",
            "model_name": candidate.model.model_name,
            "model_content_digest": candidate.model.content_digest,
            "semantics_digest": candidate.model.semantics_digest,
            "graph_digest": graph.graph_digest,
            "candidate_digest": candidate.candidate_digest,
            "backend": backend,
            "precision": precision,
            "artifacts": artifact_records,
        }
        unsigned = _canonical(manifest_body)
        manifest_digest = _digest(unsigned)
        existing = self._by_alias.get(alias)
        if existing is not None:
            if existing.manifest_digest != manifest_digest:
                raise ValueError("pre-split alias already binds different content")
            return existing
        digest_existing = self._by_digest.get(manifest_digest)
        if digest_existing is not None:
            snapshot = PreSplitCatalogSnapshot(
                **{
                    **_snapshot_values(digest_existing),
                    "alias": alias,
                    "artifact_data_names":
                        digest_existing.artifact_data_names,
                }
            )
            self._by_alias[alias] = snapshot
            self._record("ALIAS_BOUND", snapshot, now, {})
            return snapshot

        manifest_name = (
            f"{self._prefix}/manifests/{manifest_digest[7:]}")
        staged_names = tuple(payloads) + (manifest_name,)
        self._staging[manifest_digest] = _StagingRecord(
            alias, manifest_digest, staged_names, now)
        try:
            for object_name, payload in payloads.items():
                self._repository.publish_segment(
                    object_name, payload, _digest(payload))
            signer_key_id, signature = self._sign_manifest(unsigned)
            if (not signer_key_id or not signature or
                    not self._verify_manifest(
                        signer_key_id, unsigned, signature)):
                raise PermissionError(
                    "pre-split manifest signer/trust verification failed")
            signed_manifest = _canonical({
                **manifest_body,
                "manifest_digest": manifest_digest,
                "signer_key_id": signer_key_id,
                "signature": signature,
            })
            self._repository.activate_manifest(
                manifest_name, signed_manifest, manifest_digest)
        except Exception:
            self._record_raw(
                "STAGING_FAILED", alias, manifest_digest, now,
                {"objects": staged_names})
            raise

        snapshot = PreSplitCatalogSnapshot(
            alias=alias,
            manifest_digest=manifest_digest,
            model_content_digest=candidate.model.content_digest,
            semantics_digest=candidate.model.semantics_digest,
            graph_digest=graph.graph_digest,
            candidate_digest=candidate.candidate_digest,
            backend=backend,
            precision=precision,
            artifact_data_names={
                role: tuple(names)
                for role, names in names_by_role.items()
            },
            status="ACTIVE",
            created_at_ms=now,
        )
        self._by_alias[alias] = snapshot
        self._by_digest[manifest_digest] = snapshot
        self._staging.pop(manifest_digest, None)
        self._record("ACTIVATED", snapshot, now, {
            "manifest_name": manifest_name,
        })
        return snapshot

    @staticmethod
    def _validate_splitter_output(
        output: SplitterOutput,
        candidate: SplitCandidate,
        artifacts: Sequence[PreSplitArtifactInput],
    ) -> None:
        expected_roles = set(candidate.execution_plan.roles)
        output_roles = {
            role for service in output.services for role in service.roles
        }
        output_artifacts = {
            artifact.role
            for service in output.services
            for artifact in service.artifacts
        }
        supplied_roles = {artifact.role for artifact in artifacts}
        if output_roles != expected_roles:
            raise ValueError("SplitterOutput role coverage mismatch")
        if output_artifacts != expected_roles or supplied_roles != expected_roles:
            raise ValueError("SplitterOutput artifact coverage mismatch")
        for artifact in artifacts:
            if artifact.digest not in candidate.artifacts_by_role[artifact.role]:
                raise ValueError(
                    "SplitterOutput artifact digest does not match candidate")
        if len(artifacts) != len({
                (item.role, item.digest) for item in artifacts}):
            raise ValueError("duplicate pre-split artifact")

    def snapshot(self) -> tuple[PreSplitCatalogSnapshot, ...]:
        """Return an immutable strategy view with no Provider residency claims."""
        return tuple(sorted(
            (entry for entry in self._by_alias.values()
             if entry.status == "ACTIVE"),
            key=lambda item: (item.alias, item.manifest_digest),
        ))

    def retire(
        self, alias: str, *, revoke: bool = False,
        at_ms: int | None = None,
    ) -> PreSplitCatalogSnapshot:
        current = self._by_alias.get(alias)
        if current is None:
            raise KeyError(alias)
        if current.status != "ACTIVE":
            return current
        now = int(time.time() * 1000) if at_ms is None else int(at_ms)
        status = "REVOKED" if revoke else "RETIRED"
        # Propagation is deliberately ordered before the local catalog stops
        # offering this manifest to a new preparation.
        self._repository.publish_revocation(
            alias, current.manifest_digest, status)
        updated = PreSplitCatalogSnapshot(
            **{
                **_snapshot_values(current),
                "artifact_data_names": current.artifact_data_names,
                "status": status,
            }
        )
        self._by_alias[alias] = updated
        self._by_digest[current.manifest_digest] = updated
        self._record(status, updated, now, {})
        return updated

    def cleanup_staging(
        self, *, now_ms: int | None = None, max_age_ms: int,
    ) -> tuple[str, ...]:
        if max_age_ms < 0:
            raise ValueError("staging cleanup bound must be non-negative")
        now = int(time.time() * 1000) if now_ms is None else int(now_ms)
        cleaned = []
        for digest, record in tuple(self._staging.items()):
            if now - record.staged_at_ms < max_age_ms:
                continue
            for object_name in record.object_names:
                self._repository.delete_staging(object_name)
            self._staging.pop(digest, None)
            cleaned.append(digest)
            self._record_raw(
                "STAGING_CLEANED", record.alias, digest, now,
                {"objects": record.object_names})
        return tuple(sorted(cleaned))

    def audit_evidence(self) -> tuple[PreSplitAuditEvent, ...]:
        return tuple(self._audit)

    def _record(
        self, event: str, snapshot: PreSplitCatalogSnapshot,
        at_ms: int, evidence: Mapping[str, Any],
    ) -> None:
        self._record_raw(
            event, snapshot.alias, snapshot.manifest_digest, at_ms, evidence)

    def _record_raw(
        self, event: str, alias: str, manifest_digest: str,
        at_ms: int, evidence: Mapping[str, Any],
    ) -> None:
        self._audit.append(PreSplitAuditEvent(
            len(self._audit) + 1, event, alias, manifest_digest,
            at_ms, evidence,
        ))


__all__ = ["PreSplitAuditEvent", "PreSplitCatalog"]
