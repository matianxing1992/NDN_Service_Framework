#!/usr/bin/env python3
"""Spec 164 T006 streaming CAS and transactional metadata tests."""

from __future__ import annotations

import hashlib
import sqlite3
import tempfile
from pathlib import Path
import unittest

from py_repoclient.persistence import (
    ArtifactStorageIdentity,
    FilesystemCasPayloadStore,
    SqliteRepositoryPersistence,
)


def identity_for(payload: bytes, generation: int = 1) -> ArtifactStorageIdentity:
    return ArtifactStorageIdentity(
        content_digest=hashlib.sha256(payload).hexdigest(),
        size_bytes=len(payload),
        generation=generation,
    )


class StreamingArtifactStoreTests(unittest.TestCase):
    def test_out_of_order_ranges_merge_and_survive_reopen(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "cas"
            payload = bytes((index * 31 + 11) & 0xFF for index in range(2_000_033))
            identity = identity_for(payload)
            store = FilesystemCasPayloadStore(root)
            store.begin(identity)

            split = 1_250_000
            store.write_range(identity, split, payload[split:])
            store.mark_verified(identity, split, len(payload) - split)
            with self.assertRaisesRegex(
                    RuntimeError, "^repo-artifact-incomplete:"):
                store.finalize(identity)

            store.write_range(identity, 0, payload[:split])
            store.mark_verified(identity, 0, split)
            self.assertEqual(
                store.verified_ranges(identity), ((0, len(payload)),)
            )
            self.assertEqual(
                store.read_range(identity, 12345, 65537),
                payload[12345:12345 + 65537],
            )
            committed = store.finalize(identity)
            self.assertTrue(committed.is_file())
            self.assertFalse(store.staging_path(identity).exists())

            reopened = FilesystemCasPayloadStore(root)
            resumed_identity = ArtifactStorageIdentity(
                content_digest=identity.content_digest,
                size_bytes=identity.size_bytes,
                generation=99,
            )
            self.assertTrue(reopened.is_committed(resumed_identity))
            self.assertEqual(
                reopened.read_range(resumed_identity, 0, len(payload)), payload
            )

    def test_corruption_is_rejected_before_committed_visibility(self):
        with tempfile.TemporaryDirectory() as temporary:
            expected = b"expected" * 1024
            corrupt = b"corrupt!" * 1024
            identity = identity_for(expected)
            store = FilesystemCasPayloadStore(Path(temporary) / "cas")
            store.begin(identity)
            store.write_range(identity, 0, corrupt)
            store.mark_verified(identity, 0, len(corrupt))
            with self.assertRaisesRegex(
                    RuntimeError, "^repo-artifact-digest-mismatch:"):
                store.finalize(identity)
            self.assertFalse(store.is_committed(identity))
            self.assertTrue(store.staging_path(identity).is_file())
            store.abort(identity)
            self.assertFalse(store.staging_path(identity).exists())

    def test_range_progress_is_compact_and_not_one_sqlite_row_per_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "repo.sqlite3"
            persistence = SqliteRepositoryPersistence(database, "test-owner")
            try:
                payload = b"x" * (128 * 100)
                identity = identity_for(payload)
                store = persistence.artifact_payload_store
                self.assertEqual(
                    store.backend_kind, "artifact-manifest-v2/filesystem-cas"
                )
                store.begin(identity)
                for index in reversed(range(100)):
                    offset = index * 128
                    store.write_range(identity, offset, payload[offset:offset + 128])
                    store.mark_verified(identity, offset, 128)
                self.assertEqual(
                    store.verified_ranges(identity), ((0, len(payload)),)
                )
                range_map = Path(str(store.staging_path(identity)) + ".ranges")
                self.assertEqual(range_map.stat().st_size, 28)

                tables = {
                    row[0] for row in persistence.connection.execute(
                        "SELECT name FROM sqlite_master WHERE type='table'"
                    )
                }
                self.assertNotIn("artifact_packets", tables)
                lifecycle_rows = persistence.connection.execute(
                    "SELECT COUNT(*) FROM artifact_lifecycle_journal"
                ).fetchone()[0]
                self.assertEqual(lifecycle_rows, 0)
            finally:
                persistence.close()

            with sqlite3.connect(database) as reopened:
                self.assertEqual(
                    reopened.execute(
                        "SELECT COUNT(*) FROM artifact_lifecycle_journal"
                    ).fetchone()[0],
                    0,
                )

    def test_empty_artifact_finalizes_without_synthetic_range(self):
        with tempfile.TemporaryDirectory() as temporary:
            identity = identity_for(b"")
            store = FilesystemCasPayloadStore(Path(temporary) / "cas")
            store.begin(identity)
            self.assertEqual(store.verified_ranges(identity), ())
            committed = store.finalize(identity)
            self.assertEqual(committed.read_bytes(), b"")
            self.assertTrue(store.is_committed(identity))

    def test_configured_range_limit_bounds_one_operation(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = b"x" * 8192
            identity = identity_for(payload)
            store = FilesystemCasPayloadStore(
                Path(temporary) / "cas", max_range_bytes=4096
            )
            store.begin(identity)
            with self.assertRaisesRegex(
                    ValueError, "^repo-artifact-range-too-large:"):
                store.write_range(identity, 0, payload)
            with self.assertRaisesRegex(
                    ValueError, "^repo-artifact-range-too-large:"):
                store.read_range(identity, 0, len(payload))


if __name__ == "__main__":
    unittest.main()
