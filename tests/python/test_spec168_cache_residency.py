#!/usr/bin/env python3
"""Spec 168 content-addressed Provider residency contract."""

from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

from ndnsf_distributed_inference.artifact_deployment import (
    ProviderResidencyIdentity,
    ProviderResidencyLedger,
)


DIGEST_A = "sha256:" + "a" * 64
DIGEST_B = "sha256:" + "b" * 64
DIGEST_C = "sha256:" + "c" * 64
DIGEST_D = "sha256:" + hashlib.sha256(b"model-stage").hexdigest()


def identity(**changes) -> ProviderResidencyIdentity:
    values = {
        "model_content_digest": DIGEST_A,
        "graph_digest": DIGEST_B,
        "partition_digest": DIGEST_C,
        "artifact_digest": DIGEST_D,
        "adapter_id": "qwen-transformers",
        "adapter_version": "1",
        "backend": "transformers-cuda",
        "device": "cuda:0",
        "provider_boot_epoch": "boot-1",
    }
    values.update(changes)
    return ProviderResidencyIdentity(**values)


class ProviderResidencyLedgerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.ledger = ProviderResidencyLedger(
            self.root, provider_boot_epoch="boot-1")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_content_addressed_disk_view_never_duplicates_payload(self) -> None:
        artifact = identity()
        canonical = self.ledger.content_path(artifact, "stage.pt")
        canonical.parent.mkdir(parents=True)
        canonical.write_bytes(b"model-stage")
        self.ledger.admit_disk(
            artifact, canonical, size=11, unique_bytes=11, wire_bytes=17)

        view = self.ledger.create_view(artifact, self.root / "run/stage.pt")
        self.assertTrue(view.is_symlink())
        self.assertEqual(view.resolve(), canonical.resolve())
        self.assertEqual(self.ledger.lookup(artifact).tier, "DISK")
        counters = self.ledger.snapshot()["counters"]
        self.assertEqual(counters["repoUniqueBytes"], 11)
        self.assertEqual(counters["repoWireBytes"], 17)
        self.assertEqual(counters["duplicatePayloadBytes"], 0)

    def test_disk_ram_gpu_promotions_and_exact_reuse_are_counted(self) -> None:
        artifact = identity()
        canonical = self.ledger.content_path(artifact, "stage.pt")
        canonical.parent.mkdir(parents=True)
        canonical.write_bytes(b"model-stage")
        self.ledger.admit_disk(artifact, canonical, size=11)
        ram_object = object()
        gpu_object = object()
        self.ledger.promote_ram(artifact, ram_object, bytes_loaded=11)
        self.ledger.promote_gpu(
            artifact, gpu_object, bytes_loaded=11,
            load_completed=True, warmup_completed=True,
            cpu_fallback_count=0)

        hit = self.ledger.acquire(artifact, owner="request-1")
        self.assertEqual(hit.tier, "GPU")
        self.assertIs(hit.resource, gpu_object)
        snapshot = self.ledger.snapshot()
        self.assertEqual(snapshot["counters"]["ramLoadCount"], 1)
        self.assertEqual(snapshot["counters"]["deviceLoadCount"], 1)
        self.assertEqual(snapshot["counters"]["gpuHitCount"], 1)
        with self.assertRaisesRegex(RuntimeError, "owned"):
            self.ledger.evict(artifact, tier="GPU")
        self.ledger.release(artifact, owner="request-1")
        self.ledger.evict(artifact, tier="GPU")
        self.assertEqual(self.ledger.lookup(artifact).tier, "RAM")

    def test_restart_device_and_identity_changes_fail_closed_by_tier(self) -> None:
        artifact = identity()
        canonical = self.ledger.content_path(artifact, "stage.pt")
        canonical.parent.mkdir(parents=True)
        canonical.write_bytes(b"model-stage")
        self.ledger.admit_disk(artifact, canonical, size=11)
        self.ledger.promote_ram(artifact, object(), bytes_loaded=11)
        self.ledger.promote_gpu(
            artifact, object(), bytes_loaded=11,
            load_completed=True, warmup_completed=True,
            cpu_fallback_count=0)

        self.assertEqual(
            self.ledger.lookup(identity(device="cuda:1")).tier, "RAM")
        self.ledger.rebind_boot_epoch("boot-2")
        self.assertEqual(
            self.ledger.lookup(identity(provider_boot_epoch="boot-2")).tier,
            "DISK")
        for field, value in (
            ("model_content_digest", "sha256:" + "1" * 64),
            ("graph_digest", "sha256:" + "2" * 64),
            ("partition_digest", "sha256:" + "3" * 64),
            ("artifact_digest", "sha256:" + "4" * 64),
            ("adapter_id", "other-adapter"),
            ("adapter_version", "2"),
            ("backend", "tensorrt-cuda"),
        ):
            with self.subTest(field=field):
                self.assertIsNone(self.ledger.lookup(identity(
                    provider_boot_epoch="boot-2", **{field: value})))
        self.assertEqual(
            identity().to_dict()["schema"],
            "ndnsf-di-provider-residency-identity-v1",
        )

    def test_gpu_promotion_rejects_untruthful_runtime_evidence(self) -> None:
        artifact = identity()
        canonical = self.ledger.content_path(artifact, "stage.pt")
        canonical.parent.mkdir(parents=True)
        canonical.write_bytes(b"model-stage")
        self.ledger.admit_disk(artifact, canonical, size=11)
        cases = (
            {"load_completed": False, "warmup_completed": True,
             "cpu_fallback_count": 0},
            {"load_completed": True, "warmup_completed": False,
             "cpu_fallback_count": 0},
            {"load_completed": True, "warmup_completed": True,
             "cpu_fallback_count": 1},
        )
        for values in cases:
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.ledger.promote_gpu(
                    artifact, object(), bytes_loaded=11, **values)


if __name__ == "__main__":
    unittest.main()
