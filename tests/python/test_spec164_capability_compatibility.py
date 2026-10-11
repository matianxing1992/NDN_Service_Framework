#!/usr/bin/env python3
"""Native capability decoding and retired Python negotiation compatibility."""

from __future__ import annotations

import unittest
from py_repoclient import (
    ArtifactCapabilityRequirements,
    ArtifactErrorCode,
    RepoClient,
    RepoCompatibilityError,
    artifact_capability_from_ack,
    artifact_capability_from_dict,
    artifact_reference_from_dict,
    negotiate_artifact_capabilities,
)
import py_repoclient.orchestration as legacy_repo


DIGEST = "ab" * 32


def reference(*, format_version="artifact-manifest-v2"):
    return artifact_reference_from_dict({
        "logicalName": "/publisher/models/qwen",
        "digestAlgorithm": "sha256",
        "contentDigest": DIGEST,
        "sizeBytes": 8192,
        "formatVersion": format_version,
        "rootManifestName": "/publisher/models/qwen/root",
        "publisherIdentity": "/publisher",
        "policyEpoch": "epoch-1",
    })


def capability_values(repo_node="/repo/a"):
    return {
        "repoNode": repo_node,
        "formatVersions": ["artifact-manifest-v2", "exact-packet-v1"],
        "digestAlgorithms": ["sha256"],
        "signatureAlgorithms": ["ed25519"],
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


class CapabilityCompatibilityTest(unittest.TestCase):
    def test_native_capability_decoder_preserves_supported_fields(self):
        value = artifact_capability_from_dict(capability_values())
        self.assertEqual(value.repo_node, "/repo/a")
        self.assertEqual(tuple(value.format_versions), (
            "artifact-manifest-v2",
            "exact-packet-v1",
        ))
        self.assertEqual(value.max_artifact_bytes, 1 << 30)
        self.assertTrue(value.supports_resume)
        self.assertTrue(value.supports_replica_receipts)
        self.assertEqual(value.policy_epoch, "epoch-1")

    def test_python_ack_capability_decoder_has_versioned_retirement(self):
        with self.assertRaises(RepoCompatibilityError) as caught:
            artifact_capability_from_ack(object())
        self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(caught.exception.operation, "artifact_capability_from_ack")

    def test_python_owned_negotiation_and_facades_fail_with_versioned_error(self):
        requirements = ArtifactCapabilityRequirements()
        with self.assertRaises(RepoCompatibilityError) as code_error:
            ArtifactErrorCode()
        self.assertEqual(
            code_error.exception.code, "SPEC192_COMPATIBILITY_V1"
        )
        with self.assertRaises(RepoCompatibilityError) as backend_error:
            RepoClient(None, artifact_backend=object())
        self.assertEqual(
            backend_error.exception.code, "SPEC192_COMPATIBILITY_V1"
        )
        with self.assertRaises(RepoCompatibilityError):
            negotiate_artifact_capabilities([], reference(), requested_replicas=1,
                                            requirements=requirements)
        with self.assertRaises(RepoCompatibilityError):
            legacy_repo.ExactPacketRepositoryApi()


if __name__ == "__main__":
    unittest.main()
