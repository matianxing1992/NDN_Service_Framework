"""Trusted single-replica artifact commit, activation, and atomic retrieval."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import hmac
import json
import os
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

from ._py_repoclient import (
    ArtifactResumeSession,
    ArtifactCapability,
    ArtifactChunk,
    ArtifactLimits,
    ArtifactManifestPage,
    ArtifactManifestTrustPolicy,
    ArtifactReference,
    ArtifactReplicaReceipt,
    ArtifactUploadLease,
    SignedArtifactRoot,
    artifact_reference_from_dict,
    artifact_replica_receipt_from_dict,
    artifact_resume_identity_from_dict,
    artifact_upload_lease_from_dict,
    verify_artifact_chunk_payload,
    verify_artifact_manifest_graph,
)
from .persistence import (
    ArtifactStorageIdentity,
    FilesystemCasPayloadStore,
    LifecycleTransitionError,
    SqliteRepositoryPersistence,
)


RECEIPT_SCHEMA = "ndnsf-repo-authenticated-replica-receipt-v1"
RECEIPT_AUTHENTICATION_ALGORITHM = "hmac-sha256"
REPO_RECEIPT_AUTHENTICATION_FAILED = "repo-receipt-authentication-failed"
REPO_ARTIFACT_NOT_ACTIVE = "repo-artifact-not-active"
REPO_ATOMIC_DESTINATION_CONFLICT = "repo-atomic-destination-conflict"
REPO_ATOMIC_DESTINATION_INCOMPLETE = "repo-atomic-destination-incomplete"
REPO_RESUME_IDENTITY_CONFLICT = "repo-resume-identity-conflict"


def _artifact_dict(artifact: ArtifactReference) -> dict[str, Any]:
    return {
        "logicalName": artifact.logical_name,
        "digestAlgorithm": artifact.digest_algorithm,
        "contentDigest": artifact.content_digest,
        "sizeBytes": int(artifact.size_bytes),
        "formatVersion": artifact.format_version,
        "rootManifestName": artifact.root_manifest_name,
        "publisherIdentity": artifact.publisher_identity,
        "policyEpoch": artifact.policy_epoch,
    }


def _receipt_dict(receipt: ArtifactReplicaReceipt) -> dict[str, Any]:
    return {
        "receiptId": receipt.receipt_id,
        "operationId": receipt.operation_id,
        "repoNode": receipt.repo_node,
        "artifact": _artifact_dict(receipt.artifact),
        "committedAtMs": int(receipt.committed_at_ms),
        "storageGeneration": int(receipt.storage_generation),
        "policyEpoch": receipt.policy_epoch,
        "state": receipt.state,
    }


def _lease_dict(lease: ArtifactUploadLease) -> dict[str, Any]:
    return {
        "leaseId": lease.lease_id,
        "operationId": lease.operation_id,
        "repoNode": lease.repo_node,
        "artifact": _artifact_dict(lease.artifact),
        "reservedBytes": int(lease.reserved_bytes),
        "issuedAtMs": int(lease.issued_at_ms),
        "expiresAtMs": int(lease.expires_at_ms),
        "replayId": lease.replay_id,
    }


def _resume_identity_dict(
    artifact: ArtifactReference,
    signed_root: SignedArtifactRoot,
) -> dict[str, Any]:
    return {
        "artifact": _artifact_dict(artifact),
        "manifestRootDigest": signed_root.root.manifest_root_digest,
        "packetPayloadBytes": int(signed_root.root.packet_payload_bytes),
        "chunkBytes": int(signed_root.root.chunk_bytes),
    }


def _canonical_json(value: dict[str, Any]) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


@dataclass(frozen=True)
class AuthenticatedReplicaReceipt:
    receipt: ArtifactReplicaReceipt
    signer_key_id: str
    authentication_algorithm: str
    signature: bytes

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": RECEIPT_SCHEMA,
            "receipt": _receipt_dict(self.receipt),
            "signerKeyId": self.signer_key_id,
            "authenticationAlgorithm": self.authentication_algorithm,
            "signatureHex": bytes(self.signature).hex(),
        }

    def to_bytes(self) -> bytes:
        return _canonical_json(self.to_dict())

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "AuthenticatedReplicaReceipt":
        value = dict(value)
        if value.pop("schema", None) != RECEIPT_SCHEMA or set(value) != {
            "receipt",
            "signerKeyId",
            "authenticationAlgorithm",
            "signatureHex",
        }:
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: invalid receipt envelope"
            )
        try:
            signature = bytes.fromhex(str(value["signatureHex"]))
        except ValueError as exc:
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: invalid signature encoding"
            ) from exc
        return cls(
            receipt=artifact_replica_receipt_from_dict(dict(value["receipt"])),
            signer_key_id=str(value["signerKeyId"]),
            authentication_algorithm=str(value["authenticationAlgorithm"]),
            signature=signature,
        )

    @classmethod
    def from_bytes(cls, wire: bytes) -> "AuthenticatedReplicaReceipt":
        try:
            value = json.loads(bytes(wire).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: malformed receipt envelope"
            ) from exc
        if not isinstance(value, dict):
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: receipt must be an object"
            )
        return cls.from_dict(value)


class HmacReceiptAuthenticator:
    """Repository-identity-bound MAC for receipts inside protected NDNSF flows.

    The key is provisioned by the same authorization domain that validates the
    repository's NDNSF identity. It is not serialized into a receipt or catalog.
    """

    def __init__(self, repo_node: str, key_id: str, key: bytes) -> None:
        self.repo_node = str(repo_node).strip()
        self.key_id = str(key_id).strip()
        self._key = bytes(key)
        if (
            not self.repo_node.startswith("/")
            or not self.key_id
            or len(self._key) < 32
        ):
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: "
                "repository identity, key id, and 256-bit key are required"
            )

    @staticmethod
    def _signed_bytes(receipt: ArtifactReplicaReceipt) -> bytes:
        return _canonical_json({
            "schema": RECEIPT_SCHEMA,
            "receipt": _receipt_dict(receipt),
        })

    def sign(self, receipt: ArtifactReplicaReceipt) -> AuthenticatedReplicaReceipt:
        if receipt.repo_node != self.repo_node:
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: signer identity mismatch"
            )
        signature = hmac.new(
            self._key, self._signed_bytes(receipt), hashlib.sha256
        ).digest()
        return AuthenticatedReplicaReceipt(
            receipt=receipt,
            signer_key_id=self.key_id,
            authentication_algorithm=RECEIPT_AUTHENTICATION_ALGORITHM,
            signature=signature,
        )

    def verify(
        self,
        envelope: AuthenticatedReplicaReceipt,
        *,
        expected_artifact: ArtifactReference | None = None,
        expected_operation_id: str = "",
    ) -> ArtifactReplicaReceipt:
        receipt = envelope.receipt
        if (
            envelope.signer_key_id != self.key_id
            or envelope.authentication_algorithm
            != RECEIPT_AUTHENTICATION_ALGORITHM
            or receipt.repo_node != self.repo_node
        ):
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: authentication binding mismatch"
            )
        expected = hmac.new(
            self._key, self._signed_bytes(receipt), hashlib.sha256
        ).digest()
        if not hmac.compare_digest(expected, bytes(envelope.signature)):
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: signature mismatch"
            )
        if expected_operation_id and receipt.operation_id != expected_operation_id:
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: operation mismatch"
            )
        if expected_artifact is not None and _artifact_dict(receipt.artifact) != (
            _artifact_dict(expected_artifact)
        ):
            raise ValueError(
                f"{REPO_RECEIPT_AUTHENTICATION_FAILED}: artifact mismatch"
            )
        return receipt


class ArtifactReplicaSession:
    """Compose manifest trust, bounded chunk writes, durable commit, and activation."""

    def __init__(
        self,
        *,
        persistence: SqliteRepositoryPersistence,
        operation_id: str,
        repo_node: str,
        generation: int,
        upload_lease: ArtifactUploadLease,
        lease_validation_time_ms: int,
        artifact: ArtifactReference,
        signed_root: SignedArtifactRoot,
        pages: Sequence[ArtifactManifestPage],
        chunks: Sequence[ArtifactChunk],
        capability: ArtifactCapability,
        trust_policy: ArtifactManifestTrustPolicy,
        receipt_authenticator: HmacReceiptAuthenticator,
        limits: ArtifactLimits | None = None,
    ) -> None:
        self.persistence = persistence
        self.operation_id = str(operation_id).strip()
        self.repo_node = str(repo_node).strip()
        self.generation = int(generation)
        self.artifact = artifact
        self.upload_lease = artifact_upload_lease_from_dict(
            _lease_dict(upload_lease), int(lease_validation_time_ms)
        )
        self.signed_root = signed_root
        self.pages = tuple(pages)
        self.chunks = tuple(chunks)
        self.receipt_authenticator = receipt_authenticator
        self.limits = limits or ArtifactLimits()
        if (
            not self.operation_id
            or self.repo_node != receipt_authenticator.repo_node
            or self.upload_lease.operation_id != self.operation_id
            or self.upload_lease.repo_node != self.repo_node
            or _artifact_dict(self.upload_lease.artifact) != _artifact_dict(artifact)
        ):
            raise ValueError("repo-artifact-session-invalid-identity")
        self.verification = verify_artifact_manifest_graph(
            signed_root,
            artifact,
            list(self.pages),
            list(self.chunks),
            capability,
            trust_policy,
            self.limits,
        )
        self.identity = ArtifactStorageIdentity(
            content_digest=artifact.content_digest,
            size_bytes=int(artifact.size_bytes),
            generation=self.generation,
            digest_algorithm=artifact.digest_algorithm,
        )
        self._chunks = {int(chunk.index): chunk for chunk in self.chunks}
        self._resume_identity_dict = _resume_identity_dict(
            self.artifact, self.signed_root
        )
        self.resume_identity = artifact_resume_identity_from_dict(
            self._resume_identity_dict, self.limits
        )
        self._resume = ArtifactResumeSession(
            self.resume_identity,
            self.upload_lease,
            list(self.chunks),
            int(lease_validation_time_ms),
        )
        verified = self._verified_chunk_indices()
        self._resume.restore_verified(verified)
        prior = self.persistence.transfer_session(self.operation_id)
        self._base_newly_verified_bytes = 0
        self._base_avoided_retransmission_bytes = 0
        if prior is not None:
            if (
                prior.artifact_digest != self.artifact.content_digest
                or prior.generation != self.generation
                or prior.identity != self._resume_identity_dict
                or prior.verified_chunks > len(verified)
            ):
                raise ValueError(REPO_RESUME_IDENTITY_CONFLICT)
            self._base_newly_verified_bytes = prior.newly_verified_bytes
            self._base_avoided_retransmission_bytes = (
                prior.avoided_retransmission_bytes
            )
            if prior.state in {"CANCELLED", "EXPIRED"}:
                old_lease = dict(prior.lease)
                new_lease = _lease_dict(self.upload_lease)
                if (
                    new_lease["operationId"] != old_lease.get("operationId")
                    or new_lease["repoNode"] != old_lease.get("repoNode")
                    or new_lease["artifact"] != old_lease.get("artifact")
                    or new_lease["leaseId"] == old_lease.get("leaseId")
                    or new_lease["replayId"] == old_lease.get("replayId")
                    or new_lease["expiresAtMs"]
                    <= int(old_lease.get("expiresAtMs", 0))
                ):
                    raise ValueError("repo-resume-invalid-renewal")
            elif prior.state == "FAILED":
                raise LifecycleTransitionError(
                    "repo-resume-progress-discarded: failed session cannot resume"
                )
            elif prior.state == "COMPLETED":
                if len(verified) != len(self.chunks):
                    raise LifecycleTransitionError(
                        "repo-resume-completed-progress-missing"
                    )
                self._resume.complete(int(lease_validation_time_ms))
                return
        self._save_resume_checkpoint(int(lease_validation_time_ms))

    @property
    def payload_store(self) -> FilesystemCasPayloadStore:
        return self.persistence.artifact_payload_store

    def _verified_chunk_indices(self) -> list[int]:
        ranges = self.payload_store.verified_ranges(self.identity)
        verified: list[int] = []
        for chunk in self.chunks:
            start = int(chunk.offset_bytes)
            end = start + int(chunk.length_bytes)
            if any(offset <= start and offset + length >= end
                   for offset, length in ranges):
                verified.append(int(chunk.index))
        return verified

    @staticmethod
    def _resume_state_name(snapshot: Any) -> str:
        return str(snapshot.state).rsplit(".", 1)[-1].upper()

    def _save_resume_checkpoint(self, now_ms: int) -> None:
        snapshot = self._resume.snapshot()
        self.persistence.save_transfer_session(
            operation_id=self.operation_id,
            artifact_digest=self.artifact.content_digest,
            generation=self.generation,
            identity=self._resume_identity_dict,
            lease=_lease_dict(self.upload_lease),
            state=self._resume_state_name(snapshot),
            preserves_progress=bool(snapshot.preserves_progress),
            verified_chunks=int(snapshot.verified_chunks),
            newly_verified_bytes=(
                self._base_newly_verified_bytes
                + int(snapshot.newly_verified_bytes)
            ),
            avoided_retransmission_bytes=(
                self._base_avoided_retransmission_bytes
                + int(snapshot.avoided_retransmission_bytes)
            ),
            updated_at_ms=int(now_ms),
        )

    def missing_chunks(self, now_ms: int) -> tuple[int, ...]:
        missing = tuple(int(index) for index in self._resume.missing_chunks(
            int(now_ms)
        ))
        self._save_resume_checkpoint(int(now_ms))
        return missing

    def renew_lease(self, lease: ArtifactUploadLease, now_ms: int) -> None:
        validated = artifact_upload_lease_from_dict(
            _lease_dict(lease), int(now_ms)
        )
        self._resume.renew_lease(validated, int(now_ms))
        self.upload_lease = validated
        self.persistence.renew_artifact_capacity(
            operation_id=self.operation_id,
            lease_id=validated.lease_id,
            expires_at_ms=int(validated.expires_at_ms),
            now_ms=int(now_ms),
        )
        self._save_resume_checkpoint(int(now_ms))

    def resume(self, lease: ArtifactUploadLease, now_ms: int) -> None:
        validated = artifact_upload_lease_from_dict(
            _lease_dict(lease), int(now_ms)
        )
        self._resume.resume(self.resume_identity, validated, int(now_ms))
        self.upload_lease = validated
        self.persistence.renew_artifact_capacity(
            operation_id=self.operation_id,
            lease_id=validated.lease_id,
            expires_at_ms=int(validated.expires_at_ms),
            now_ms=int(now_ms),
        )
        self._save_resume_checkpoint(int(now_ms))

    def expire(self, now_ms: int) -> bool:
        expired = bool(self._resume.expire(int(now_ms)))
        if expired:
            self._save_resume_checkpoint(int(now_ms))
        return expired

    def cancel(self, *, preserve_progress: bool, now_ms: int) -> None:
        lifecycle_state = self.state
        self._resume.cancel(bool(preserve_progress))
        if not preserve_progress:
            self.payload_store.abort(self.identity)
            self.persistence.release_artifact_capacity(
                self.operation_id, int(now_ms)
            )
        self._save_resume_checkpoint(int(now_ms))
        if (
            not preserve_progress
            and lifecycle_state in {
                "QUEUED", "RESERVED", "RECEIVING", "VERIFIED"
            }
        ):
            self.persistence.transition(
                event_id=f"{self.operation_id}:cancelled-failed",
                operation_id=self.operation_id,
                artifact_digest=self.artifact.content_digest,
                generation=self.generation,
                from_state=lifecycle_state,
                to_state="FAILED",
                detail={"reason": "destructive cancellation"},
                event_time_ms=int(now_ms),
            )

    @property
    def state(self) -> str:
        events = self.persistence.lifecycle_events(self.operation_id)
        accepted = [event for event in events if event.accepted]
        return "ABSENT" if not accepted else accepted[-1].to_state

    def _transition(
        self, from_state: str, to_state: str, event_suffix: str, now_ms: int
    ) -> None:
        self.persistence.transition(
            event_id=f"{self.operation_id}:{event_suffix}",
            operation_id=self.operation_id,
            artifact_digest=self.artifact.content_digest,
            generation=self.generation,
            from_state=from_state,
            to_state=to_state,
            detail={"rootManifestName": self.artifact.root_manifest_name},
            event_time_ms=int(now_ms),
        )

    def reserve(self, now_ms: int) -> None:
        """Start a legacy capacity-reservation session.

        New Selection-assigned tasks use :meth:`begin_assigned_task`.
        """
        self.persistence.reserve_artifact_capacity(
            operation_id=self.operation_id,
            artifact_digest=self.artifact.content_digest,
            generation=self.generation,
            lease_id=self.upload_lease.lease_id,
            reserved_bytes=int(self.upload_lease.reserved_bytes),
            expires_at_ms=int(self.upload_lease.expires_at_ms),
            now_ms=int(now_ms),
        )
        if self.state == "ABSENT":
            try:
                self._transition("ABSENT", "RESERVED", "reserved", now_ms)
            except BaseException:
                self.persistence.release_artifact_capacity(
                    self.operation_id, int(now_ms)
                )
                raise
        elif self.state not in {"RESERVED", "RECEIVING", "VERIFIED", "ACTIVE"}:
            raise LifecycleTransitionError(
                f"repo-artifact-session-state-conflict: cannot reserve from {self.state}"
            )
        self.payload_store.begin(self.identity)

    def begin_assigned_task(self, now_ms: int) -> None:
        """Accept one selected task without reserving bytes or taking a lock."""

        if self.state == "ABSENT":
            self._transition("ABSENT", "QUEUED", "queued", now_ms)
        elif self.state not in {"QUEUED", "RECEIVING", "VERIFIED", "ACTIVE"}:
            raise LifecycleTransitionError(
                "repo-artifact-session-state-conflict: "
                f"cannot begin assigned task from {self.state}"
            )
        self.payload_store.begin(self.identity)

    def receive_chunk(
        self, chunk_index: int, payload: bytes, *, now_ms: int
    ) -> bool:
        missing = set(self.missing_chunks(int(now_ms)))
        state = self.state
        if state in {"QUEUED", "RESERVED"}:
            self._transition(state, "RECEIVING", "receiving", now_ms)
        elif state != "RECEIVING":
            raise LifecycleTransitionError(
                f"repo-artifact-session-state-conflict: cannot receive from {state}"
            )
        try:
            chunk = self._chunks[int(chunk_index)]
        except KeyError as exc:
            raise ValueError("repo-artifact-session-unknown-chunk") from exc
        payload = bytes(payload)
        verify_artifact_chunk_payload(chunk, payload)
        if int(chunk_index) not in missing:
            self._resume.mark_verified(int(chunk_index), int(now_ms))
            self._save_resume_checkpoint(int(now_ms))
            return False
        self.payload_store.write_range(
            self.identity, int(chunk.offset_bytes), payload
        )
        self.payload_store.mark_verified(
            self.identity, int(chunk.offset_bytes), len(payload)
        )
        self._resume.mark_verified(int(chunk_index), int(now_ms))
        self._save_resume_checkpoint(int(now_ms))
        return True

    def verify_complete(self, now_ms: int) -> None:
        state = self.state
        if state in {"QUEUED", "RESERVED"} and int(self.artifact.size_bytes) == 0:
            self._transition(state, "RECEIVING", "receiving", now_ms)
            state = "RECEIVING"
        if state != "RECEIVING":
            raise LifecycleTransitionError(
                f"repo-artifact-session-state-conflict: cannot verify from {state}"
            )
        expected = (
            () if int(self.artifact.size_bytes) == 0
            else ((0, int(self.artifact.size_bytes)),)
        )
        if self.payload_store.verified_ranges(self.identity) != expected:
            raise LifecycleTransitionError(
                "repo-artifact-session-incomplete: verified coverage is incomplete"
            )
        self._resume.complete(int(now_ms))
        self._save_resume_checkpoint(int(now_ms))
        self._transition("RECEIVING", "VERIFIED", "verified", now_ms)

    def commit_and_activate(
        self,
        now_ms: int,
        *,
        crash_injector: Callable[[str], None] | None = None,
    ) -> AuthenticatedReplicaReceipt:
        if self.state == "ACTIVE":
            stored = self.persistence.authenticated_receipt(self.operation_id)
            if stored is None:
                raise LifecycleTransitionError(
                    "repo-artifact-receipt-missing: active artifact has no receipt"
                )
            envelope = AuthenticatedReplicaReceipt.from_dict({
                "schema": RECEIPT_SCHEMA,
                **stored,
            })
            self.receipt_authenticator.verify(
                envelope,
                expected_artifact=self.artifact,
                expected_operation_id=self.operation_id,
            )
            return envelope
        if self.state != "VERIFIED":
            raise LifecycleTransitionError(
                "repo-artifact-activation-state-conflict: state must be VERIFIED"
            )

        receipt_id = "receipt-" + hashlib.sha256(
            (
                self.operation_id
                + "\0"
                + self.repo_node
                + "\0"
                + self.artifact.content_digest
                + "\0"
                + str(self.generation)
            ).encode("utf-8")
        ).hexdigest()
        receipt = artifact_replica_receipt_from_dict({
            "receiptId": receipt_id,
            "operationId": self.operation_id,
            "repoNode": self.repo_node,
            "artifact": _artifact_dict(self.artifact),
            "committedAtMs": int(now_ms),
            "storageGeneration": self.generation,
            "policyEpoch": self.artifact.policy_epoch,
            "state": "COMMITTED",
        })
        envelope = self.receipt_authenticator.sign(receipt)
        self.receipt_authenticator.verify(
            envelope,
            expected_artifact=self.artifact,
            expected_operation_id=self.operation_id,
        )
        finalization = dict(
            operation_id=self.operation_id,
            artifact_digest=self.artifact.content_digest,
            generation=self.generation,
            logical_name=self.artifact.logical_name,
            policy_epoch=self.artifact.policy_epoch,
            artifact=_artifact_dict(self.artifact),
            receipt_id=receipt.receipt_id,
            receipt=_receipt_dict(receipt),
            repo_node=self.repo_node,
            signer_key_id=envelope.signer_key_id,
            authentication_algorithm=envelope.authentication_algorithm,
            signature_hex=envelope.signature.hex(),
            committed_at_ms=int(now_ms),
        )
        self.persistence.begin_finalization(**finalization)
        if crash_injector is not None:
            crash_injector("after-intent")
        self.payload_store.finalize(self.identity)
        if crash_injector is not None:
            crash_injector("after-payload-rename")
        self.persistence.mark_payload_finalized(
            self.operation_id, int(now_ms)
        )
        if crash_injector is not None:
            crash_injector("after-payload-finalize")
        self.persistence.commit_finalized_artifact(self.operation_id)
        if crash_injector is not None:
            crash_injector("after-metadata-commit")
        self.persistence.activate_finalized_artifact(self.operation_id)
        if crash_injector is not None:
            crash_injector("after-activation")
        return envelope

    def fail(self, reason: str, now_ms: int) -> None:
        state = self.state
        self._resume.fail(str(reason))
        self.payload_store.abort(self.identity)
        self.persistence.release_artifact_capacity(
            self.operation_id, int(now_ms)
        )
        self._save_resume_checkpoint(int(now_ms))
        if state in {"QUEUED", "RESERVED", "RECEIVING", "VERIFIED"}:
            self.persistence.transition(
                event_id=f"{self.operation_id}:failed",
                operation_id=self.operation_id,
                artifact_digest=self.artifact.content_digest,
                generation=self.generation,
                from_state=state,
                to_state="FAILED",
                detail={"reason": str(reason)[:1024]},
                event_time_ms=int(now_ms),
            )


def resolve_active_artifact(
    persistence: SqliteRepositoryPersistence,
    logical_name: str,
    policy_epoch: str,
) -> ArtifactReference:
    value = persistence.active_artifact(logical_name, policy_epoch)
    if value is None:
        raise LookupError(
            f"{REPO_ARTIFACT_NOT_ACTIVE}: {logical_name}@{policy_epoch}"
        )
    return artifact_reference_from_dict(value)


class AtomicArtifactDestination:
    """Out-of-order bounded range sink with no partial destination visibility."""

    _MAX_RANGES = 1 << 16
    _MAX_SIDECAR_BYTES = 4 * 1024 * 1024
    _HASH_BUFFER_BYTES = 256 * 1024

    def __init__(
        self,
        destination: str | Path,
        artifact: ArtifactReference,
        operation_id: str,
        *,
        max_range_bytes: int = 16 * 1024 * 1024,
    ) -> None:
        self.destination = Path(destination).resolve()
        self.artifact = artifact
        self.operation_id = str(operation_id).strip()
        self.max_range_bytes = int(max_range_bytes)
        if not self.operation_id or self.max_range_bytes <= 0:
            raise ValueError("repo-atomic-destination-invalid-options")
        self.destination.parent.mkdir(parents=True, exist_ok=True)
        suffix = hashlib.sha256(self.operation_id.encode("utf-8")).hexdigest()[:16]
        self.temporary = self.destination.with_name(
            f".{self.destination.name}.{suffix}.part"
        )
        self.sidecar = Path(str(self.temporary) + ".resume.json")
        self._ranges: list[tuple[int, int]] = []
        self._reused = False
        if self.destination.exists():
            if self._file_digest(self.destination) == artifact.content_digest:
                self._reused = True
                return
            raise FileExistsError(
                f"{REPO_ATOMIC_DESTINATION_CONFLICT}: {self.destination}"
            )
        if self.temporary.exists():
            self._restore_sidecar()
        else:
            descriptor = os.open(
                self.temporary, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600
            )
            try:
                os.ftruncate(descriptor, int(artifact.size_bytes))
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            self._persist_sidecar()

    @classmethod
    def _file_digest(cls, path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb", buffering=0) as stream:
            while True:
                block = stream.read(cls._HASH_BUFFER_BYTES)
                if not block:
                    break
                digest.update(block)
        return digest.hexdigest()

    @staticmethod
    def _fsync_directory(path: Path) -> None:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _sidecar_value(self) -> dict[str, Any]:
        return {
            "schema": "ndnsf-repo-atomic-destination-resume-v1",
            "operationId": self.operation_id,
            "artifact": _artifact_dict(self.artifact),
            "ranges": [[offset, length] for offset, length in self._ranges],
        }

    def _persist_sidecar(self) -> None:
        payload = _canonical_json(self._sidecar_value())
        if len(payload) > self._MAX_SIDECAR_BYTES:
            raise RuntimeError("repo-atomic-destination-sidecar-limit")
        temporary = Path(str(self.sidecar) + ".tmp")
        descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600
        )
        try:
            view = memoryview(payload)
            while view:
                written = os.write(descriptor, view)
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        os.replace(temporary, self.sidecar)
        self._fsync_directory(self.sidecar.parent)

    def _restore_sidecar(self) -> None:
        try:
            payload = self.sidecar.read_bytes()
            if len(payload) > self._MAX_SIDECAR_BYTES:
                raise ValueError("sidecar is too large")
            value = json.loads(payload.decode("utf-8"))
        except (FileNotFoundError, UnicodeDecodeError, json.JSONDecodeError,
                ValueError) as exc:
            raise RuntimeError(
                f"{REPO_RESUME_IDENTITY_CONFLICT}: invalid destination checkpoint"
            ) from exc
        if (
            not isinstance(value, dict)
            or value.get("schema")
            != "ndnsf-repo-atomic-destination-resume-v1"
            or value.get("operationId") != self.operation_id
            or value.get("artifact") != _artifact_dict(self.artifact)
            or set(value) != {"schema", "operationId", "artifact", "ranges"}
        ):
            raise ValueError(REPO_RESUME_IDENTITY_CONFLICT)
        if self.temporary.stat().st_size != int(self.artifact.size_bytes):
            raise ValueError(REPO_RESUME_IDENTITY_CONFLICT)
        ranges = value.get("ranges")
        if not isinstance(ranges, list) or len(ranges) > self._MAX_RANGES:
            raise ValueError(REPO_RESUME_IDENTITY_CONFLICT)
        prior_end = -1
        for item in ranges:
            if (
                not isinstance(item, list)
                or len(item) != 2
                or not all(isinstance(part, int) for part in item)
            ):
                raise ValueError(REPO_RESUME_IDENTITY_CONFLICT)
            offset, length = item
            if (
                length <= 0
                or offset < 0
                or offset > int(self.artifact.size_bytes)
                or length > int(self.artifact.size_bytes) - offset
                or offset <= prior_end
            ):
                raise ValueError(REPO_RESUME_IDENTITY_CONFLICT)
            self._merge_range(offset, length)
            prior_end = offset + length

    def missing_ranges(
        self, *, maximum_range_bytes: int | None = None
    ) -> tuple[tuple[int, int], ...]:
        bound = self.max_range_bytes if maximum_range_bytes is None else int(
            maximum_range_bytes
        )
        if bound <= 0 or bound > self.max_range_bytes:
            raise ValueError("repo-atomic-destination-range-invalid")
        missing: list[tuple[int, int]] = []
        cursor = 0
        for offset, length in [*self._ranges, (
            int(self.artifact.size_bytes), 0
        )]:
            while cursor < offset:
                span = min(bound, offset - cursor)
                missing.append((cursor, span))
                cursor += span
            cursor = max(cursor, offset + length)
        return tuple(missing)

    def write_range(self, offset: int, payload: bytes) -> None:
        if self._reused:
            raise RuntimeError("repo-atomic-destination-already-complete")
        offset = int(offset)
        payload = bytes(payload)
        if (
            offset < 0
            or len(payload) > self.max_range_bytes
            or offset > int(self.artifact.size_bytes)
            or len(payload) > int(self.artifact.size_bytes) - offset
        ):
            raise ValueError("repo-atomic-destination-range-invalid")
        end = offset + len(payload)
        overlaps = [
            (current_offset, current_length)
            for current_offset, current_length in self._ranges
            if offset < current_offset + current_length
            and current_offset < end
        ]
        if overlaps:
            if any(
                current_offset <= offset
                and current_offset + current_length >= end
                for current_offset, current_length in overlaps
            ):
                descriptor = os.open(self.temporary, os.O_RDONLY)
                try:
                    existing = os.pread(descriptor, len(payload), offset)
                finally:
                    os.close(descriptor)
                if existing == payload:
                    return
            raise ValueError(
                "repo-atomic-destination-verified-range-conflict"
            )
        descriptor = os.open(self.temporary, os.O_WRONLY)
        try:
            view = memoryview(payload)
            cursor = offset
            while view:
                written = os.pwrite(descriptor, view, cursor)
                view = view[written:]
                cursor += written
        finally:
            os.close(descriptor)
        self._merge_range(offset, len(payload))
        descriptor = os.open(self.temporary, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        self._persist_sidecar()

    def _merge_range(self, offset: int, length: int) -> None:
        if length == 0:
            return
        merged: list[tuple[int, int]] = []
        for current_offset, current_length in sorted(
            [*self._ranges, (offset, length)]
        ):
            end = current_offset + current_length
            if merged and current_offset <= merged[-1][0] + merged[-1][1]:
                prior_offset, prior_length = merged[-1]
                merged[-1] = (
                    prior_offset,
                    max(prior_offset + prior_length, end) - prior_offset,
                )
            else:
                merged.append((current_offset, current_length))
        if len(merged) > self._MAX_RANGES:
            raise RuntimeError("repo-atomic-destination-range-limit")
        self._ranges = merged

    def finalize(self) -> Path:
        if self._reused:
            return self.destination
        expected = (
            [] if int(self.artifact.size_bytes) == 0
            else [(0, int(self.artifact.size_bytes))]
        )
        if self._ranges != expected:
            raise RuntimeError(
                f"{REPO_ATOMIC_DESTINATION_INCOMPLETE}: verified coverage incomplete"
            )
        descriptor = os.open(self.temporary, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        if self._file_digest(self.temporary) != self.artifact.content_digest:
            self.abort()
            raise ValueError(
                "repo-atomic-destination-digest-mismatch: full digest mismatch"
            )
        try:
            os.link(self.temporary, self.destination)
        except FileExistsError:
            if self._file_digest(self.destination) != self.artifact.content_digest:
                self.abort()
                raise FileExistsError(
                    f"{REPO_ATOMIC_DESTINATION_CONFLICT}: {self.destination}"
                )
        self.temporary.unlink(missing_ok=True)
        self.sidecar.unlink(missing_ok=True)
        self._fsync_directory(self.destination.parent)
        return self.destination

    def abort(self, *, preserve_progress: bool = False) -> None:
        if not self._reused:
            if preserve_progress:
                self._persist_sidecar()
                return
            self.temporary.unlink(missing_ok=True)
            self.sidecar.unlink(missing_ok=True)
            self._fsync_directory(self.destination.parent)

    def cancel(self, *, preserve_progress: bool = True) -> None:
        self.abort(preserve_progress=preserve_progress)


def retrieve_to_atomic_destination(
    artifact: ArtifactReference,
    destination: str | Path,
    operation_id: str,
    verified_ranges: Iterable[tuple[int, bytes]],
) -> Path:
    sink = AtomicArtifactDestination(destination, artifact, operation_id)
    if sink._reused:
        return sink.finalize()
    try:
        for offset, payload in verified_ranges:
            sink.write_range(offset, payload)
        return sink.finalize()
    except BaseException:
        sink.abort()
        raise


__all__ = [
    "AuthenticatedReplicaReceipt",
    "ArtifactReplicaSession",
    "AtomicArtifactDestination",
    "HmacReceiptAuthenticator",
    "resolve_active_artifact",
    "retrieve_to_atomic_destination",
]
