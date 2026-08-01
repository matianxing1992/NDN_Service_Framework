#!/usr/bin/env python3
"""Contract tests for the Spec 164 public artifact API."""

from __future__ import annotations

import asyncio
import hashlib
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

from py_repoclient import (
    ArtifactApiError,
    ArtifactCancellationToken,
    ArtifactControlMode,
    ArtifactControlOptions,
    ArtifactDescriptor,
    ArtifactErrorCode,
    ArtifactFetchResult,
    ArtifactProgress,
    ArtifactPublishResult,
    ArtifactReference,
    ArtifactReplicaResult,
    ArtifactRepositoryApi,
    ArtifactSessionStatus,
    RepoClient,
)


class FakePublishDriver:
    def __init__(self, backend, descriptor, operation_id, emit):
        self.backend = backend
        self.descriptor = descriptor
        self.operation_id = operation_id
        self.emit = emit
        self.payload = b""
        self.state = "OPEN"
        self.abort_calls = []

    def _progress(self, sequence, committed=False):
        size = int(self.descriptor.reference.size_bytes)
        progress = ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.descriptor.reference,
            phase="commit" if committed else "transfer",
            received_bytes=size,
            verified_bytes=size,
            committed_bytes=size if committed else 0,
            total_bytes=size,
            selected_replicas=self.descriptor.requested_replicas,
            committed_replicas=(
                self.descriptor.requested_replicas if committed else 0
            ),
            retransmitted_bytes=0,
            sequence=sequence,
            timestamp_ms=1000 + sequence,
        )
        self.emit(progress)
        return progress

    def transfer(self, path, cancellation):
        cancellation.raise_if_cancelled(
            self.operation_id, self.descriptor.reference
        )
        if self.backend.timeout_on_publish:
            raise TimeoutError("simulated bounded transfer timeout")
        self.payload = Path(path).read_bytes()
        self._progress(1)
        if self.backend.non_monotonic:
            self._progress(1)
        self.state = "TRANSFERRED"

    def status(self):
        return ArtifactSessionStatus(
            self.operation_id,
            "PUBLISH",
            self.state,
            self.descriptor.reference,
        )

    def commit(self):
        reference = self.descriptor.reference
        digest = reference.content_digest
        deduplicated = digest in self.backend.payloads
        self.backend.payloads[digest] = self.payload
        self._progress(2, committed=True)
        self.state = "COMMITTED"
        replicas = tuple(
            ArtifactReplicaResult(
                repo_node=f"/repo/{index}",
                state="COMMITTED",
                receipt_id=f"{self.operation_id}-receipt-{index}",
            )
            for index in range(self.descriptor.requested_replicas)
        )
        return ArtifactPublishResult(
            reference=reference,
            operation_id=self.operation_id,
            requested_replicas=self.descriptor.requested_replicas,
            achieved_replicas=self.descriptor.requested_replicas,
            replicas=replicas,
            deduplicated=deduplicated,
            resumed=False,
            total_duration_ms=1.0,
            phase_durations_ms={"transfer": 0.5, "commit": 0.5},
        )

    def abort(self, preserve_progress):
        self.abort_calls.append(bool(preserve_progress))
        self.state = "CANCELLED"
        return self.status()


class FakeFetchDriver:
    def __init__(
        self, backend, reference, destination, operation_id, emit
    ):
        self.backend = backend
        self.reference = reference
        self.destination = destination
        self.operation_id = operation_id
        self.emit = emit
        self.state = "OPEN"
        self.reused = 0
        self.transferred = 0
        self.abort_calls = []

    def transfer(self, cancellation):
        cancellation.raise_if_cancelled(
            self.operation_id, self.reference
        )
        payload = self.backend.payloads[self.reference.content_digest]
        if (
            self.destination.is_file()
            and hashlib.sha256(self.destination.read_bytes()).hexdigest()
            == self.reference.content_digest
        ):
            self.reused = len(payload)
        else:
            temporary = Path(str(self.destination) + ".part")
            temporary.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_bytes(payload)
            temporary.replace(self.destination)
            self.transferred = len(payload)
        self.emit(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.reference,
            phase="transfer",
            received_bytes=len(payload),
            verified_bytes=len(payload),
            committed_bytes=len(payload),
            total_bytes=len(payload),
            selected_replicas=1,
            committed_replicas=1,
            retransmitted_bytes=0,
            sequence=1,
            timestamp_ms=2001,
        ))
        self.state = "TRANSFERRED"

    def status(self):
        return ArtifactSessionStatus(
            self.operation_id,
            "FETCH",
            self.state,
            self.reference,
        )

    def commit(self):
        self.state = "COMMITTED"
        return ArtifactFetchResult(
            reference=self.reference,
            operation_id=self.operation_id,
            destination=self.destination,
            reused_bytes=self.reused,
            transferred_bytes=self.transferred,
            source_replicas=("/repo/0",),
            total_duration_ms=1.0,
            phase_durations_ms={"transfer": 1.0},
        )

    def abort(self, preserve_progress):
        self.abort_calls.append(bool(preserve_progress))
        self.state = "CANCELLED"
        return self.status()


