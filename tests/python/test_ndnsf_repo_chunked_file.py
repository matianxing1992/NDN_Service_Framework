#!/usr/bin/env python3

import hashlib
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from py_repoclient.orchestration import DistributedRepo


class MemoryRepoClient:
    publisher_namespace = "/example/user/NDNSF-DISTRIBUTED-REPO/OBJECT"

    def __init__(self) -> None:
        self.objects = {}
        self.manifests = {}
        self.file_chunk_payload_sizes = []

    def publisher_object_name(self, suffix: str) -> str:
        return f"{self.publisher_namespace}/{suffix.lstrip('/')}"

    def _require_publisher_object_name(self, name: str) -> str:
        if not name.startswith(self.publisher_namespace + "/"):
            raise ValueError("object is outside publisher namespace")
        return name

    def store_object(self, *, object_name, payload, object_type,
                     replication_factor, replica_nodes, policy_epoch, metadata):
        del replication_factor, replica_nodes, policy_epoch
        value = bytes(payload)
        manifest = SimpleNamespace(
            object_name=object_name,
            object_type=object_type,
            sha256=hashlib.sha256(value).hexdigest(),
            size=len(value),
            metadata=dict(metadata or {}),
        )
        self.objects[object_name] = value
        self.manifests[object_name] = manifest
        if object_type == "file-chunk":
            self.file_chunk_payload_sizes.append(len(value))
        return manifest

    def fetch_object(self, object_name, manifest=None):
        del manifest
        return self.objects[object_name]


class ChunkedRepoFileTest(unittest.TestCase):
    def test_round_trip_is_bounded_and_content_addressed(self) -> None:
        client = MemoryRepoClient()
        repo = DistributedRepo(client)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "stage.bin"
            payload = (b"0123456789abcdef" * (1024 * 1024 // 16)) * 3 + b"tail"
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            manifest = repo.put_file(
                "qwen/stage-0",
                source,
                chunk_size=1024 * 1024,
                expected_sha256=f"sha256:{digest}",
                expected_size=len(payload),
                object_type="model-stage",
            )
            self.assertEqual(client.file_chunk_payload_sizes,
                             [1024 * 1024, 1024 * 1024, 1024 * 1024, 4])
            self.assertEqual(manifest.metadata["fileSha256"], digest)
            destination = root / "restored.bin"
            repo.get_file(
                "qwen/stage-0",
                destination,
                manifest=manifest,
                expected_sha256=digest,
                expected_size=len(payload),
            )
            self.assertEqual(destination.read_bytes(), payload)

    def test_tampered_chunk_removes_partial_destination(self) -> None:
        client = MemoryRepoClient()
        repo = DistributedRepo(client)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "stage.bin"
            source.write_bytes(b"a" * (1024 * 1024 + 7))
            manifest = repo.put_file(
                "qwen/stage-1", source, chunk_size=1024 * 1024)
            chunk_name = next(
                name for name, value in client.manifests.items()
                if value.object_type == "file-chunk")
            client.objects[chunk_name] = b"tampered"
            destination = root / "restored.bin"
            with self.assertRaises(RuntimeError):
                repo.get_file(
                    "qwen/stage-1", destination, manifest=manifest)
            self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
