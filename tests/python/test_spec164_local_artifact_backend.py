#!/usr/bin/env python3
"""Public local-backend persistence and resume tests for Spec 164."""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

from py_repoclient import (
    ArtifactApiError,
    ArtifactCancellationToken,
    ArtifactErrorCode,
    ArtifactRepositoryApi,
    FilesystemArtifactApiBackend,
)


class CancelAfterFirstBlock(ArtifactCancellationToken):
    def __init__(self):
        super().__init__()
        self.checks = 0

    def raise_if_cancelled(self, operation_id, artifact=None):
        self.checks += 1
        # The public session checks once before entering the driver, and the
        # driver checks before each block.  Cancel before the second block so
        # one verified staging block remains available for exact-key resume.
        if self.checks > 2:
            self.cancel()
        return super().raise_if_cancelled(operation_id, artifact)


class LocalArtifactBackendTest(unittest.TestCase):
    def test_cross_process_shape_dedup_atomic_fetch_and_conflict(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            payload = b"persistent-local-artifact"
            source = root / "source.bin"
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            first = ArtifactRepositoryApi(
                FilesystemArtifactApiBackend(root / "store"),
                publisher_identity="/publisher",
            )
            published = first.publish_file(
                source,
                name="/artifacts/local",
                expected_sha256=digest,
            )

            reopened = ArtifactRepositoryApi(
                FilesystemArtifactApiBackend(root / "store"),
                publisher_identity="/publisher",
            )
            duplicate = reopened.publish_file(
                source,
                name="/artifacts/local",
                expected_sha256=digest,
            )
            self.assertTrue(duplicate.deduplicated)
            destination = root / "destination.bin"
            fetched = reopened.fetch_file(
                published.reference, destination
            )
            self.assertEqual(fetched.transferred_bytes, len(payload))
            self.assertEqual(destination.read_bytes(), payload)
            reused = reopened.fetch_file(
                published.reference, destination
            )
            self.assertEqual(reused.reused_bytes, len(payload))

            destination.write_bytes(b"conflict")
            with self.assertRaises(ArtifactApiError) as captured:
                reopened.fetch_file(published.reference, destination)
            self.assertEqual(
                captured.exception.code,
                ArtifactErrorCode.DESTINATION_CONFLICT,
            )
            replaced = reopened.fetch_file(
                published.reference, destination, replace=True
            )
            self.assertEqual(replaced.transferred_bytes, len(payload))
            self.assertEqual(destination.read_bytes(), payload)

    def test_cancelled_publication_resumes_verified_prefix(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            payload = bytes(range(251)) * 10_000
            source = root / "large.bin"
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            api = ArtifactRepositoryApi(
                FilesystemArtifactApiBackend(root / "store"),
                publisher_identity="/publisher",
            )
            with self.assertRaises(ArtifactApiError) as captured:
                api.publish_file(
                    source,
                    name="/artifacts/resume",
                    expected_sha256=digest,
                    idempotency_key="resumable",
                    cancellation=CancelAfterFirstBlock(),
                )
            self.assertEqual(
                captured.exception.code, ArtifactErrorCode.CANCELLED
            )
            completed = api.publish_file(
                source,
                name="/artifacts/resume",
                expected_sha256=digest,
                idempotency_key="resumable",
            )
            self.assertTrue(completed.resumed)
            self.assertEqual(completed.achieved_replicas, 1)


if __name__ == "__main__":
    unittest.main()
