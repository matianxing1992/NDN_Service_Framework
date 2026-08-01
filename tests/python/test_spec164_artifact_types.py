#!/usr/bin/env python3
"""Contract tests for Spec 164 canonical large-artifact types."""

from __future__ import annotations

import copy
import unittest

from py_repoclient import (
    ArtifactLimits,
    ArtifactValidationError,
    artifact_capability_from_dict,
    artifact_chunk_from_dict,
    artifact_manifest_page_from_dict,
    artifact_reference_from_dict,
    artifact_replica_receipt_from_dict,
    artifact_root_manifest_from_dict,
    artifact_upload_lease_from_dict,
)


DIGEST = "ab" * 32
OTHER_DIGEST = "cd" * 32


def reference_values(**overrides):
    values = {
        "logicalName": "/publisher/models/qwen",
        "digestAlgorithm": "sha256",
        "contentDigest": DIGEST,
        "sizeBytes": 8192,
        "formatVersion": "artifact-manifest-v2",
        "rootManifestName": "/publisher/models/qwen/manifest/v=2",
        "publisherIdentity": "/publisher",
        "policyEpoch": "epoch-1",
    }
    values.update(overrides)
    return values


def capability_values(**overrides):
    values = {
        "repoNode": "/repo/node-1",
        "formatVersions": ["artifact-manifest-v2", "exact-packet-v1"],
        "digestAlgorithms": ["sha256"],
        "signatureAlgorithms": ["rsa-sha256", "ed25519"],
        "maxArtifactBytes": 1 << 30,
        "maxChunkBytes": 1 << 20,
        "maxRootEncodedBytes": 1 << 16,
        "maxPageEncodedBytes": 1 << 20,
        "maxPageEntries": 4096,
        "maxManifestDepth": 8,
        "supportsResume": True,
        "supportsReplicaReceipts": True,
        "policyEpoch": "epoch-1",
    }
    values.update(overrides)
    return values


def child_values(index, offset, length, **overrides):
    values = {
        "kind": "chunk",
        "index": index,
        "offsetBytes": offset,
        "lengthBytes": length,
        "digestAlgorithm": "sha256",
        "digest": DIGEST if index == 0 else OTHER_DIGEST,
    }
    values.update(overrides)
    return values