class FakeBackend:
    def __init__(self):
        self.payloads = {}
        self.publish_drivers = []
        self.fetch_drivers = []
        self.publish_descriptors = []
        self.fetch_options = []
        self.non_monotonic = False
        self.timeout_on_publish = False

    def begin_publish(self, descriptor, operation_id, emit_progress):
        self.publish_descriptors.append(descriptor)
        driver = FakePublishDriver(
            self, descriptor, operation_id, emit_progress
        )
        self.publish_drivers.append(driver)
        return driver

    def begin_fetch(
        self,
        reference,
        destination,
        operation_id,
        *,
        resume,
        verify,
        replace,
        timeout_ms,
        control,
        emit_progress,
    ):
        self.fetch_options.append({
            "resume": resume,
            "verify": verify,
            "replace": replace,
            "timeout_ms": timeout_ms,
            "control": control,
        })
        driver = FakeFetchDriver(
            self,
            reference,
            destination,
            operation_id,
            emit_progress,
        )
        self.fetch_drivers.append(driver)
        return driver


class PublicArtifactApiTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.payload = b"spec164-public-api-payload" * 8
        self.source = self.root / "source.bin"
        self.source.write_bytes(self.payload)
        self.digest = hashlib.sha256(self.payload).hexdigest()
        self.backend = FakeBackend()
        self.api = ArtifactRepositoryApi(
            self.backend, publisher_identity="/publisher"
        )

    def tearDown(self):
        self.temporary.cleanup()

    def publish(self, **kwargs):
        return self.api.publish_file(
            self.source,
            name="/models/example/stage-0",
            expected_sha256=self.digest,
            replicas=kwargs.pop("replicas", 2),
            **kwargs,
        )

    def test_sync_publish_fetch_idempotency_dedup_and_replica_results(self):
        first = self.publish(idempotency_key="stable-publication")
        second = self.publish(idempotency_key="stable-publication")
        self.assertEqual(first.operation_id, second.operation_id)
        self.assertFalse(first.deduplicated)
        self.assertTrue(second.deduplicated)
        self.assertEqual(first.achieved_replicas, 2)
        self.assertEqual(
            len({item.receipt_id for item in first.replicas}), 2
        )

        destination = self.root / "fetched.bin"
        fetched = self.api.fetch_file(first.reference, destination)
        reused = self.api.fetch_file(first.reference, destination)
        self.assertEqual(fetched.transferred_bytes, len(self.payload))
        self.assertEqual(reused.reused_bytes, len(self.payload))
        self.assertEqual(destination.read_bytes(), self.payload)

    def test_artifact_reference_has_validated_public_constructor_and_dict(self):
        reference = ArtifactReference(
            logical_name="/models/constructed",
            content_digest=self.digest,
            size_bytes=len(self.payload),
            root_manifest_name=(
                f"/models/constructed/root/sha256={self.digest}"
            ),
            publisher_identity="/publisher",
            policy_epoch="epoch-1",
        )
        self.assertEqual(reference.to_dict()["contentDigest"], self.digest)
        with self.assertRaisesRegex(ValueError, "artifact-invalid-digest"):
            ArtifactReference(
                logical_name="/models/invalid",
                content_digest="not-a-digest",
                size_bytes=1,
                root_manifest_name="/models/invalid/root",
                publisher_identity="/publisher",
                policy_epoch="epoch-1",
            )

    def test_repo_client_exposes_only_public_control_selection(self):
        client = RepoClient(
            SimpleNamespace(user="/publisher"),
            artifact_backend=self.backend,
        )
        control = ArtifactControlOptions(
            ArtifactControlMode.TARGETED, "/repo/selected"
        )
        published = client.publish_file(
            self.source,
            name="/models/example/stage-0",
            expected_sha256=self.digest,
            control=control,
        )
        self.assertEqual(
            self.backend.publish_descriptors[-1].control, control
        )
        client.fetch_file(
            published.reference,
            self.root / "client-fetch.bin",
            control=control,
        )
        self.assertEqual(
            self.backend.fetch_options[-1]["control"], control
        )

    def test_advanced_session_is_deterministic_and_abort_is_idempotent(self):
        published = self.publish(replicas=1)
        descriptor = ArtifactDescriptor(
            published.reference,
            requested_replicas=1,
            idempotency_key="advanced",
        )
        session = self.api.begin_upload(descriptor)
        with self.assertRaisesRegex(
            ArtifactApiError, "RECOVERY_REQUIRED"
        ):
            session.commit()
        session.upload_file(self.source)
        committed = session.commit()
        self.assertIs(session.commit(), committed)
        status = session.abort()
        self.assertEqual(status.state, "COMMITTED")
        self.assertEqual(self.backend.publish_drivers[-1].abort_calls, [])

        fetch = self.api.begin_fetch(
            published.reference,
            self.root / "advanced-fetch.bin",
            idempotency_key="advanced-fetch",
        )
        first_abort = fetch.abort(preserve_progress=False)
        second_abort = fetch.abort(preserve_progress=True)
        self.assertEqual(first_abort, second_abort)
        self.assertEqual(
            self.backend.fetch_drivers[-1].abort_calls, [False]
        )

    def test_progress_is_monotonic_and_blocked_observer_does_not_block_engine(self):
        release = threading.Event()
        entered = threading.Event()

        def blocked(_progress):
            entered.set()
            release.wait(2)

        started = time.monotonic()
        result = self.publish(on_progress=blocked, replicas=1)
        elapsed = time.monotonic() - started
        self.assertEqual(result.achieved_replicas, 1)
        self.assertLess(elapsed, 0.5)
        self.assertTrue(entered.wait(0.5))
        release.set()

        self.backend.non_monotonic = True
        with self.assertRaisesRegex(
            ArtifactApiError, "progress is not monotonic"
        ):
            self.publish(idempotency_key="bad-progress", replicas=1)

    def test_cancellation_and_stable_errors_are_public(self):
        token = ArtifactCancellationToken()
        token.cancel()
        with self.assertRaises(ArtifactApiError) as captured:
            self.publish(cancellation=token, replicas=1)
        self.assertEqual(captured.exception.code, ArtifactErrorCode.CANCELLED)
        self.assertEqual(
            self.backend.publish_drivers[-1].abort_calls, [True]
        )

        with self.assertRaises(ArtifactApiError) as captured:
            self.api.publish_file(
                self.source,
                name="/models/example/stage-0",
                expected_sha256="0" * 64,
            )
        self.assertEqual(
            captured.exception.code,
            ArtifactErrorCode.CONTENT_DIGEST_MISMATCH,
        )

        unavailable = ArtifactRepositoryApi(
            None, publisher_identity="/publisher"
        )
        with self.assertRaises(ArtifactApiError) as captured:
            unavailable.publish_file(
                self.source,
                name="/models/example/stage-0",
                expected_sha256=self.digest,
            )
        self.assertEqual(
            captured.exception.code,
            ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
        )

        self.backend.timeout_on_publish = True
        with self.assertRaises(ArtifactApiError) as captured:
            self.publish(idempotency_key="timeout", replicas=1)
        self.assertEqual(
            captured.exception.code, ArtifactErrorCode.TRANSFER_TIMEOUT
        )
        self.assertNotIn("simulated", str(captured.exception))

        with self.assertRaisesRegex(
            ValueError, "distinct committed receipts"
        ):
            ArtifactPublishResult(
                reference=self.backend.publish_descriptors[0].reference,
                operation_id="invalid-durability",
                requested_replicas=1,
                achieved_replicas=1,
                replicas=(),
            )


