from __future__ import annotations

import hashlib
import json
import os
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
    PendingReplicaTaskCollaboration,
    SqliteRepositoryPersistence,
    artifact_reference_from_dict,
    decode_store_assignment,
    encode_store_offer_ack,
)
from py_repoclient.artifact_api import ArtifactDescriptor
from py_repoclient.artifact_transfer import _store_assignment_payload
from py_repoclient.network_artifact_backend import (
    _PreparedArtifactSource,
    _NetworkFetchDriver,
    _artifact_adaptive_fetch_options,
    _execute_repo_store_assignment,
    _rehydrate_committed_artifact_producers,
)
from ndnsf import make_segmented_data_packets, inspect_data_name


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


def test_collaboration_backend_close_releases_native_user():
    user = SimpleNamespace(stop_calls=0)

    def stop():
        user.stop_calls += 1

    user.stop = stop
    backend = CollaborationArtifactApiBackend(None, user, "/repo/store")

    backend.close()

    assert user.stop_calls == 1
    assert backend.control is None


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
    def test_from_config_refreshes_until_repo_service_permission_is_ready(self):
        service_name = "/NDNSF/DistributedRepo/Artifact/v2/STORE"

        class FakeServiceUser:
            instance = None

            def __init__(self, **_kwargs):
                self.refresh_count = 0
                self.pump_calls = []
                FakeServiceUser.instance = self

            def get_allowed_services(self):
                if self.refresh_count == 0:
                    return []
                return [SimpleNamespace(service=service_name)]

            def refresh_permissions(self):
                self.refresh_count += 1

            def pump(self, milliseconds):
                self.pump_calls.append(milliseconds)

        deployment = SimpleNamespace(
            deployment=SimpleNamespace(
                group="/group",
                controller="/controller",
                trust_schema="trust-schema.conf",
            )
        )
        with tempfile.TemporaryDirectory() as temporary, mock.patch(
            "ndnsf.ServiceUser", FakeServiceUser
        ), mock.patch(
            "ndnsf_distributed_inference.app.APPDeployment.from_config",
            return_value=deployment,
        ):
            backend = CollaborationArtifactApiBackend.from_config(
                "policy.yaml",
                generated_policy_dir=Path(temporary) / "generated",
                state_root=Path(temporary) / "state",
                user="/example/user",
                bootstrap_token="token",
            )

        self.assertIsNotNone(backend)
        self.assertEqual(FakeServiceUser.instance.refresh_count, 1)
        self.assertEqual(FakeServiceUser.instance.pump_calls, [6000])

    def test_insufficient_store_cover_reports_capacity_exclusion(self):
        artifact = artifact_reference_from_dict({
            "logicalName": "/artifact/spec168/diagnostic",
            "digestAlgorithm": "sha256",
            "contentDigest": "1" * 64,
            "sizeBytes": 1024,
            "formatVersion": "artifact-manifest-v2",
            "rootManifestName": "/artifact/spec168/diagnostic/root",
            "publisherIdentity": "/publisher",
            "policyEpoch": "default",
        })
        invocation = SimpleNamespace(
            request_id="request-diagnostic",
            acks_closed=lambda _timeout_ms=None: SimpleNamespace(
                digest="sha256:" + "2" * 64,
                candidates=(SimpleNamespace(
                    provider_name="/repo/small",
                    status=True,
                    payload=encode_store_offer_ack(ArtifactStoreOffer(
                        queue_depth=0,
                        queue_capacity=8,
                        available_bytes=512,
                        max_artifact_bytes=512,
                    )),
                ),),
            ),
        )
        pending = PendingReplicaTaskCollaboration(
            invocation,
            "/NDNSF/DistributedRepo/Artifact/v2/STORE",
            artifact,
            requested_replicas=1,
            operation_id="operation-diagnostic",
        )

        with self.assertRaisesRegex(
            ValueError,
            r"candidateCount=1.*eligibleCount=0.*requiredBytes=1024.*"
            r"availableBytes=512.*maxArtifactBytes=512",
        ):
            pending.commit_ack_tasks()

    def test_large_artifact_fetch_profile_is_bounded_for_concurrent_shards(self):
        options = _artifact_adaptive_fetch_options(19 * 1024**3)
        self.assertEqual(options["interest_lifetime_ms"], 30_000)
        self.assertEqual(options["initial_window"], 4)
        self.assertEqual(options["maximum_window"], 16)
        self.assertEqual(options["maximum_retries"], 8)
        self.assertEqual(options["persistence_backlog_limit"], 16)
        self.assertLessEqual(
            options["maximum_window"] * 3,
            48,
            "three concurrent shard pulls must not recreate the old 96-interest fan-out",
        )
        with mock.patch.dict(
            "os.environ",
            {
                "NDNSF_ARTIFACT_FETCH_INITIAL_WINDOW": "not-an-int",
                "NDNSF_ARTIFACT_FETCH_MAXIMUM_WINDOW": "9999",
                "NDNSF_ARTIFACT_FETCH_BACKLOG": "0",
                "NDNSF_ARTIFACT_FETCH_INTEREST_LIFETIME_MS": "-1",
                "NDNSF_ARTIFACT_FETCH_MAX_RETRIES": "0",
            },
            clear=False,
        ):
            clamped = _artifact_adaptive_fetch_options(19 * 1024**3)
        self.assertEqual(clamped["initial_window"], 4)
        self.assertEqual(clamped["maximum_window"], 32)
        self.assertEqual(clamped["persistence_backlog_limit"], 4)
        self.assertEqual(clamped["interest_lifetime_ms"], 10_000)
        self.assertEqual(clamped["maximum_retries"], 5)

    def test_network_fetch_emits_segment_progress_and_final_coverage(self):
        payload = b"segment-progress" * 700
        artifact = artifact_reference_from_dict({
            "logicalName": "/artifact/spec164/fetch-progress",
            "digestAlgorithm": "sha256",
            "contentDigest": hashlib.sha256(payload).hexdigest(),
            "sizeBytes": len(payload),
            "formatVersion": "artifact-manifest-v2",
            "rootManifestName": "/artifact/spec164/fetch-progress/root",
            "publisherIdentity": "/publisher",
            "policyEpoch": "default",
        })
        receipt = {
            "receipt": {
                "artifact": {"contentDigest": artifact.content_digest},
                "repoNode": "/repo/a",
            },
            "dataName": "/repo/a/payload",
        }
        backend = SimpleNamespace(
            packet_payload_bytes=7600,
            last_receipts=[receipt],
        )
        events = []
        fetch_options = {}

        def fetch_payload(_name, on_packet, **_kwargs):
            fetch_options.update(_kwargs)
            delivered = 0
            for segment, offset in enumerate(range(0, len(payload), 7600)):
                content = payload[offset:offset + 7600]
                on_packet(SimpleNamespace(segment=segment, content=content))
                delivered += 1
            return SimpleNamespace(
                total_segments=delivered,
                delivered_segments=delivered,
                logical_bytes=len(payload),
                retransmitted_bytes=0,
            )

        class NoCancellation:
            def raise_if_cancelled(self, *_args):
                return None

        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "stage.bin"
            with mock.patch(
                "py_repoclient.network_artifact_backend."
                "fetch_adaptive_segmented_data_packets",
                side_effect=fetch_payload,
            ), mock.patch(
                "py_repoclient.network_artifact_backend."
                "_PROGRESS_CHECKPOINT_BYTES",
                1,
            ):
                driver = _NetworkFetchDriver(
                    backend,
                    artifact,
                    destination,
                    "fetch-progress-operation",
                    resume=True,
                    emit_progress=events.append,
                )
                driver.transfer(NoCancellation())
                result = driver.commit()
                committed_payload = destination.read_bytes()

        self.assertEqual(result.transferred_bytes, len(payload))
        self.assertEqual(committed_payload, payload)
        self.assertTrue(events)
        final = events[-1]
        self.assertEqual(final.received_bytes, len(payload))
        self.assertEqual(final.delivered_segments, final.total_segments)
        self.assertGreater(final.total_segments, 0)
        self.assertEqual(final.last_segment, final.total_segments - 1)
        self.assertEqual(fetch_options["initial_window"], 4)
        self.assertEqual(fetch_options["maximum_window"], 16)
        self.assertEqual(fetch_options["persistence_backlog_limit"], 16)
        self.assertEqual(fetch_options["interest_lifetime_ms"], 30_000)
        self.assertEqual(fetch_options["maximum_retries"], 8)

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
            for name in (prepared.payload_name, prepared.root_name, prepared.page_name):
                self.assertTrue(name.startswith(inspect_data_name("/publisher")["canonical_name"] + "/"))
                self.assertNotIn("NDNSF-ARTIFACT-SOURCE", name)
            from ndn.encoding import Name, Component
            payload_components = Name.from_str(prepared.payload_name)
            self.assertEqual(bytes(Component.get_value(payload_components[1])), b"NDNSF")
            self.assertEqual(bytes(Component.get_value(payload_components[2])), b"blob")
            self.assertTrue(bytes(Component.get_value(payload_components[3])).startswith(b"name=artifact-payload:"))
            self.assertEqual([len(Component.get_value(value)) for value in payload_components[4:6]], [32, 32])
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
                _activate_serving_prefix=mock.Mock(),
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

            original_packets = {}
            with mock.patch.dict(os.environ, {
                    "NDN_CLIENT_PIB": "pib-sqlite3:" + str(root / "pib"),
                    "NDN_CLIENT_TPM": "tpm-file:" + str(root / "tpm")}):
                for name, payload in ((prepared.root_name, prepared.root_path.read_bytes()),
                                      (prepared.page_name, prepared.page_path.read_bytes()),
                                      (prepared.payload_name, source_bytes)):
                    original_packets[name] = make_segmented_data_packets(
                        name, payload, signing_identity="/publisher",
                        max_segment_size=prepared.packet_payload_bytes, freshness_ms=60_000)

            def fetch_payload(name, on_packet, **_kwargs):
                packets = original_packets[name]
                for packet in reversed(packets):
                    on_packet(packet)
                    self.assertIsNone(persistence.lookup_original_packet(
                        inspect_data_name(packet.name)["canonical_name"], False))
                return SimpleNamespace(delivered_segments=len(packets), total_segments=len(packets))

            try:
                with mock.patch(
                    "py_repoclient.network_artifact_backend."
                    "fetch_adaptive_segmented_data_packets",
                    side_effect=fetch_payload,
                ), mock.patch(
                    "py_repoclient.network_artifact_backend."
                    "FileSegmentedObjectProducer",
                ) as republisher:
                    receipt = _execute_repo_store_assignment(repo, context)
                    republisher.assert_not_called()
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
                self.assertEqual(receipt["dataName"], inspect_data_name(
                    original_packets[prepared.payload_name][0].name)["segment_prefix"])
                repo._activate_serving_prefix.reset_mock()
                self.assertEqual(_execute_repo_store_assignment(repo, context), receipt)
                def rebound_context(value):
                    transfer = value["transfer"]
                    payload = _store_assignment_payload(value["operationId"], value["repoNode"],
                        artifact_reference_from_dict(value["artifact"]),
                        source_root_name=transfer["sourceRootName"],
                        source_page_name=transfer["sourcePageName"],
                        source_payload_name=transfer["sourcePayloadName"],
                        publisher_key_pem=transfer["publisherKeyPem"],
                        publisher_key_locator=transfer["publisherKeyLocator"],
                        packet_payload_bytes=transfer["packetPayloadBytes"],
                        manifest_page_encoded_bytes=transfer["manifestPageEncodedBytes"],
                        receipt_scope=transfer["receiptScope"], receipt_topic=transfer["receiptTopic"],
                        coordinator_role=transfer["coordinatorRole"],
                        requested_replicas=transfer["requestedReplicas"])
                    return SimpleNamespace(assignment=SimpleNamespace(assignment_payload=payload))
                for field, value in (("contentDigest", "f" * 64),
                                     ("publisherIdentity", "/other-publisher")):
                    conflicting = json.loads(assignment_payload)
                    conflicting["artifact"][field] = value
                    bad_context = rebound_context(conflicting)
                    repo._activate_serving_prefix.reset_mock()
                    with self.assertRaisesRegex(ValueError, "artifact-conflict"):
                        _execute_repo_store_assignment(repo, bad_context)
                    repo._activate_serving_prefix.assert_not_called()
                alternate = json.loads(assignment_payload)
                alternate["repoNode"] = inspect_data_name(alternate["repoNode"])["canonical_name"]
                for field in ("publisherIdentity", "rootManifestName"):
                    alternate["artifact"][field] = inspect_data_name(alternate["artifact"][field])["canonical_name"]
                alternate["transfer"]["sourcePayloadName"] = inspect_data_name(
                    alternate["transfer"]["sourcePayloadName"])["alternate_name"]
                alias_context = rebound_context(alternate)
                self.assertEqual(_execute_repo_store_assignment(repo, alias_context), receipt)
                conflicting = json.loads(assignment_payload)
                conflicting["transfer"]["sourcePayloadName"] += "/8=other"
                bad_context = rebound_context(conflicting)
                repo._activate_serving_prefix.reset_mock()
                with self.assertRaisesRegex(ValueError, "source-conflict"):
                    _execute_repo_store_assignment(repo, bad_context)
                repo._activate_serving_prefix.assert_not_called()
                for packets in original_packets.values():
                    for packet in packets:
                        row = persistence.lookup_original_packet(
                            inspect_data_name(packet.name)["canonical_name"], False)
                        self.assertEqual(bytes(row[1]), packet.wire)
                persistence.close()
                persistence = SqliteRepositoryPersistence(root / "repo.sqlite3", "spec164-network-test")
                repo._persistence = persistence
                repo._activate_serving_prefix.reset_mock()
                with mock.patch("py_repoclient.network_artifact_backend.FileSegmentedObjectProducer") as republisher:
                    _rehydrate_committed_artifact_producers(repo)
                    republisher.assert_not_called()
                self.assertGreaterEqual(repo._activate_serving_prefix.call_count, 3)
                for packets in original_packets.values():
                    for packet in packets:
                        self.assertEqual(bytes(persistence.lookup_original_packet(
                            inspect_data_name(packet.name)["canonical_name"], False)[1]), packet.wire)
            finally:
                persistence.close()
                prepared.stop()


if __name__ == "__main__":
    unittest.main()
