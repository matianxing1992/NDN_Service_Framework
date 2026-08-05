"""Whole-artifact NDNSF Collaboration control and RepoNode data plane."""

from __future__ import annotations

import base64
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import threading
import time
from typing import Any

from ndnsf import (
    AckDecision,
    FileSegmentedObjectProducer,
    ServiceOperationState,
    ServiceOperationStatus,
    fetch_adaptive_segmented_data_packets,
    fetch_segmented_object,
)

from ._py_repoclient import (
    ArtifactLimits,
    SignedArtifactRoot,
    artifact_capability_from_dict,
    artifact_chunk_from_dict,
    artifact_manifest_page_from_dict,
    artifact_reference_from_dict,
    artifact_replica_receipt_from_dict,
    artifact_root_manifest_from_dict,
    artifact_sha256_hex,
    canonical_manifest_page_bytes,
    canonical_root_manifest_bytes,
    decode_artifact_manifest_page,
    decode_signed_artifact_root,
    encode_artifact_manifest_page,
    encode_signed_artifact_root,
    verify_artifact_chunk_payload,
    verify_artifact_manifest_graph,
)
from .artifact_api import (
    ArtifactApiBackend,
    ArtifactControlMode,
    ArtifactDescriptor,
    ArtifactFetchResult,
    ArtifactProgress,
    ArtifactPublishResult,
    ArtifactReplicaResult,
    ArtifactSessionStatus,
)
from .artifact_lifecycle import (
    AuthenticatedReplicaReceipt,
    AtomicArtifactDestination,
    HmacReceiptAuthenticator,
    RECEIPT_SCHEMA,
)
from .artifact_transfer import (
    ArtifactStoreOffer,
    ReplicaTaskCollaborationClient,
    decode_store_assignment,
    encode_store_offer_ack,
)
from .persistence import ArtifactStorageIdentity


_DEFAULT_PACKET_PAYLOAD_BYTES = 7600
_DEFAULT_CHUNK_BYTES = 16 * 1024 * 1024
_MAX_REPLICA_ROLES = 32


def _repo_pending_state_ttl_ms(size_bytes: int) -> int:
    """Return a bounded Core pending-state horizon for one artifact.

    The provider keeps request/token state while it pulls and verifies the
    immutable object. Eight MiB/s is deliberately conservative relative to
    the observed NDN path; five minutes of slack covers manifest validation
    and finalization. Core currently caps provider-authorized TTLs at one
    hour, so this helper never asks for an unbounded lifetime.
    """

    size = max(0, int(size_bytes))
    estimated_ms = (
        (size * 1000 + (8 * 1024 * 1024 - 1))
        // (8 * 1024 * 1024)
    )
    return min(3_600_000, max(300_000, int(estimated_ms) + 300_000))


def _artifact_dict(artifact) -> dict[str, Any]:
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


@dataclass(frozen=True)
class ArtifactControlMetrics:
    request_count: int
    ack_closed_count: int
    selection_commit_count: int
    response_count: int
    selected_replicas: int
    elapsed_ms: float

    @property
    def control_operation_count(self) -> int:
        return self.request_count + self.selection_commit_count

    @property
    def lifecycle_phase_count(self) -> int:
        return 3