class ArtifactTypesTests(unittest.TestCase):
    def assert_error(self, code, function, *args, **kwargs):
        with self.assertRaises(ArtifactValidationError) as raised:
            function(*args, **kwargs)
        self.assertTrue(str(raised.exception).startswith(f"{code}:"),
                        str(raised.exception))

    def test_reference_is_validated_read_only_and_content_addressable(self):
        reference = artifact_reference_from_dict(reference_values())
        alias = artifact_reference_from_dict(
            reference_values(logicalName="/catalog/qwen-alias")
        )
        changed = artifact_reference_from_dict(
            reference_values(contentDigest=OTHER_DIGEST)
        )

        self.assertEqual(reference.size_bytes, 8192)
        self.assertTrue(reference.same_bytes(alias))
        self.assertFalse(reference.same_bytes(changed))
        with self.assertRaises(AttributeError):
            reference.size_bytes = 1

    def test_reference_rejects_unknown_fields_and_bad_digest(self):
        values = reference_values()
        values["typoField"] = "fail closed"
        self.assert_error(
            "artifact-invalid-manifest", artifact_reference_from_dict, values
        )
        self.assert_error(
            "artifact-invalid-digest",
            artifact_reference_from_dict,
            reference_values(contentDigest="not-a-sha256"),
        )

    def test_capability_negotiates_exact_algorithms_and_hard_limits(self):
        reference = artifact_reference_from_dict(reference_values())
        capability = artifact_capability_from_dict(capability_values())
        self.assertTrue(capability.supports(reference, "ed25519"))
        self.assertFalse(capability.supports(reference, "ecdsa-sha256"))

        self.assert_error(
            "artifact-invalid-capability",
            artifact_capability_from_dict,
            capability_values(digestAlgorithms=["sha256", "sha256"]),
        )
        self.assert_error(
            "artifact-unsupported-algorithm",
            artifact_capability_from_dict,
            capability_values(signatureAlgorithms=["hmac-sha256"]),
        )
        self.assert_error(
            "artifact-limit-exceeded",
            artifact_capability_from_dict,
            capability_values(maxChunkBytes=(1 << 26) + 1),
        )

    def test_root_manifest_requires_bounded_publicly_signed_geometry(self):
        values = {
            "artifact": reference_values(),
            "packetPayloadBytes": 8000,
            "chunkBytes": 8192,
            "namingTemplate": "/publisher/models/qwen/chunk={chunk}/seg={segment}",
            "manifestRootDigestAlgorithm": "sha256",
            "manifestRootDigest": DIGEST,
            "signatureAlgorithm": "ed25519",
            "publisherKeyLocator": "/publisher/KEY/key-id",
            "createdAtMs": 1000,
            "expiresAtMs": 2000,
            "criticalExtensions": ["receipt-v1"],
        }
        manifest = artifact_root_manifest_from_dict(values, encoded_bytes=1024)
        self.assertEqual(manifest.signature_algorithm, "ed25519")

        bad_signature = copy.deepcopy(values)
        bad_signature["signatureAlgorithm"] = "hmac-sha256"
        self.assert_error(
            "artifact-unsupported-algorithm",
            artifact_root_manifest_from_dict,
            bad_signature,
            1024,
        )
        self.assert_error(
            "artifact-limit-exceeded",
            artifact_root_manifest_from_dict,
            values,
            (1 << 16) + 1,
        )

    def test_manifest_page_is_bounded_before_decode_and_gap_free(self):
        values = {
            "pageVersion": "artifact-manifest-page-v2",
            "depth": 1,
            "offsetBytes": 0,
            "lengthBytes": 8192,
            "pageDigestAlgorithm": "sha256",
            "pageDigest": DIGEST,
            "children": [
                child_values(0, 0, 4096),
                child_values(1, 4096, 4096),
            ],
        }
        page = artifact_manifest_page_from_dict(values, encoded_bytes=512)
        self.assertEqual(len(page.children), 2)

        gap = copy.deepcopy(values)
        gap["children"][1]["offsetBytes"] = 4097
        self.assert_error(
            "artifact-invalid-range",
            artifact_manifest_page_from_dict,
            gap,
            512,
        )

        limits = ArtifactLimits()
        limits.max_page_entries = 1
        self.assert_error(
            "artifact-limit-exceeded",
            artifact_manifest_page_from_dict,
            values,
            512,
            limits,
        )

    def test_chunk_range_is_constrained_by_artifact_and_segments(self):
        reference = artifact_reference_from_dict(reference_values())
        values = {
            "index": 0,
            "offsetBytes": 0,
            "lengthBytes": 4096,
            "digestAlgorithm": "sha256",
            "digest": DIGEST,
            "firstSegment": 0,
            "finalSegment": 1,
        }
        chunk = artifact_chunk_from_dict(values, reference)
        self.assertEqual(chunk.length_bytes, 4096)

        overflow = dict(values, offsetBytes=4096, lengthBytes=8192)
        self.assert_error(
            "artifact-invalid-range",
            artifact_chunk_from_dict,
            overflow,
            reference,
        )
        reversed_segments = dict(values, firstSegment=2, finalSegment=1)
        self.assert_error(
            "artifact-invalid-range",
            artifact_chunk_from_dict,
            reversed_segments,
            reference,
        )

    def test_lease_requires_capacity_freshness_and_replay_identity(self):
        values = {
            "leaseId": "lease-1",
            "operationId": "operation-1",
            "repoNode": "/repo/node-1",
            "artifact": reference_values(),
            "reservedBytes": 8192,
            "issuedAtMs": 1000,
            "expiresAtMs": 2000,
            "replayId": "nonce-1",
        }
        lease = artifact_upload_lease_from_dict(values, now_ms=1500)
        self.assertEqual(lease.replay_id, "nonce-1")

        self.assert_error(
            "artifact-invalid-lease",
            artifact_upload_lease_from_dict,
            values,
            2000,
        )
        undersized = dict(values, reservedBytes=8191)
        self.assert_error(
            "artifact-invalid-lease",
            artifact_upload_lease_from_dict,
            undersized,
            1500,
        )

    def test_receipt_proves_committed_bytes_under_exact_policy_epoch(self):
        values = {
            "receiptId": "receipt-1",
            "operationId": "operation-1",
            "repoNode": "/repo/node-1",
            "artifact": reference_values(),
            "committedAtMs": 2500,
            "storageGeneration": 3,
            "policyEpoch": "epoch-1",
            "state": "COMMITTED",
        }
        receipt = artifact_replica_receipt_from_dict(values)
        self.assertEqual(receipt.storage_generation, 3)

        self.assert_error(
            "artifact-invalid-receipt",
            artifact_replica_receipt_from_dict,
            dict(values, policyEpoch="epoch-2"),
        )
        self.assert_error(
            "artifact-invalid-receipt",
            artifact_replica_receipt_from_dict,
            dict(values, state="PENDING"),
        )


if __name__ == "__main__":
    unittest.main()
