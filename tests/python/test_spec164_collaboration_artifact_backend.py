from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

from py_repoclient import (
    ArtifactPublishResult,
    ArtifactReplicaResult,
    ArtifactRepositoryApi,
    ArtifactSessionStatus,
    ArtifactStoreOffer,
    CollaborationArtifactApiBackend,
    HmacReceiptAuthenticator,
    SqliteRepositoryPersistence,
    artifact_reference_from_dict,
    decode_store_assignment,
    encode_store_offer_ack,
)
from py_repoclient.artifact_api import ArtifactDescriptor
from py_repoclient.artifact_transfer import _store_assignment_payload
from py_repoclient.network_artifact_backend import (
    _PreparedArtifactSource,
    _execute_repo_store_assignment,
)


class FakeInvocation:
    request_id = "request-1"

    def __init__(self, payload: bytes):
        request = json.loads(payload)
        self.operation_id = request["operationId"]
        self.artifact = request["artifact"]
        self.ack = SimpleNamespace(
            provider_name="/repo/a",
            status=True,
            payload=encode_store_offer_ack(ArtifactStoreOffer(
                queue_depth=2,
                queue_capacity=8,
                available_bytes=1 << 30,
                max_artifact_bytes=1 << 30,
            )),
        )
        self.commit_calls = []

    def acks_closed(self, _timeout_ms=None):
        return SimpleNamespace(
            digest="sha256:" + "1" * 64,
            candidates=(self.ack,),
        )

    def commit_plan(self, **kwargs):
        self.commit_calls.append(kwargs)
        return True

    def result(self, _timeout_ms=None):
        return SimpleNamespace(status=True, payload=b"task queued")


class FakeUser:
    def __init__(self):
        self.begin_calls = []
        self.invocation = None

    def begin_collaboration(self, service, payload, **kwargs):
        self.begin_calls.append((service, payload, kwargs))
        self.invocation = FakeInvocation(payload)
        return self.invocation


class DelegateDriver:
    def __init__(self, descriptor, operation_id):
        self.descriptor = descriptor
        self.operation_id = operation_id
        self.transferred = False

    def transfer(self, path, cancellation):
        cancellation.raise_if_cancelled(
            self.operation_id, self.descriptor.reference
        )
        self.transferred = Path(path).is_file()

    def status(self):
        return ArtifactSessionStatus(
            self.operation_id,
            "PUBLISH",
            "VERIFIED" if self.transferred else "OPEN",
            self.descriptor.reference,
        )

    def commit(self):
        if not self.transferred:
            raise RuntimeError("delegate transfer did not run")
        return ArtifactPublishResult(
            reference=self.descriptor.reference,
            operation_id=self.operation_id,
            requested_replicas=1,
            achieved_replicas=1,
            replicas=(ArtifactReplicaResult(
                repo_node="/repo/a",
                state="COMMITTED",
                receipt_id="receipt-1",
            ),),
        )

    def abort(self, _preserve_progress):
        return self.status()


class DelegateBackend:
    def __init__(self):
        self.driver = None

    def begin_publish(self, descriptor, operation_id, _emit_progress):
        self.driver = DelegateDriver(descriptor, operation_id)
        return self.driver

    def begin_fetch(self, *args, **kwargs):
        raise AssertionError("fetch is outside this publication-control test")