class _PreparedArtifactSource:
    """One bounded-memory source plus an assignment-bound signed root."""

    def __init__(
        self,
        path: Path,
        descriptor: ArtifactDescriptor,
        operation_id: str,
        *,
        packet_payload_bytes: int,
        chunk_bytes: int,
    ) -> None:
        self.path = Path(path).resolve()
        self.descriptor = descriptor
        self.operation_id = str(operation_id)
        self.packet_payload_bytes = int(packet_payload_bytes)
        requested_chunk_bytes = int(chunk_bytes)
        self.chunk_bytes = (
            requested_chunk_bytes // self.packet_payload_bytes
        ) * self.packet_payload_bytes
        if (
            self.packet_payload_bytes <= 0
            or self.packet_payload_bytes > 8800
            or requested_chunk_bytes <= 0
            or self.chunk_bytes <= 0
            or self.chunk_bytes > 64 * 1024 * 1024
        ):
            raise ValueError("repo-artifact-source-invalid-transfer-bounds")
        self._temporary = tempfile.TemporaryDirectory(
            prefix="ndnsf-artifact-source-"
        )
        self.root_dir = Path(self._temporary.name)
        identity = descriptor.reference.publisher_identity.rstrip("/")
        suffix = hashlib.sha256(self.operation_id.encode()).hexdigest()[:32]
        self.source_prefix = f"{identity}/NDNSF-ARTIFACT-SOURCE/{suffix}"
        self.root_name = f"{self.source_prefix}/root"
        self.page_name = f"{self.source_prefix}/page/0"
        self.payload_name = f"{self.source_prefix}/payload"
        self.key_locator = f"{identity}/KEY/NDNSF-ARTIFACT/{suffix}"
        self._producers: list[FileSegmentedObjectProducer] = []
        self._prepare_manifest()

    def _prepare_manifest(self) -> None:
        reference = self.descriptor.reference
        limits = ArtifactLimits()
        size = int(reference.size_bytes)
        chunks = []
        children = []
        with self.path.open("rb", buffering=0) as source:
            offset = 0
            index = 0
            while offset < size:
                length = min(self.chunk_bytes, size - offset)
                digest = hashlib.sha256()
                remaining = length
                while remaining:
                    block = source.read(min(256 * 1024, remaining))
                    if not block:
                        raise RuntimeError(
                            "repo-artifact-source-truncated-during-manifest"
                        )
                    digest.update(block)
                    remaining -= len(block)
                chunk = {
                    "index": index,
                    "offsetBytes": offset,
                    "lengthBytes": length,
                    "digestAlgorithm": "sha256",
                    "digest": digest.hexdigest(),
                    "firstSegment": 0,
                    "finalSegment": (
                        (length - 1) // self.packet_payload_bytes
                    ),
                }
                artifact_chunk_from_dict(chunk, reference, limits)
                chunks.append(chunk)
                children.append({
                    "kind": "chunk",
                    "index": index,
                    "offsetBytes": offset,
                    "lengthBytes": length,
                    "digestAlgorithm": "sha256",
                    "digest": digest.hexdigest(),
                })
                offset += length
                index += 1
        page_value = {
            "pageVersion": "artifact-manifest-page-v2",
            "depth": 0,
            "offsetBytes": 0,
            "lengthBytes": size,
            "pageDigestAlgorithm": "sha256",
            "pageDigest": "0" * 64,
            "children": children,
        }
        placeholder = artifact_manifest_page_from_dict(
            page_value, 512, limits
        )
        page_value["pageDigest"] = artifact_sha256_hex(
            canonical_manifest_page_bytes(placeholder, limits)
        )
        page = artifact_manifest_page_from_dict(page_value, 512, limits)
        now_ms = int(time.time() * 1000)
        root_value = {
            "artifact": _artifact_dict(reference),
            "packetPayloadBytes": self.packet_payload_bytes,
            "chunkBytes": self.chunk_bytes,
            "namingTemplate": (
                reference.logical_name.rstrip("/")
                + "/payload/chunk/{chunk}/segment/{segment}"
            ),
            "manifestRootDigestAlgorithm": "sha256",
            "manifestRootDigest": page_value["pageDigest"],
            "signatureAlgorithm": "rsa-sha256",
            "publisherKeyLocator": self.key_locator,
            "createdAtMs": now_ms,
            "expiresAtMs": now_ms + max(
                24 * 60 * 60 * 1000, int(self.descriptor.timeout_ms) * 2
            ),
            "criticalExtensions": [],
        }
        root = artifact_root_manifest_from_dict(root_value, 1024, limits)
        private_key = self.root_dir / "publisher-private.pem"
        public_key = self.root_dir / "publisher-public.pem"
        canonical = self.root_dir / "root.canonical"
        signature = self.root_dir / "root.signature"
        subprocess.run([
            "openssl", "genpkey", "-algorithm", "RSA",
            "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(private_key),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([
            "openssl", "pkey", "-in", str(private_key),
            "-pubout", "-out", str(public_key),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        canonical.write_bytes(canonical_root_manifest_bytes(root, limits))
        subprocess.run([
            "openssl", "dgst", "-sha256", "-sign", str(private_key),
            "-out", str(signature), str(canonical),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        signed = SignedArtifactRoot()
        signed.root = root
        signed.signature_value = signature.read_bytes()
        self.root_path = self.root_dir / "root.wire"
        self.page_path = self.root_dir / "page.wire"
        self.root_path.write_bytes(encode_signed_artifact_root(signed, limits))
        self.page_path.write_bytes(encode_artifact_manifest_page(page, limits))
        self.public_key_pem = public_key.read_text(encoding="utf-8")
        private_key.unlink()
        canonical.unlink()
        signature.unlink()
        self.page_encoded_bytes = self.page_path.stat().st_size

    def start(self) -> None:
        for name, path in (
            (self.root_name, self.root_path),
            (self.page_name, self.page_path),
            (self.payload_name, self.path),
        ):
            self._producers.append(FileSegmentedObjectProducer(
                name,
                str(path),
                signing_identity=self.descriptor.reference.publisher_identity,
                max_segment_size=self.packet_payload_bytes,
                freshness_ms=max(60_000, int(self.descriptor.timeout_ms)),
                digest_signing=True,
            ).start())
        # Give NFD prefix registration one bounded scheduling interval.
        time.sleep(0.2)

    def stop(self) -> None:
        for producer in reversed(self._producers):
            producer.stop()
        self._producers.clear()
        self._temporary.cleanup()


class _NetworkPublishDriver:
    def __init__(
        self,
        backend: "CollaborationArtifactApiBackend",
        descriptor: ArtifactDescriptor,
        operation_id: str,
        emit_progress,
    ) -> None:
        self.backend = backend
        self.descriptor = descriptor
        self.operation_id = operation_id
        self.emit_progress = emit_progress
        self.control_metrics = None
        self._result = None
        self._state = "OPEN"
        self._started = time.monotonic()
        self._progress_sequence = 0

    def _emit_progress(
        self, *, phase: str, received: int, verified: int, committed: int,
        selected: int = 0, committed_replicas: int = 0,
    ) -> None:
        self._progress_sequence += 1
        self.emit_progress(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.descriptor.reference,
            phase=phase,
            received_bytes=int(received),
            verified_bytes=int(verified),
            committed_bytes=int(committed),
            total_bytes=int(self.descriptor.reference.size_bytes),
            selected_replicas=int(selected),
            committed_replicas=int(committed_replicas),
            retransmitted_bytes=0,
            sequence=self._progress_sequence,
            timestamp_ms=time.time_ns() // 1_000_000,
        ))

    def transfer(self, path, cancellation) -> None:
        cancellation.raise_if_cancelled(
            self.operation_id, self.descriptor.reference
        )
        if self.descriptor.control.mode == ArtifactControlMode.TARGETED:
            raise ValueError(
                "whole-artifact network publication requires Collaboration"
            )
        if self.descriptor.requested_replicas > _MAX_REPLICA_ROLES:
            raise ValueError(
                "requested replicas exceed bounded artifact role capacity"
            )
        source = _PreparedArtifactSource(
            Path(path),
            self.descriptor,
            self.operation_id,
            packet_payload_bytes=self.backend.packet_payload_bytes,
            chunk_bytes=self.backend.chunk_bytes,
        )
        started = time.monotonic()
        source.start()
        try:
            pending = self.backend.control.begin(
                self.descriptor.reference,
                requested_replicas=self.descriptor.requested_replicas,
                operation_id=self.operation_id,
                ack_timeout_ms=self.backend.ack_timeout_ms,
                timeout_ms=self.descriptor.timeout_ms,
            )
            receipt_scope = (
                "repo-artifact-receipts-"
                + hashlib.sha256(self.operation_id.encode()).hexdigest()[:24]
            )
            selected = pending.commit_ack_tasks({
                "source_root_name": source.root_name,
                "source_page_name": source.page_name,
                "source_payload_name": source.payload_name,
                "publisher_key_pem": source.public_key_pem,
                "publisher_key_locator": source.key_locator,
                "packet_payload_bytes": source.packet_payload_bytes,
                "manifest_page_encoded_bytes": source.page_encoded_bytes,
                "receipt_scope": receipt_scope,
                "receipt_topic": "/receipt",
                "receipt_key": secrets.token_bytes(32),
            })
            response = pending.result(self.descriptor.timeout_ms)
            if not response.status:
                raise RuntimeError(
                    "repository collaboration response was not successful: "
                    + str(getattr(response, "error", ""))
                )
            aggregate = json.loads(bytes(response.payload).decode("utf-8"))
            if (
                aggregate.pop("schema", None)
                != "ndnsf-repo-artifact-publish-result-v1"
                or set(aggregate) != {"operationId", "receipts"}
                or aggregate["operationId"] != self.operation_id
            ):
                raise RuntimeError("repo artifact result has invalid binding")
            receipts = tuple(aggregate["receipts"])
            providers = tuple(sorted(
                str(item["receipt"]["repoNode"]) for item in receipts
            ))
            if providers != tuple(sorted(selected)):
                raise RuntimeError(
                    "repository receipts differ from committed selection"
                )
            replica_results = []
            for value in receipts:
                envelope_value = dict(value)
                envelope_value.pop("dataName", None)
                envelope = AuthenticatedReplicaReceipt.from_dict(envelope_value)
                receipt = envelope.receipt
                if (
                    receipt.operation_id != self.operation_id
                    or receipt.artifact.content_digest
                    != self.descriptor.reference.content_digest
                    or receipt.state != "COMMITTED"
                ):
                    raise RuntimeError(
                        "repo artifact receipt identity mismatch"
                    )
                replica_results.append(ArtifactReplicaResult(
                    repo_node=receipt.repo_node,
                    state="COMMITTED",
                    receipt_id=receipt.receipt_id,
                ))
            self._emit_progress(
                phase="transfer",
                received=int(self.descriptor.reference.size_bytes),
                verified=int(self.descriptor.reference.size_bytes),
                committed=0,
                selected=len(selected),
            )
            elapsed_ms = (time.monotonic() - started) * 1000.0
            self._result = ArtifactPublishResult(
                reference=self.descriptor.reference,
                operation_id=self.operation_id,
                requested_replicas=self.descriptor.requested_replicas,
                achieved_replicas=len(replica_results),
                replicas=tuple(replica_results),
                total_duration_ms=elapsed_ms,
            )
            self._state = "VERIFIED"
            self.control_metrics = ArtifactControlMetrics(
                request_count=1,
                ack_closed_count=1,
                selection_commit_count=len(selected),
                response_count=1,
                selected_replicas=len(selected),
                elapsed_ms=elapsed_ms,
            )
            self.backend.last_control_metrics = self.control_metrics
            self.backend._store_receipts(receipts)
        finally:
            source.stop()

    def status(self):
        return ArtifactSessionStatus(
            self.operation_id,
            "PUBLISH",
            self._state,
            self.descriptor.reference,
        )

    def commit(self):
        if self._result is None:
            raise RuntimeError("artifact network transfer is incomplete")
        self._emit_progress(
            phase="commit",
            received=int(self.descriptor.reference.size_bytes),
            verified=int(self.descriptor.reference.size_bytes),
            committed=int(self.descriptor.reference.size_bytes),
            selected=int(self._result.achieved_replicas),
            committed_replicas=int(self._result.achieved_replicas),
        )
        self._state = "COMMITTED"
        return self._result

    def abort(self, preserve_progress):
        del preserve_progress
        self._state = "CANCELLED"
        return self.status()


class _NetworkFetchDriver:
    def __init__(
        self,
        backend: "CollaborationArtifactApiBackend",
        reference,
        destination: Path,
        operation_id: str,
        *,
        resume: bool,
        verify: bool,
        replace: bool,
        timeout_ms: int,
        control,
        emit_progress,
    ) -> None:
        self.backend = backend
        self.reference = reference
        self.destination = Path(destination).resolve()
        self.operation_id = str(operation_id)
        self.resume = bool(resume)
        self.verify = bool(verify)
        self.replace = bool(replace)
        self.timeout_ms = int(timeout_ms)
        self.control = control
        if not self.verify:
            raise ValueError(
                "repo artifact network fetch requires signed-manifest verification"
            )
        if self.control.mode == ArtifactControlMode.TARGETED:
            raise ValueError(
                "repo artifact network fetch does not support targeted control"
            )
        if self.timeout_ms <= 0:
            raise ValueError("repo artifact fetch timeout must be positive")
        self.emit_progress = emit_progress
        self.started = time.monotonic()
        self.state = "OPEN"
        self._sink = AtomicArtifactDestination(
            self.destination,
            self.reference,
            self.operation_id,
            max_range_bytes=max(
                16 * 1024 * 1024, self.backend.packet_payload_bytes
            ),
            resume=self.resume,
            replace=self.replace,
        )
        candidates = [
            value for value in self.backend.last_receipts
            if (
                value.get("receipt", {})
                .get("artifact", {})
                .get("contentDigest")
                == self.reference.content_digest
            )
        ]
        if not candidates:
            raise LookupError(
                "repo artifact fetch requires a committed publication receipt"
            )
        self.receipt = dict(candidates[0])
        self.data_name = str(self.receipt.get("dataName", ""))
        self.repo_node = str(
            self.receipt["receipt"]["repoNode"]
        )
        if not self.data_name:
            raise ValueError("repo artifact receipt is missing dataName")
        self.transferred_bytes = 0
        self.reused_bytes = (
            int(self.reference.size_bytes)
            if self._sink._reused else 0
        )
        self.retransmitted_bytes = 0

    def transfer(self, cancellation) -> None:
        cancellation.raise_if_cancelled(
            self.operation_id, self.reference
        )
        if not self._sink._reused:
            def persist(packet) -> None:
                cancellation.raise_if_cancelled(
                    self.operation_id, self.reference
                )
                offset = (
                    int(packet.segment) * self.backend.packet_payload_bytes
                )
                if (
                    offset >= int(self.reference.size_bytes)
                    or len(packet.content)
                    > int(self.reference.size_bytes) - offset
                ):
                    raise RuntimeError(
                        "repo artifact fetch segment exceeds declared size"
                    )
                self._sink.write_range(offset, packet.content)

            metrics = fetch_adaptive_segmented_data_packets(
                self.data_name,
                persist,
                timeout_ms=self.timeout_ms,
                interest_lifetime_ms=10_000,
                initial_window=8,
                maximum_window=128,
                maximum_retries=5,
                persistence_backlog_limit=32,
            )
            self.transferred_bytes = int(metrics.logical_bytes)
            self.retransmitted_bytes = int(metrics.retransmitted_bytes)
        self.state = "VERIFIED"
        self.emit_progress(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.reference,
            phase="transfer",
            received_bytes=(
                self.transferred_bytes + self.reused_bytes
            ),
            verified_bytes=(
                self.transferred_bytes + self.reused_bytes
            ),
            committed_bytes=0,
            total_bytes=int(self.reference.size_bytes),
            selected_replicas=1,
            committed_replicas=0,
            retransmitted_bytes=self.retransmitted_bytes,
            sequence=1,
            timestamp_ms=time.time_ns() // 1_000_000,
        ))

    def status(self):
        return ArtifactSessionStatus(
            self.operation_id,
            "FETCH",
            self.state,
            self.reference,
        )

    def commit(self):
        if self.state not in {"VERIFIED", "COMMITTED"}:
            raise RuntimeError("repo artifact fetch is not verified")
        self._sink.finalize()
        self.state = "COMMITTED"
        return ArtifactFetchResult(
            reference=self.reference,
            operation_id=self.operation_id,
            destination=self.destination,
            reused_bytes=self.reused_bytes,
            transferred_bytes=self.transferred_bytes,
            source_replicas=(self.repo_node,),
            total_duration_ms=(time.monotonic() - self.started) * 1000.0,
        )

    def abort(self, preserve_progress):
        self._sink.abort(preserve_progress=bool(preserve_progress))
        self.state = "CANCELLED"
        return self.status()


class _CollaborationPublishDriver:
    """Compatibility adapter for an existing data-plane delegate."""

    def __init__(
        self, backend, delegate_driver, descriptor, operation_id
    ) -> None:
        self.backend = backend
        self.delegate_driver = delegate_driver
        self.descriptor = descriptor
        self.operation_id = operation_id
        self.control_metrics = None
        self.selected_repo_nodes = ()

    def transfer(self, path, cancellation) -> None:
        cancellation.raise_if_cancelled(
            self.operation_id, self.descriptor.reference
        )
        if self.descriptor.control.mode == ArtifactControlMode.TARGETED:
            self.delegate_driver.transfer(path, cancellation)
            return
        started = time.monotonic()
        pending = self.backend.control.begin(
            self.descriptor.reference,
            requested_replicas=self.descriptor.requested_replicas,
            operation_id=self.operation_id,
            ack_timeout_ms=self.backend.ack_timeout_ms,
            timeout_ms=self.descriptor.timeout_ms,
        )
        selected = pending.commit_ack_tasks()
        self.selected_repo_nodes = tuple(selected)
        # A real delegate may start the producer/fetch path needed by the
        # selected provider, so it must run before waiting for final Response.
        self.delegate_driver.transfer(path, cancellation)
        response = pending.result(self.descriptor.timeout_ms)
        if not response.status:
            raise RuntimeError(
                "repository collaboration response was not successful: "
                + str(getattr(response, "error", ""))
            )
        self.control_metrics = ArtifactControlMetrics(
            request_count=1,
            ack_closed_count=1,
            selection_commit_count=len(selected),
            response_count=1,
            selected_replicas=len(selected),
            elapsed_ms=(time.monotonic() - started) * 1000.0,
        )
        self.backend.last_control_metrics = self.control_metrics

    def status(self):
        return self.delegate_driver.status()

    def commit(self):
        result = self.delegate_driver.commit()
        if self.selected_repo_nodes:
            committed = tuple(sorted(item.repo_node for item in result.replicas))
            if committed != tuple(sorted(self.selected_repo_nodes)):
                raise RuntimeError(
                    "repository receipt providers differ from committed "
                    "Collaboration selection"
                )
        return result

    def abort(self, preserve_progress):
        return self.delegate_driver.abort(preserve_progress)


class CollaborationArtifactApiBackend:
    """One collaboration and one segmented transfer per immutable artifact.

    ``delegate=None`` selects the production whole-artifact network path.
    Supplying a delegate retains the pre-T034 compatibility adapter.
    """

    def __init__(
        self,
        delegate: ArtifactApiBackend | None,
        service_user,
        service_name: str,
        *,
        ack_timeout_ms: int = 300,
        packet_payload_bytes: int = _DEFAULT_PACKET_PAYLOAD_BYTES,
        chunk_bytes: int = _DEFAULT_CHUNK_BYTES,
        committed_receipts: tuple[dict[str, Any], ...] = (),
        receipt_store_path: str | Path | None = None,
        clock_ms=None,
    ) -> None:
        if int(ack_timeout_ms) <= 0:
            raise ValueError("artifact collaboration ACK timeout must be positive")
        self.delegate = delegate
        self.control = ReplicaTaskCollaborationClient(
            service_user, str(service_name)
        )
        self.ack_timeout_ms = int(ack_timeout_ms)
        self.packet_payload_bytes = int(packet_payload_bytes)
        self.chunk_bytes = int(chunk_bytes)
        self.clock_ms = clock_ms or (lambda: time.time_ns() // 1_000_000)
        self.last_control_metrics = None
        self._receipt_store_path = (
            None if receipt_store_path is None else Path(receipt_store_path)
        )
        self.last_receipts = self._load_receipts(committed_receipts)

    def _load_receipts(
        self, committed_receipts: tuple[dict[str, Any], ...]
    ) -> tuple[dict[str, Any], ...]:
        if committed_receipts:
            return tuple(dict(value) for value in committed_receipts)
        if self._receipt_store_path is None or not self._receipt_store_path.exists():
            return ()
        try:
            value = json.loads(
                self._receipt_store_path.read_text(encoding="utf-8")
            )
            if not isinstance(value, list) or not all(
                isinstance(item, dict) for item in value
            ):
                raise ValueError("receipt catalog must be a list of objects")
            return tuple(dict(item) for item in value)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise RuntimeError("repo artifact receipt catalog is invalid") from error

    def _store_receipts(self, receipts: tuple[dict[str, Any], ...]) -> None:
        merged = list(self.last_receipts)
        merged.extend(dict(value) for value in receipts)
        unique: dict[tuple[str, str], dict[str, Any]] = {}
        for value in merged:
            receipt = value.get("receipt", {})
            key = (
                str(receipt.get("receiptId", "")),
                str(value.get("dataName", "")),
            )
            if key != ("", ""):
                unique[key] = value
        self.last_receipts = tuple(unique.values())[-1024:]
        if self._receipt_store_path is None:
            return
        path = self._receipt_store_path
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(str(path) + ".tmp")
        temporary.write_text(
            json.dumps(
                list(self.last_receipts), sort_keys=True, separators=(",", ":")
            ),
            encoding="utf-8",
        )
        os.replace(temporary, path)

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path,
        state_root: str | Path,
        user: str,
        service_name: str = "/NDNSF/DistributedRepo/Artifact/v2/STORE",
        bootstrap_token: str = "",
        ack_timeout_ms: int = 3000,
        packet_payload_bytes: int = _DEFAULT_PACKET_PAYLOAD_BYTES,
        chunk_bytes: int = _DEFAULT_CHUNK_BYTES,
        committed_receipts: tuple[dict[str, Any], ...] = (),
    ) -> "CollaborationArtifactApiBackend":
        """Construct the public artifact transport from one deployment file."""
        from ndnsf import ServiceUser
        from ndnsf_distributed_inference.app import APPDeployment

        deployment = APPDeployment.from_config(
            config,
            state_root=state_root,
            identity=(
                "artifact-client-"
                + hashlib.sha256(str(user).encode()).hexdigest()[:16]
            ),
            generated_policy_dir=generated_policy_dir,
        ).deployment
        service_user = ServiceUser(
            group=deployment.group,
            controller=deployment.controller,
            user=user,
            trust_schema=deployment.trust_schema,
            permission_wait_ms=6000,
            adaptive_admission=False,
            bootstrap_token=bootstrap_token,
        )
        return cls(
            None,
            service_user,
            service_name,
            ack_timeout_ms=ack_timeout_ms,
            packet_payload_bytes=packet_payload_bytes,
            chunk_bytes=chunk_bytes,
            committed_receipts=committed_receipts,
            receipt_store_path=Path(state_root) / "artifact-receipts.json",
        )

    def begin_publish(
        self, descriptor: ArtifactDescriptor, operation_id: str, emit_progress
    ):
        if self.delegate is None:
            return _NetworkPublishDriver(
                self, descriptor, operation_id, emit_progress
            )
        delegate_driver = self.delegate.begin_publish(
            descriptor, operation_id, emit_progress
        )
        return _CollaborationPublishDriver(
            self, delegate_driver, descriptor, operation_id
        )

    def begin_fetch(self, *args, **kwargs):
        if self.delegate is None:
            reference, destination, operation_id = args[:3]
            return _NetworkFetchDriver(
                self,
                reference,
                Path(destination),
                operation_id,
                resume=bool(kwargs.get("resume", True)),
                verify=bool(kwargs.get("verify", True)),
                replace=bool(kwargs.get("replace", False)),
                timeout_ms=int(kwargs.get("timeout_ms", 60_000)),
                control=kwargs["control"],
                emit_progress=kwargs["emit_progress"],
            )
        return self.delegate.begin_fetch(*args, **kwargs)


def _receipt_key(storage_dir: Path) -> bytes:
    path = Path(storage_dir) / "artifact-receipt.key"
    try:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
    except FileExistsError:
        key = path.read_bytes()
    else:
        key = os.urandom(32)
        try:
            os.write(descriptor, key)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    if len(key) != 32:
        raise RuntimeError("repo artifact receipt key must be exactly 32 bytes")
    return key


def _current_lifecycle_state(persistence, operation_id: str) -> str:
    accepted = [
        event for event in persistence.lifecycle_events(operation_id)
        if event.accepted
    ]
    return "ABSENT" if not accepted else accepted[-1].to_state


def _transition(persistence, assignment, generation, before, after, suffix):
    persistence.transition(
        event_id=f"{assignment.operation_id}:{suffix}",
        operation_id=assignment.operation_id,
        artifact_digest=assignment.artifact.content_digest,
        generation=generation,
        from_state=before,
        to_state=after,
        detail={"taskId": assignment.task_id},
        event_time_ms=int(time.time() * 1000),
    )


def _execute_repo_store_assignment(repo_app, context) -> dict[str, Any]:
    assignment = decode_store_assignment(
        context.assignment.assignment_payload
    )
    if (
        not assignment.source_payload_name
        or assignment.repo_node != repo_app.provider_name
    ):
        raise ValueError("repo-store-assignment-provider-binding-mismatch")
    persistence = repo_app._persistence
    generation = 1
    identity = ArtifactStorageIdentity(
        content_digest=assignment.artifact.content_digest,
        size_bytes=int(assignment.artifact.size_bytes),
        generation=generation,
    )
    state = _current_lifecycle_state(
        persistence, assignment.operation_id
    )
    if state == "ACTIVE":
        stored = persistence.authenticated_receipt(assignment.operation_id)
        if stored is None:
            raise RuntimeError("active repo artifact is missing its receipt")
        return {
            "schema": RECEIPT_SCHEMA,
            **stored,
            "dataName": (
                f"{repo_app.provider_name.rstrip('/')}/NDNSF-ARTIFACT/"
                f"sha256/{assignment.artifact.content_digest}"
            ),
        }
    if state == "ABSENT":
        _transition(
            persistence, assignment, generation,
            "ABSENT", "QUEUED", "queued",
        )
        state = "QUEUED"
    if state == "QUEUED":
        _transition(
            persistence, assignment, generation,
            "QUEUED", "RECEIVING", "receiving",
        )
        state = "RECEIVING"

    limits = ArtifactLimits()
    limits.max_artifact_bytes = int(repo_app.capacity_bytes)
    limits.max_manifest_pages = 8
    limits.max_manifest_chunks = 65_536
    limits.max_page_entries = max(limits.max_page_entries, limits.max_manifest_chunks)
    limits.max_cryptographic_operations = (
        limits.max_manifest_pages + limits.max_manifest_chunks + 8
    )
    root_wire = fetch_segmented_object(
        assignment.source_root_name,
        timeout_ms=30_000,
        interest_lifetime_ms=10_000,
        init_cwnd=4.0,
    )
    page_wire = fetch_segmented_object(
        assignment.source_page_name,
        timeout_ms=30_000,
        interest_lifetime_ms=10_000,
        init_cwnd=4.0,
    )
    if len(page_wire) != assignment.manifest_page_encoded_bytes:
        raise RuntimeError("repo artifact manifest page size mismatch")
    signed_root = decode_signed_artifact_root(root_wire, limits)
    page = decode_artifact_manifest_page(page_wire, limits)
    chunks = [
        artifact_chunk_from_dict({
            "index": int(child.index),
            "offsetBytes": int(child.offset_bytes),
            "lengthBytes": int(child.length_bytes),
            "digestAlgorithm": str(child.digest_algorithm),
            "digest": str(child.digest),
            "firstSegment": 0,
            "finalSegment": (
                (int(child.length_bytes) - 1)
                // assignment.packet_payload_bytes
            ),
        }, assignment.artifact, limits)
        for child in page.children
    ]
    capability = artifact_capability_from_dict({
        "repoNode": repo_app.provider_name,
        "formatVersions": ["artifact-manifest-v2"],
        "digestAlgorithms": ["sha256"],
        "signatureAlgorithms": ["rsa-sha256"],
        "maxArtifactBytes": int(repo_app.capacity_bytes),
        "maxChunkBytes": 64 * 1024 * 1024,
        "maxRootEncodedBytes": 64 * 1024,
        "maxPageEncodedBytes": 4 * 1024 * 1024,
        "maxPageEntries": int(limits.max_page_entries),
        "maxManifestDepth": 16,
        "supportsResume": True,
        "supportsReplicaReceipts": True,
        "policyEpoch": assignment.artifact.policy_epoch,
    }, limits)
    from ._py_repoclient import ArtifactManifestTrustPolicy
    policy = ArtifactManifestTrustPolicy()
    policy.trusted_publisher_identity = assignment.artifact.publisher_identity
    policy.trusted_key_locator = assignment.publisher_key_locator
    policy.public_key_pem = assignment.publisher_key_pem
    policy.policy_epoch = assignment.artifact.policy_epoch
    policy.evaluation_time_ms = int(time.time() * 1000)
    policy.allowed_digest_algorithms = ["sha256"]
    policy.allowed_signature_algorithms = ["rsa-sha256"]
    verify_artifact_manifest_graph(
        signed_root,
        assignment.artifact,
        [page],
        chunks,
        capability,
        policy,
        limits,
    )

    store = repo_app._artifact_payload_store
    store.begin(identity)
    if not store.is_committed(identity):
        def persist(packet) -> None:
            offset = int(packet.segment) * assignment.packet_payload_bytes
            if offset >= identity.size_bytes:
                raise RuntimeError("repo artifact segment exceeds declared size")
            expected = min(
                len(packet.content), identity.size_bytes - offset
            )
            if expected != len(packet.content):
                raise RuntimeError("repo artifact final segment exceeds size")
            store.write_range(identity, offset, packet.content)

        fetch_adaptive_segmented_data_packets(
            assignment.source_payload_name,
            persist,
            timeout_ms=max(60_000, int(
                assignment.artifact.size_bytes // (256 * 1024)
            ) * 1000),
            interest_lifetime_ms=10_000,
            initial_window=8,
            maximum_window=128,
            maximum_retries=5,
            persistence_backlog_limit=32,
        )
        for chunk in chunks:
            payload = store.read_range(
                identity,
                int(chunk.offset_bytes),
                int(chunk.length_bytes),
            )
            verify_artifact_chunk_payload(chunk, payload)
            store.mark_verified(
                identity,
                int(chunk.offset_bytes),
                int(chunk.length_bytes),
            )
        store.flush(identity)
    if state == "RECEIVING":
        _transition(
            persistence, assignment, generation,
            "RECEIVING", "VERIFIED", "verified",
        )
    committed_at_ms = int(time.time() * 1000)
    receipt = artifact_replica_receipt_from_dict({
        "receiptId": "receipt-" + hashlib.sha256(
            (
                assignment.operation_id + "\0" + repo_app.provider_name
                + "\0" + assignment.artifact.content_digest
            ).encode()
        ).hexdigest(),
        "operationId": assignment.operation_id,
        "repoNode": repo_app.provider_name,
        "artifact": _artifact_dict(assignment.artifact),
        "committedAtMs": committed_at_ms,
        "storageGeneration": generation,
        "policyEpoch": assignment.artifact.policy_epoch,
        "state": "COMMITTED",
    })
    authenticator = repo_app._artifact_receipt_authenticator
    envelope = authenticator.sign(receipt)
    finalization = dict(
        operation_id=assignment.operation_id,
        artifact_digest=assignment.artifact.content_digest,
        generation=generation,
        logical_name=assignment.artifact.logical_name,
        policy_epoch=assignment.artifact.policy_epoch,
        artifact=_artifact_dict(assignment.artifact),
        receipt_id=receipt.receipt_id,
        receipt=envelope.to_dict()["receipt"],
        repo_node=repo_app.provider_name,
        signer_key_id=envelope.signer_key_id,
        authentication_algorithm=envelope.authentication_algorithm,
        signature_hex=envelope.signature.hex(),
        committed_at_ms=committed_at_ms,
    )
    persistence.begin_finalization(**finalization)
    store.finalize(identity)
    persistence.mark_payload_finalized(
        assignment.operation_id, committed_at_ms
    )
    persistence.commit_finalized_artifact(assignment.operation_id)
    persistence.activate_finalized_artifact(assignment.operation_id)

    data_name = (
        f"{repo_app.provider_name.rstrip('/')}/NDNSF-ARTIFACT/"
        f"sha256/{assignment.artifact.content_digest}"
    )
    with repo_app._artifact_file_producer_lock:
        if assignment.artifact.content_digest not in repo_app._artifact_file_producers:
            repo_app._artifact_file_producers[
                assignment.artifact.content_digest
            ] = FileSegmentedObjectProducer(
                data_name,
                str(store.committed_path(identity)),
                signing_identity=repo_app.provider_name,
                max_segment_size=assignment.packet_payload_bytes,
                freshness_ms=60_000,
                digest_signing=True,
            ).start()
    value = envelope.to_dict()
    value["dataName"] = data_name
    return value


def _rehydrate_committed_artifact_producers(repo_app) -> None:
    """Re-publish durable active artifacts after a RepoNode restart."""
    limits = ArtifactLimits()
    for row in repo_app._persistence.active_artifacts():
        artifact = artifact_reference_from_dict(row["artifact"], limits)
        identity = ArtifactStorageIdentity(
            content_digest=artifact.content_digest,
            size_bytes=int(artifact.size_bytes),
            generation=int(row["generation"]),
            digest_algorithm=artifact.digest_algorithm,
        )
        path = repo_app._artifact_payload_store.committed_path(identity)
        if not path.is_file():
            continue
        data_name = (
            f"{repo_app.provider_name.rstrip('/')}/NDNSF-ARTIFACT/"
            f"sha256/{artifact.content_digest}"
        )
        repo_app._artifact_file_producers[artifact.content_digest] = (
            FileSegmentedObjectProducer(
                data_name,
                str(path),
                signing_identity=repo_app.provider_name,
                max_segment_size=_DEFAULT_PACKET_PAYLOAD_BYTES,
                freshness_ms=60_000,
                digest_signing=True,
            ).start()
        )


def install_artifact_collaboration_service(
    repo_app,
    service_name: str | None = None,
    *,
    queue_capacity: int = 4,
) -> str:
    """Register the queued whole-artifact service on one RepoNodeApp."""

    service = str(
        service_name
        or f"{repo_app.service_name.rstrip('/')}/Artifact/v2/STORE"
    )
    repo_app._artifact_file_producers = {}
    repo_app._artifact_file_producer_lock = threading.RLock()
    repo_app._artifact_receipt_authenticator = HmacReceiptAuthenticator(
        repo_app.provider_name,
        f"{repo_app.provider_name.rstrip('/')}/KEY/artifact-receipt",
        _receipt_key(repo_app.storage_dir),
    )
    _rehydrate_committed_artifact_producers(repo_app)

    def acknowledge(payload: bytes):
        try:
            print(
                f"REPO_ARTIFACT_ACK_EVALUATE provider={repo_app.provider_name}",
                flush=True,
            )
            request = json.loads(bytes(payload).decode("utf-8"))
            artifact = dict(request["artifact"])
            size = int(artifact["sizeBytes"])
            if (
                request.get("schema") != "ndnsf-repo-store-request-v1"
                or size < 0
                or size > repo_app.capacity_bytes
            ):
                return AckDecision(
                    status=False, message="repo-store-invalid-request"
                )
            runtime = repo_app._runtime_snapshot()
            depth = int(runtime["queueDepth"]) + int(runtime["inflightWrites"])
            if depth >= int(queue_capacity):
                return AckDecision(
                    status=False, message="repo-store-queue-full"
                )
            available = max(
                0, int(repo_app.capacity_bytes) - int(repo_app._used_bytes)
            )
            decision = AckDecision(
                status=True,
                message="repo-store-offer",
                payload=encode_store_offer_ack(ArtifactStoreOffer(
                    queue_depth=depth,
                    queue_capacity=int(queue_capacity),
                    available_bytes=available,
                    max_artifact_bytes=int(repo_app.capacity_bytes),
                )),
                pending_state_ttl_ms=_repo_pending_state_ttl_ms(size),
            )
            # A whole-artifact collaboration remains selected while the Repo
            # pulls, verifies, and finalizes the immutable payload.  The Core
            # default pending-request TTL is intentionally short for ordinary
            # request/response services; applying that default here expires
            # the consumed ProviderToken during a multi-GB transfer and makes
            # harmless Selection retransmissions look like token replays. Tie
            # the provider-authorized horizon to the advertised artifact size
            # using a conservative 8 MiB/s lower-bound, with bounded slack and
            # the Core's one-hour maximum. Progress/status reporting remains
            # authoritative after selection; this TTL only protects the
            # request/token/duplicate-selection state until the handler ends.
            print(
                "REPO_ARTIFACT_ACK_ACCEPT "
                f"provider={repo_app.provider_name} queueDepth={depth} "
                f"availableBytes={available}",
                flush=True,
            )
            return decision
        except BaseException as error:
            print(
                "REPO_ARTIFACT_ACK_REJECT "
                f"provider={repo_app.provider_name} error={error}",
                flush=True,
            )
            return AckDecision(
                status=False, message="repo-store-invalid-request"
            )

    def execute(context, _request_payload: bytes) -> None:
        assignment = decode_store_assignment(
            context.assignment.assignment_payload
        )
        context.report_operation_status(ServiceOperationStatus(
            operation_id=assignment.operation_id,
            operation="ARTIFACT_STORE",
            state=ServiceOperationState.RUNNING,
            progress_known=False,
            message="queued assignment started",
        ))
        try:
            receipt = _execute_repo_store_assignment(repo_app, context)
            encoded = json.dumps(
                receipt, sort_keys=True, separators=(",", ":")
            ).encode()
            context.report_operation_status(ServiceOperationStatus(
                operation_id=assignment.operation_id,
                operation="ARTIFACT_STORE",
                state=ServiceOperationState.DONE,
                progress_known=True,
                progress=1.0,
                result_reference={
                    "receiptId": receipt["receipt"]["receiptId"],
                    "dataName": receipt["dataName"],
                },
                message="artifact committed and serving",
                sequence=2,
            ))
            if context.role == assignment.coordinator_role:
                values = (
                    context.wait_for(
                        assignment.receipt_scope,
                        assignment.receipt_topic,
                        assignment.requested_replicas - 1,
                        timeout_ms=max(
                            60_000,
                            int(
                                assignment.artifact.size_bytes
                                // (256 * 1024)
                            ) * 1000,
                        ),
                    )
                    if assignment.requested_replicas > 1 else []
                )
                receipts = [receipt] + [
                    json.loads(bytes(item.payload).decode("utf-8"))
                    for item in values
                ]
                unique = {
                    str(item["receipt"]["repoNode"]): item
                    for item in receipts
                }
                if len(unique) != assignment.requested_replicas:
                    raise RuntimeError(
                        "repo artifact receipt aggregation incomplete"
                    )
                context.publish_final_response(json.dumps({
                    "schema": "ndnsf-repo-artifact-publish-result-v1",
                    "operationId": assignment.operation_id,
                    "receipts": [
                        unique[key] for key in sorted(unique)
                    ],
                }, sort_keys=True, separators=(",", ":")).encode())
            else:
                context.publish(
                    assignment.receipt_scope,
                    assignment.receipt_topic + "/"
                    + repo_app.provider_name.strip("/"),
                    encoded,
                )
        except BaseException as error:
            context.report_operation_status(ServiceOperationStatus(
                operation_id=assignment.operation_id,
                operation="ARTIFACT_STORE",
                state=ServiceOperationState.FAILED,
                reason_code="repo-artifact-store-failed",
                message=str(error)[:512],
                sequence=2,
            ))
            context.fail(str(error)[:512])
            raise

    repo_app.provider.add_collaboration_handler(
        service,
        [
            f"artifact-replica-{index}"
            for index in range(_MAX_REPLICA_ROLES)
        ],
        execute,
        acknowledge,
    )
    return service


__all__ = [
    "ArtifactControlMetrics",
    "CollaborationArtifactApiBackend",
    "install_artifact_collaboration_service",
]
