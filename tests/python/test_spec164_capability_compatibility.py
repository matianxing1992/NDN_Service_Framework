#!/usr/bin/env python3
"""Native capability decoding and retired Python negotiation compatibility."""

from __future__ import annotations

import unittest
from py_repoclient import artifact_capability_from_dict


DIGEST = "ab" * 32




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


if __name__ == "__main__":
    unittest.main()