class PublicArtifactAsyncApiTest(unittest.IsolatedAsyncioTestCase):
    async def test_async_publish_and_fetch(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            payload = b"async-spec164"
            source = root / "source.bin"
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            backend = FakeBackend()
            api = ArtifactRepositoryApi(
                backend, publisher_identity="/publisher"
            )
            published = await api.publish_file_async(
                source,
                name="/models/async",
                expected_sha256=digest,
            )
            fetched = await api.fetch_file_async(
                published.reference, root / "destination.bin"
            )
            self.assertEqual(fetched.transferred_bytes, len(payload))
            self.assertEqual(
                (root / "destination.bin").read_bytes(), payload
            )


class PublicArtifactNativeBuildContractTest(unittest.TestCase):
    def test_repo_extension_declares_ndn_svs_public_dependency(self):
        root = Path(__file__).resolve().parents[2]
        setup_source = (
            root
            / "NDNSF-DistributedRepo"
            / "pythonWrapper"
            / "setup.py"
        ).read_text(encoding="utf-8")
        self.assertIn('"libndn-cxx"', setup_source)
        self.assertIn('"libndn-svs"', setup_source)

    def test_tiger_overlay_probes_current_native_symbols(self):
        root = Path(__file__).resolve().parents[2]
        dockerfile = (
            root
            / "specs"
            / "162-itiger-qwen36-generation"
            / "jobs"
            / "Dockerfile.spec164-native-overlay"
        ).read_text(encoding="utf-8")
        for symbol in (
            "FileSegmentedObjectProducer",
            "fetch_adaptive_segmented_data_packets",
            "AdaptiveArtifactTransfer",
            "ArtifactRepositoryApi",
            "APPDeployment",
            "state_root",
        ):
            self.assertIn(symbol, dockerfile)
        self.assertGreaterEqual(
            dockerfile.count("SPEC164_NATIVE_RUNTIME_PASS"), 2
        )


if __name__ == "__main__":
    unittest.main()
