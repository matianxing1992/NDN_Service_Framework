"""NDNSF collaboration adapter for artifact replica upload leases."""

from __future__ import annotations

import json
from dataclasses import dataclass
import hashlib
from typing import Any, Iterable

from ._py_repoclient import (
    ArtifactReference,
    ArtifactUploadLease,
    ReplicaLeaseControlFlow,
    artifact_reference_from_dict,
    artifact_upload_lease_from_dict,
)


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


def _lease_payload(lease: ArtifactUploadLease) -> bytes:
    return json.dumps({
        "schema": "ndnsf-repo-upload-lease-v1",
        "leaseId": lease.lease_id,
        "operationId": lease.operation_id,
        "repoNode": lease.repo_node,
        "artifact": _artifact_dict(lease.artifact),
        "reservedBytes": int(lease.reserved_bytes),
        "issuedAtMs": int(lease.issued_at_ms),
        "expiresAtMs": int(lease.expires_at_ms),
        "replayId": lease.replay_id,
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")


def encode_upload_lease_ack(lease: ArtifactUploadLease) -> bytes:
    """Encode one provider-issued upload lease for an authenticated ACK."""

    return _lease_payload(lease)


def decode_upload_lease_assignment(
    payload: bytes, *, now_ms: int
) -> ArtifactUploadLease:
    """Decode and validate the exact provider-side Selection assignment."""

    try:
        value = json.loads(bytes(payload).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("repo-upload-lease-invalid-json") from error
    if not isinstance(value, dict) or value.pop("schema", None) != (
            "ndnsf-repo-upload-lease-v1"):
        raise ValueError("repo-upload-lease-invalid-schema")
    return artifact_upload_lease_from_dict(value, int(now_ms))


@dataclass(frozen=True)
class ArtifactStoreOffer:
    """Advisory ACK metadata; it does not reserve or lock repository capacity."""

    queue_depth: int
    queue_capacity: int
    available_bytes: int
    max_artifact_bytes: int


@dataclass(frozen=True)
class ArtifactStoreAssignment:
    """Exact store task delivered only after ACK_CLOSED plan selection."""

    task_id: str
    operation_id: str
    repo_node: str
    artifact: ArtifactReference
    source_root_name: str = ""
    source_page_name: str = ""
    source_payload_name: str = ""
    publisher_key_pem: str = ""
    publisher_key_locator: str = ""
    packet_payload_bytes: int = 0
    manifest_page_encoded_bytes: int = 0
    receipt_scope: str = ""
    receipt_topic: str = ""
    coordinator_role: str = ""
    requested_replicas: int = 1


@dataclass(frozen=True)
class ReplicaTaskControlSnapshot:
    state: str
    request_id: str
    candidate_count: int
    selected_repo_nodes: tuple[str, ...]
    control_operation_count: int


def encode_store_offer_ack(offer: ArtifactStoreOffer) -> bytes:
    values = (
        int(offer.queue_depth),
        int(offer.queue_capacity),
        int(offer.available_bytes),
        int(offer.max_artifact_bytes),
    )
    if (values[0] < 0 or values[1] <= 0 or values[0] > values[1]
            or values[2] < 0 or values[3] <= 0):
        raise ValueError("repo-store-offer-invalid-bounds")
    return json.dumps({
        "schema": "ndnsf-repo-store-offer-v1",
        "queueDepth": values[0],
        "queueCapacity": values[1],
        "availableBytes": values[2],
        "maxArtifactBytes": values[3],
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")


def decode_store_offer_ack(payload: bytes) -> ArtifactStoreOffer:
    try:
        value = json.loads(bytes(payload).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("repo-store-offer-invalid-json") from error
    if not isinstance(value, dict) or value.pop("schema", None) != (
            "ndnsf-repo-store-offer-v1"):
        raise ValueError("repo-store-offer-invalid-schema")
    expected = {
        "queueDepth", "queueCapacity", "availableBytes", "maxArtifactBytes"
    }
    if set(value) != expected:
        raise ValueError("repo-store-offer-unknown-field")
    offer = ArtifactStoreOffer(
        queue_depth=int(value["queueDepth"]),
        queue_capacity=int(value["queueCapacity"]),
        available_bytes=int(value["availableBytes"]),
        max_artifact_bytes=int(value["maxArtifactBytes"]),
    )
    # Reuse the encoder as the canonical bounds validator.
    encode_store_offer_ack(offer)
    return offer


def _store_assignment_payload(
    operation_id: str,
    repo_node: str,
    artifact: ArtifactReference,
    *,
    source_root_name: str = "",
    source_page_name: str = "",
    source_payload_name: str = "",
    publisher_key_pem: str = "",
    publisher_key_locator: str = "",
    packet_payload_bytes: int = 0,
    manifest_page_encoded_bytes: int = 0,
    receipt_scope: str = "",
    receipt_topic: str = "",
    coordinator_role: str = "",
    requested_replicas: int = 1,
) -> bytes:
    transfer = {
        "sourceRootName": str(source_root_name),
        "sourcePageName": str(source_page_name),
        "sourcePayloadName": str(source_payload_name),
        "publisherKeyPem": str(publisher_key_pem),
        "publisherKeyLocator": str(publisher_key_locator),
        "packetPayloadBytes": int(packet_payload_bytes),
        "manifestPageEncodedBytes": int(manifest_page_encoded_bytes),
        "receiptScope": str(receipt_scope),
        "receiptTopic": str(receipt_topic),
        "coordinatorRole": str(coordinator_role),
        "requestedReplicas": int(requested_replicas),
    }
    schema = (
        "ndnsf-repo-store-assignment-v2"
        if any((
            source_root_name, source_page_name, source_payload_name,
            publisher_key_pem, publisher_key_locator, receipt_scope,
            receipt_topic, coordinator_role,
        ))
        else "ndnsf-repo-store-assignment-v1"
    )
    value = {
        "schema": schema,
        "operationId": str(operation_id),
        "repoNode": str(repo_node),
        "artifact": _artifact_dict(artifact),
    }
    if schema.endswith("-v2"):
        if (
            not all((
                transfer["sourceRootName"],
                transfer["sourcePageName"],
                transfer["sourcePayloadName"],
                transfer["publisherKeyPem"],
                transfer["publisherKeyLocator"],
                transfer["receiptScope"],
                transfer["receiptTopic"],
                transfer["coordinatorRole"],
            ))
            or transfer["packetPayloadBytes"] <= 0
            or transfer["packetPayloadBytes"] > 8800
            or transfer["manifestPageEncodedBytes"] <= 0
            or transfer["manifestPageEncodedBytes"] > 4 * 1024 * 1024
            or transfer["requestedReplicas"] <= 0
            or transfer["requestedReplicas"] > 1024
        ):
            raise ValueError("repo-store-assignment-invalid-transfer-descriptor")
        value["transfer"] = transfer
    task_binding = json.dumps(
        value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    value["taskId"] = hashlib.sha256(task_binding).hexdigest()
    return json.dumps(
        value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def decode_store_assignment(payload: bytes) -> ArtifactStoreAssignment:
    try:
        value = json.loads(bytes(payload).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("repo-store-assignment-invalid-json") from error
    if not isinstance(value, dict):
        raise ValueError("repo-store-assignment-invalid-schema")
    schema = value.pop("schema", None)
    if schema not in {
        "ndnsf-repo-store-assignment-v1",
        "ndnsf-repo-store-assignment-v2",
    }:
        raise ValueError("repo-store-assignment-invalid-schema")
    expected = {"taskId", "operationId", "repoNode", "artifact"}
    if schema.endswith("-v2"):
        expected.add("transfer")
    if set(value) != expected:
        raise ValueError("repo-store-assignment-unknown-field")
    artifact = artifact_reference_from_dict(value["artifact"])
    transfer = dict(value.get("transfer") or {})
    if schema.endswith("-v2") and set(transfer) != {
        "sourceRootName", "sourcePageName", "sourcePayloadName",
        "publisherKeyPem", "publisherKeyLocator", "packetPayloadBytes",
        "manifestPageEncodedBytes", "receiptScope", "receiptTopic",
        "coordinatorRole", "requestedReplicas",
    }:
        raise ValueError("repo-store-assignment-unknown-transfer-field")
    canonical = json.loads(_store_assignment_payload(
        str(value["operationId"]),
        str(value["repoNode"]),
        artifact,
        source_root_name=str(transfer.get("sourceRootName", "")),
        source_page_name=str(transfer.get("sourcePageName", "")),
        source_payload_name=str(transfer.get("sourcePayloadName", "")),
        publisher_key_pem=str(transfer.get("publisherKeyPem", "")),
        publisher_key_locator=str(transfer.get("publisherKeyLocator", "")),
        packet_payload_bytes=int(transfer.get("packetPayloadBytes", 0)),
        manifest_page_encoded_bytes=int(
            transfer.get("manifestPageEncodedBytes", 0)
        ),
        receipt_scope=str(transfer.get("receiptScope", "")),
        receipt_topic=str(transfer.get("receiptTopic", "")),
        coordinator_role=str(transfer.get("coordinatorRole", "")),
        requested_replicas=int(transfer.get("requestedReplicas", 1)),
    ))
    if str(value["taskId"]) != canonical["taskId"]:
        raise ValueError("repo-store-assignment-task-binding-mismatch")
    return ArtifactStoreAssignment(
        task_id=str(value["taskId"]),
        operation_id=str(value["operationId"]),
        repo_node=str(value["repoNode"]),
        artifact=artifact,
        source_root_name=str(transfer.get("sourceRootName", "")),
        source_page_name=str(transfer.get("sourcePageName", "")),
        source_payload_name=str(transfer.get("sourcePayloadName", "")),
        publisher_key_pem=str(transfer.get("publisherKeyPem", "")),
        publisher_key_locator=str(transfer.get("publisherKeyLocator", "")),
        packet_payload_bytes=int(transfer.get("packetPayloadBytes", 0)),
        manifest_page_encoded_bytes=int(
            transfer.get("manifestPageEncodedBytes", 0)
        ),
        receipt_scope=str(transfer.get("receiptScope", "")),
        receipt_topic=str(transfer.get("receiptTopic", "")),
        coordinator_role=str(transfer.get("coordinatorRole", "")),
        requested_replicas=int(transfer.get("requestedReplicas", 1)),
    )


class PendingReplicaTaskCollaboration:
    """One store task selection; positive ACKs are offers, never reservations."""

    def __init__(
        self,
        invocation,
        service_name: str,
        artifact: ArtifactReference,
        requested_replicas: int,
        operation_id: str,
        service_user=None,
    ) -> None:
        self.invocation = invocation
        self.service_name = str(service_name)
        self.artifact = artifact
        self.requested_replicas = int(requested_replicas)
        self.operation_id = str(operation_id)
        self.service_user = service_user
        self._closed = None
        self._selected_repo_nodes: tuple[str, ...] = ()

    def acks_closed(self, timeout_ms: int | None = None):
        if self._closed is None:
            self._closed = self.invocation.acks_closed(timeout_ms)
        return self._closed

    def commit_ack_tasks(
        self, transfer_descriptor: dict[str, Any] | None = None
    ) -> tuple[str, ...]:
        closed = self.acks_closed()
        offers = []
        seen = set()
        successful_count = 0
        excluded = []
        for candidate in closed.candidates:
            if not bool(candidate.status):
                continue
            successful_count += 1
            provider = str(candidate.provider_name)
            if provider in seen:
                raise ValueError("repo-store-offer-duplicate-provider")
            seen.add(provider)
            try:
                offer = decode_store_offer_ack(bytes(candidate.payload))
            except (TypeError, ValueError) as error:
                raise ValueError(
                    "repo-store-offer-invalid-ack: successful ACK lacks "
                    "bounded advisory metadata"
                ) from error
            if (offer.queue_depth >= offer.queue_capacity
                    or offer.available_bytes < int(self.artifact.size_bytes)
                    or offer.max_artifact_bytes < int(self.artifact.size_bytes)):
                if len(excluded) < 8:
                    excluded.append(
                        f"{provider}(queue={offer.queue_depth}/"
                        f"{offer.queue_capacity},"
                        f"availableBytes={offer.available_bytes},"
                        f"maxArtifactBytes={offer.max_artifact_bytes})"
                    )
                continue
            offers.append((offer.queue_depth, provider))
        offers.sort(key=lambda item: (item[0], item[1]))
        selected = tuple(
            provider for _depth, provider in offers[:self.requested_replicas]
        )
        if len(selected) != self.requested_replicas:
            raise ValueError(
                "repo-store-insufficient-cover: "
                f"candidateCount={len(closed.candidates)} "
                f"successfulCount={successful_count} "
                f"eligibleCount={len(offers)} "
                f"requestedReplicas={self.requested_replicas} "
                f"requiredBytes={int(self.artifact.size_bytes)} "
                f"excluded={';'.join(excluded) or 'none'}"
            )
        roles = []
        provider_assignments = {}
        assignment_payloads = {}
        role_names = [
            f"artifact-replica-{index}" for index in range(len(selected))
        ]
        transfer = dict(transfer_descriptor or {})
        receipt_scope = str(transfer.get("receipt_scope", ""))
        scope_key_data_names = {}
        key_scopes = {}
        role_scopes = {}
        if transfer:
            if not receipt_scope:
                raise ValueError("repo-store-assignment-receipt-scope-required")
            if self.service_user is None:
                raise RuntimeError(
                    "repo artifact transfer requires the collaboration user"
                )
            published = self.service_user.publish_encrypted_large_data(
                self.service_name,
                bytes(transfer.pop("receipt_key")),
                object_label=(
                    "repo-artifact-receipts-"
                    + hashlib.sha256(self.operation_id.encode()).hexdigest()[:16]
                ),
                freshness_ms=max(60_000, int(
                    getattr(self.invocation, "timeout_ms", 60_000)
                )),
            )
            if not published.success:
                raise RuntimeError(
                    "repo artifact receipt-scope key publication failed: "
                    + str(published.error)
                )
            key_scopes[receipt_scope] = role_names
            role_scopes = {role: [receipt_scope] for role in role_names}
            scope_key_data_names[receipt_scope] = published.encrypted_data_name
        for index, provider in enumerate(selected):
            role = role_names[index]
            roles.append({
                "role": role,
                "service": self.service_name,
                "artifact": self.artifact.root_manifest_name,
                "allow_dynamic_provisioning": False,
                "provisioning_timeout_ms": 0,
                "app_requirement": b"",
                "assignment_payload": b"",
                "min_providers": 1,
                "max_providers": 1,
            })
            provider_assignments[role] = provider
            assignment_payloads[role] = _store_assignment_payload(
                self.operation_id,
                provider,
                self.artifact,
                coordinator_role=role_names[0] if transfer else "",
                requested_replicas=len(selected),
                **transfer,
            )
        if not self.invocation.commit_plan(
            ack_closed_digest=closed.digest,
            roles=roles,
            key_scopes=key_scopes,
            dependencies=[],
            scope_key_data_names=scope_key_data_names,
            role_scopes=role_scopes,
            role_provider_assignments=provider_assignments,
            assignment_payloads_by_role=assignment_payloads,
        ):
            raise RuntimeError("repo store collaboration plan was not accepted")
        self._selected_repo_nodes = selected
        return selected

    def snapshot(self) -> ReplicaTaskControlSnapshot:
        closed = self.acks_closed()
        return ReplicaTaskControlSnapshot(
            state=("PLAN_COMMITTED" if self._selected_repo_nodes
                   else "ACK_CLOSED"),
            request_id=str(self.invocation.request_id),
            candidate_count=len(closed.candidates),
            selected_repo_nodes=self._selected_repo_nodes,
            control_operation_count=(
                1 + len(self._selected_repo_nodes)
                if self._selected_repo_nodes else 1
            ),
        )

    def result(self, timeout_ms: int | None = None):
        return self.invocation.result(timeout_ms)


class ReplicaTaskCollaborationClient:
    """Start one delayed-planning store-task collaboration per artifact."""

    def __init__(self, service_user, service_name: str) -> None:
        self.service_user = service_user
        self.service_name = str(service_name)
        if not self.service_name:
            raise ValueError("repository collaboration service name is required")

    def begin(
        self,
        artifact: ArtifactReference,
        *,
        requested_replicas: int,
        operation_id: str,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 30000,
        request_id: str = "",
    ) -> PendingReplicaTaskCollaboration:
        artifact_reference_from_dict(_artifact_dict(artifact))
        requested_replicas = int(requested_replicas)
        if requested_replicas <= 0 or requested_replicas > 1024:
            raise ValueError("requested replica count is outside safety bound")
        payload = json.dumps({
            "schema": "ndnsf-repo-store-request-v1",
            "operationId": str(operation_id),
            "artifact": _artifact_dict(artifact),
            "requestedReplicas": requested_replicas,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")
        invocation = self.service_user.begin_collaboration(
            self.service_name,
            payload,
            mode="DEFERRED",
            ack_timeout_ms=int(ack_timeout_ms),
            timeout_ms=int(timeout_ms),
            request_id=str(request_id),
        )
        return PendingReplicaTaskCollaboration(
            invocation,
            self.service_name,
            artifact,
            requested_replicas,
            str(operation_id),
            self.service_user,
        )


class PendingReplicaLeaseCollaboration:
    """One durable NDNSF invocation from Request through lease Selection."""

    def __init__(
        self,
        invocation,
        service_name: str,
        artifact: ArtifactReference,
        requested_replicas: int,
        operation_id: str,
    ) -> None:
        self.invocation = invocation
        self.service_name = service_name
        self.artifact = artifact
        self.requested_replicas = int(requested_replicas)
        self.operation_id = str(operation_id)
        self._control = ReplicaLeaseControlFlow()
        self._control.begin_collaboration(str(invocation.request_id))
        self._closed = None

    def acks_closed(self, timeout_ms: int | None = None):
        if self._closed is None:
            self._closed = self.invocation.acks_closed(timeout_ms)
            self._control.close_acks(len(self._closed.candidates))
        return self._closed

    def commit_leases(
        self,
        leases: Iterable[ArtifactUploadLease],
        *,
        now_ms: int,
    ) -> bool:
        closed = self.acks_closed()
        selected = list(leases)
        successful_providers = {
            str(candidate.provider_name)
            for candidate in closed.candidates
            if bool(candidate.status)
        }
        if not selected or len(selected) > self.requested_replicas:
            raise ValueError(
                "repo-lease-selection-count: selected leases must satisfy request"
            )
        if any(lease.repo_node not in successful_providers for lease in selected):
            raise ValueError(
                "repo-lease-selection-outside-ack: lease provider lacks successful ACK"
            )
        if any(lease.operation_id != self.operation_id for lease in selected):
            raise ValueError(
                "repo-lease-operation-mismatch: ACK lease binds another operation"
            )
        if any(lease.artifact.to_dict() != self.artifact.to_dict()
               for lease in selected):
            raise ValueError(
                "repo-lease-artifact-mismatch: ACK lease binds another artifact"
            )

        # Validate identity, expiry, uniqueness, and control-count invariants
        # on a candidate state machine before emitting any Selection. The
        # authoritative snapshot advances only if NDNSF accepts the commit.
        committed_control = ReplicaLeaseControlFlow()
        committed_control.begin_collaboration(str(self.invocation.request_id))
        committed_control.close_acks(len(closed.candidates))
        committed_control.commit_plan(selected, int(now_ms))
        roles = []
        provider_assignments: dict[str, str] = {}
        assignment_payloads: dict[str, bytes] = {}
        for index, lease in enumerate(selected):
            role = f"artifact-replica-{index}"
            roles.append({
                "role": role,
                "service": self.service_name,
                "artifact": self.artifact.root_manifest_name,
                "allow_dynamic_provisioning": False,
                "provisioning_timeout_ms": 30000,
                "app_requirement": b"",
                "assignment_payload": b"",
                "min_providers": 1,
                "max_providers": 1,
            })
            provider_assignments[role] = lease.repo_node
            assignment_payloads[role] = _lease_payload(lease)
        accepted = bool(self.invocation.commit_plan(
            ack_closed_digest=closed.digest,
            roles=roles,
            key_scopes={},
            dependencies=[],
            role_provider_assignments=provider_assignments,
            assignment_payloads_by_role=assignment_payloads,
        ))
        if accepted:
            self._control = committed_control
        return accepted

    def leases_from_acks(self, *, now_ms: int) -> tuple[ArtifactUploadLease, ...]:
        """Validate provider-issued ACK leases and return a deterministic subset."""

        closed = self.acks_closed()
        leases = []
        seen = set()
        for candidate in sorted(
                closed.candidates, key=lambda item: str(item.provider_name)):
            if not bool(candidate.status):
                continue
            try:
                lease = decode_upload_lease_assignment(
                    bytes(candidate.payload), now_ms=int(now_ms)
                )
            except (TypeError, ValueError) as error:
                raise ValueError(
                    "repo-lease-invalid-ack: successful ACK lacks a valid lease"
                ) from error
            provider = str(candidate.provider_name)
            if lease.repo_node != provider or provider in seen:
                raise ValueError(
                    "repo-lease-provider-mismatch: ACK lease/provider identity differs"
                )
            if lease.operation_id != self.operation_id:
                raise ValueError(
                    "repo-lease-operation-mismatch: ACK lease binds another operation"
                )
            if lease.artifact.to_dict() != self.artifact.to_dict():
                raise ValueError(
                    "repo-lease-artifact-mismatch: ACK lease binds another artifact"
                )
            seen.add(provider)
            leases.append(lease)
            if len(leases) == self.requested_replicas:
                break
        if len(leases) != self.requested_replicas:
            raise ValueError(
                "repo-lease-insufficient-cover: successful ACK leases do not "
                "cover requested replicas"
            )
        return tuple(leases)

    def commit_ack_leases(self, *, now_ms: int) -> tuple[ArtifactUploadLease, ...]:
        leases = self.leases_from_acks(now_ms=now_ms)
        if not self.commit_leases(leases, now_ms=now_ms):
            raise RuntimeError("repo collaboration plan was not accepted")
        return leases

    def snapshot(self):
        return self._control.snapshot()

    def result(self, timeout_ms: int | None = None):
        return self.invocation.result(timeout_ms)


class ReplicaLeaseCollaborationClient:
    """Start one delayed-planning NDNSF collaboration per artifact operation."""

    def __init__(self, service_user, service_name: str) -> None:
        self.service_user = service_user
        self.service_name = str(service_name)
        if not self.service_name:
            raise ValueError("repository collaboration service name is required")

    def begin(
        self,
        artifact: ArtifactReference,
        *,
        requested_replicas: int,
        operation_id: str,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 30000,
        request_id: str = "",
    ) -> PendingReplicaLeaseCollaboration:
        artifact_reference_from_dict(_artifact_dict(artifact))
        requested_replicas = int(requested_replicas)
        if requested_replicas <= 0 or requested_replicas > 1024:
            raise ValueError("requested replica count is outside safety bound")
        payload = json.dumps({
            "schema": "ndnsf-repo-replica-request-v1",
            "operationId": str(operation_id),
            "artifact": _artifact_dict(artifact),
            "requestedReplicas": requested_replicas,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")
        invocation = self.service_user.begin_collaboration(
            self.service_name,
            payload,
            mode="DEFERRED",
            ack_timeout_ms=int(ack_timeout_ms),
            timeout_ms=int(timeout_ms),
            request_id=str(request_id),
        )
        return PendingReplicaLeaseCollaboration(
            invocation,
            self.service_name,
            artifact,
            requested_replicas,
            str(operation_id),
        )


__all__ = [
    "ArtifactStoreAssignment",
    "ArtifactStoreOffer",
    "ReplicaTaskControlSnapshot",
    "PendingReplicaTaskCollaboration",
    "ReplicaTaskCollaborationClient",
    "decode_store_assignment",
    "decode_store_offer_ack",
    "encode_store_offer_ack",
    "decode_upload_lease_assignment",
    "encode_upload_lease_ack",
    "PendingReplicaLeaseCollaboration",
    "ReplicaLeaseCollaborationClient",
]