class CollaborationArtifactBackendTests(unittest.TestCase):
    def test_public_publish_assigns_task_without_ack_reservation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "artifact.bin"
            source.write_bytes(b"spec164-control-adapter")
            user = FakeUser()
            delegate = DelegateBackend()
            backend = CollaborationArtifactApiBackend(
                delegate,
                user,
                "/NDNSF/DistributedRepo/Store",
                ack_timeout_ms=300,
                clock_ms=lambda: 2000,
            )
            api = ArtifactRepositoryApi(
                backend, publisher_identity="/publisher"
            )
            result = api.publish_file(
                source,
                name="/artifact/spec164/control",
                expected_sha256=hashlib.sha256(
                    source.read_bytes()
                ).hexdigest(),
                replicas=1,
                idempotency_key="stable-control",
            )

            self.assertEqual(result.achieved_replicas, 1)
            self.assertTrue(delegate.driver.transferred)
            self.assertEqual(len(user.begin_calls), 1)
            self.assertEqual(len(user.invocation.commit_calls), 1)
            assignment = decode_store_assignment(
                user.invocation.commit_calls[0][
                    "assignment_payloads_by_role"
                ]["artifact-replica-0"]
            )
            self.assertEqual(assignment.repo_node, "/repo/a")
            ack_value = json.loads(user.invocation.ack.payload)
            self.assertNotIn("leaseId", ack_value)
            self.assertNotIn("reservedBytes", ack_value)
            self.assertEqual(
                backend.last_control_metrics.control_operation_count, 2
            )
            self.assertEqual(
                backend.last_control_metrics.lifecycle_phase_count, 3
            )
            self.assertEqual(
                backend.last_control_metrics.selected_replicas, 1
            )

    def test_v2_assignment_binds_real_network_source_descriptor(self):
        artifact = artifact_reference_from_dict({
            "logicalName": "/artifact/spec164/network",
            "digestAlgorithm": "sha256",
            "contentDigest": "ab" * 32,
            "sizeBytes": 1234,
            "formatVersion": "artifact-manifest-v2",
            "rootManifestName": "/artifact/spec164/network/root",
            "publisherIdentity": "/publisher",
            "policyEpoch": "default",
        })
        payload = _store_assignment_payload(
            "operation-network",
            "/repo/a",
            artifact,
            source_root_name="/publisher/source/root",
            source_page_name="/publisher/source/page/0",
            source_payload_name="/publisher/source/payload",
            publisher_key_pem="PUBLIC KEY",
            publisher_key_locator="/publisher/KEY/1",
            packet_payload_bytes=7600,
            manifest_page_encoded_bytes=1024,
            receipt_scope="receipt-scope",
            receipt_topic="/receipt",
            coordinator_role="artifact-replica-0",
            requested_replicas=2,
        )
        assignment = decode_store_assignment(payload)
        self.assertEqual(assignment.source_payload_name,
                         "/publisher/source/payload")
        self.assertEqual(assignment.packet_payload_bytes, 7600)
        self.assertEqual(assignment.requested_replicas, 2)
        corrupted = json.loads(payload)
        corrupted["transfer"]["sourcePayloadName"] += "/substituted"
        with self.assertRaisesRegex(ValueError, "task-binding-mismatch"):
            decode_store_assignment(json.dumps(corrupted).encode())

    def test_repo_task_fetches_verifies_commits_and_serves_whole_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_path = root / "source.bin"
            source_bytes = bytes(range(251)) * 1000
            source_path.write_bytes(source_bytes)
            artifact = artifact_reference_from_dict({
                "logicalName": "/artifact/spec164/real-network",
                "digestAlgorithm": "sha256",
                "contentDigest": hashlib.sha256(source_bytes).hexdigest(),
                "sizeBytes": len(source_bytes),
                "formatVersion": "artifact-manifest-v2",
                "rootManifestName": "/artifact/spec164/real-network/root",
                "publisherIdentity": "/publisher",
                "policyEpoch": "default",
            })
            prepared = _PreparedArtifactSource(
                source_path,
                ArtifactDescriptor(
                    artifact, 1, "real-network", timeout_ms=60_000
                ),
                "operation-real-network",
                packet_payload_bytes=7600,
                chunk_bytes=30_400,
            )
            persistence = SqliteRepositoryPersistence(
                root / "repo.sqlite3", "spec164-network-test"
            )
            repo = SimpleNamespace(
                provider_name="/repo/a",
                capacity_bytes=1 << 30,
                _persistence=persistence,
                _artifact_payload_store=persistence.artifact_payload_store,
                _artifact_receipt_authenticator=HmacReceiptAuthenticator(
                    "/repo/a", "/repo/a/KEY/receipt", b"R" * 32
                ),
                _artifact_file_producers={},
                _artifact_file_producer_lock=threading.RLock(),
            )
            assignment_payload = _store_assignment_payload(
                "operation-real-network",
                "/repo/a",
                artifact,
                source_root_name=prepared.root_name,
                source_page_name=prepared.page_name,
                source_payload_name=prepared.payload_name,
                publisher_key_pem=prepared.public_key_pem,
                publisher_key_locator=prepared.key_locator,
                packet_payload_bytes=prepared.packet_payload_bytes,
                manifest_page_encoded_bytes=prepared.page_encoded_bytes,
                receipt_scope="receipt-scope",
                receipt_topic="/receipt",
                coordinator_role="artifact-replica-0",
                requested_replicas=1,
            )
            context = SimpleNamespace(
                assignment=SimpleNamespace(
                    assignment_payload=assignment_payload
                )
            )

            def fetch_small(name, **_kwargs):
                if name == prepared.root_name:
                    return prepared.root_path.read_bytes()
                if name == prepared.page_name:
                    return prepared.page_path.read_bytes()
                raise AssertionError(name)

            def fetch_payload(name, on_packet, **_kwargs):
                self.assertEqual(name, prepared.payload_name)
                delivered = 0
                for segment, offset in enumerate(
                    range(0, len(source_bytes), prepared.packet_payload_bytes)
                ):
                    content = source_bytes[
                        offset:offset + prepared.packet_payload_bytes
                    ]
                    on_packet(SimpleNamespace(
                        segment=segment, content=content
                    ))
                    delivered += 1
                return SimpleNamespace(delivered_segments=delivered)

            class FakeProducer:
                def __init__(self, *_args, **_kwargs):
                    self.started = False

                def start(self):
                    self.started = True
                    return self

                def stop(self):
                    self.started = False

            try:
                with mock.patch(
                    "py_repoclient.network_artifact_backend."
                    "fetch_segmented_object",
                    side_effect=fetch_small,
                ), mock.patch(
                    "py_repoclient.network_artifact_backend."
                    "fetch_adaptive_segmented_data_packets",
                    side_effect=fetch_payload,
                ), mock.patch(
                    "py_repoclient.network_artifact_backend."
                    "FileSegmentedObjectProducer",
                    FakeProducer,
                ):
                    receipt = _execute_repo_store_assignment(repo, context)
                self.assertEqual(
                    receipt["receipt"]["artifact"]["contentDigest"],
                    artifact.content_digest,
                )
                self.assertEqual(
                    [
                        item.to_state for item in
                        persistence.lifecycle_events(
                            "operation-real-network"
                        ) if item.accepted
                    ],
                    [
                        "QUEUED", "RECEIVING", "VERIFIED",
                        "COMMITTED", "ACTIVE",
                    ],
                )
                committed = next(
                    persistence.artifact_payload_store.root_path.glob(
                        "payloads/sha256/*/*"
                    )
                )
                self.assertEqual(committed.read_bytes(), source_bytes)
                self.assertIn(artifact.content_digest,
                              repo._artifact_file_producers)
            finally:
                persistence.close()
                prepared.stop()


if __name__ == "__main__":
    unittest.main()
