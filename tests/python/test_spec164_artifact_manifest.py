#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path
import unittest

from py_repoclient import (
    ArtifactLimits,
    ArtifactManifestTrustPolicy,
    ArtifactValidationError,
    SignedArtifactRoot,
    artifact_capability_from_dict,
    artifact_chunk_from_dict,
    artifact_manifest_page_from_dict,
    artifact_reference_from_dict,
    artifact_root_manifest_from_dict,
    artifact_sha256_hex,
    canonical_manifest_page_bytes,
    canonical_root_manifest_bytes,
    decode_signed_artifact_root,
    encode_signed_artifact_root,
    validate_artifact_resume_identity,
    verify_artifact_chunk_payload,
    verify_artifact_manifest_graph,
    verify_artifact_payload,
)


class ManifestFixture:
    def __init__(self) -> None:
        self.limits = ArtifactLimits()
        self.limits.max_manifest_pages = 8
        self.limits.max_manifest_chunks = 8
        self.limits.max_cryptographic_operations = 32
        self.payload = b"abcdefgh"
        self.artifact_dict = {
            "logicalName": "/publisher/artifact",
            "digestAlgorithm": "sha256",
            "contentDigest": artifact_sha256_hex(self.payload),
            "sizeBytes": len(self.payload),
            "formatVersion": "artifact-manifest-v2",
            "rootManifestName": "/publisher/artifact/root/v=2",
            "publisherIdentity": "/publisher",
            "policyEpoch": "epoch-1",
        }
        self.artifact = artifact_reference_from_dict(
            self.artifact_dict, self.limits)
        self.chunk_dict = {
            "index": 0,
            "offsetBytes": 0,
            "lengthBytes": len(self.payload),
            "digestAlgorithm": "sha256",
            "digest": artifact_sha256_hex(self.payload),
            "firstSegment": 0,
            "finalSegment": 1,
        }
        self.chunk = artifact_chunk_from_dict(
            self.chunk_dict, self.artifact, self.limits)
        page_dict = {
            "pageVersion": "artifact-manifest-page-v2",
            "depth": 0,
            "offsetBytes": 0,
            "lengthBytes": len(self.payload),
            "pageDigestAlgorithm": "sha256",
            "pageDigest": "0" * 64,
            "children": [{
                "kind": "chunk",
                "index": 0,
                "offsetBytes": 0,
                "lengthBytes": len(self.payload),
                "digestAlgorithm": "sha256",
                "digest": artifact_sha256_hex(self.payload),
            }],
        }
        placeholder = artifact_manifest_page_from_dict(
            page_dict, 256, self.limits)
        page_dict["pageDigest"] = artifact_sha256_hex(
            canonical_manifest_page_bytes(placeholder, self.limits))
        self.page_dict = page_dict
        self.page = artifact_manifest_page_from_dict(
            page_dict, 256, self.limits)

        self.root_dict = {
            "artifact": self.artifact_dict,
            "packetPayloadBytes": 4,
            "chunkBytes": 8,
            "namingTemplate":
                "/publisher/artifact/chunk={chunk}/seg={segment}",
            "manifestRootDigestAlgorithm": "sha256",
            "manifestRootDigest": page_dict["pageDigest"],
            "signatureAlgorithm": "rsa-sha256",
            "publisherKeyLocator": "/publisher/KEY/1",
            "createdAtMs": 1000,
            "expiresAtMs": 3000,
            "criticalExtensions": [],
        }
        self.root = artifact_root_manifest_from_dict(
            self.root_dict, 512, self.limits)
        self.capability = artifact_capability_from_dict({
            "repoNode": "/repo/1",
            "formatVersions": ["artifact-manifest-v2"],
            "digestAlgorithms": ["sha256"],
            "signatureAlgorithms": ["rsa-sha256"],
            "maxArtifactBytes": 1 << 50,
            "maxChunkBytes": 64 * 1024 * 1024,
            "maxRootEncodedBytes": 64 * 1024,
            "maxPageEncodedBytes": 4 * 1024 * 1024,
            "maxPageEntries": 65536,
            "maxManifestDepth": 16,
            "policyEpoch": "epoch-1",
        }, self.limits)

        self.temp = tempfile.TemporaryDirectory()
        directory = Path(self.temp.name)
        self.private_key = directory / "private.pem"
        self.public_key = directory / "public.pem"
        subprocess.run([
            "openssl", "genpkey", "-algorithm", "RSA",
            "-pkeyopt", "rsa_keygen_bits:2048",
            "-out", str(self.private_key),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([
            "openssl", "pkey", "-in", str(self.private_key),
            "-pubout", "-out", str(self.public_key),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        self.signed_root = self.sign(self.root)
        self.policy = ArtifactManifestTrustPolicy()
        self.policy.trusted_publisher_identity = "/publisher"
        self.policy.trusted_key_locator = "/publisher/KEY/1"
        self.policy.public_key_pem = self.public_key.read_text()
        self.policy.policy_epoch = "epoch-1"
        self.policy.evaluation_time_ms = 2000
        self.policy.allowed_digest_algorithms = ["sha256"]
        self.policy.allowed_signature_algorithms = ["rsa-sha256"]

    def sign(self, root) -> SignedArtifactRoot:
        directory = Path(self.temp.name)
        canonical = directory / "root.bin"
        signature = directory / "root.sig"
        canonical.write_bytes(canonical_root_manifest_bytes(root, self.limits))
        subprocess.run([
            "openssl", "dgst", "-sha256", "-sign", str(self.private_key),
            "-out", str(signature), str(canonical),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        signed = SignedArtifactRoot()
        signed.root = root
        signed.signature_value = signature.read_bytes()
        return signed

    def close(self) -> None:
        self.temp.cleanup()


class ArtifactManifestV2Test(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = ManifestFixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def test_signed_root_page_chunk_and_full_digest_compose(self) -> None:
        f = self.fixture
        result = verify_artifact_manifest_graph(
            f.signed_root, f.artifact, [f.page], [f.chunk],
            f.capability, f.policy, f.limits)
        self.assertEqual(result.asymmetric_verification_count, 1)
        self.assertEqual(result.digest_verification_count, 1)
        self.assertEqual(result.verified_page_count, 1)
        self.assertEqual(result.verified_chunk_count, 1)
        verify_artifact_chunk_payload(f.chunk, f.payload)
        verify_artifact_payload(f.artifact, f.payload)

        wire = encode_signed_artifact_root(f.signed_root, f.limits)
        decoded = decode_signed_artifact_root(wire, f.limits)
        self.assertEqual(decoded.signature_value, f.signed_root.signature_value)
        with self.assertRaisesRegex(
                ArtifactValidationError, "malformed-encoding"):
            decode_signed_artifact_root(wire + b"\x00", f.limits)

    def test_substitution_downgrade_revocation_and_critical_field_reject(self) -> None:
        f = self.fixture
        substituted_dict = dict(f.artifact_dict)
        substituted_dict["logicalName"] = "/publisher/other"
        substituted = artifact_reference_from_dict(
            substituted_dict, f.limits)
        with self.assertRaisesRegex(ArtifactValidationError, "substitution"):
            verify_artifact_manifest_graph(
                f.signed_root, substituted, [f.page], [f.chunk],
                f.capability, f.policy, f.limits)

        downgraded = artifact_capability_from_dict({
            "repoNode": "/repo/1",
            "formatVersions": ["artifact-manifest-v2"],
            "digestAlgorithms": ["sha256"],
            "signatureAlgorithms": ["ecdsa-sha256"],
            "maxArtifactBytes": 1 << 50,
            "maxChunkBytes": 64 * 1024 * 1024,
            "maxRootEncodedBytes": 64 * 1024,
            "maxPageEncodedBytes": 4 * 1024 * 1024,
            "maxPageEntries": 65536,
            "maxManifestDepth": 16,
            "policyEpoch": "epoch-1",
        }, f.limits)
        with self.assertRaisesRegex(ArtifactValidationError, "downgrade"):
            verify_artifact_manifest_graph(
                f.signed_root, f.artifact, [f.page], [f.chunk],
                downgraded, f.policy, f.limits)

        f.policy.revoked_key_locators = ["/publisher/KEY/1"]
        with self.assertRaisesRegex(ArtifactValidationError, "revoked-publisher"):
            verify_artifact_manifest_graph(
                f.signed_root, f.artifact, [f.page], [f.chunk],
                f.capability, f.policy, f.limits)

        f.policy.revoked_key_locators = []
        critical_dict = dict(f.root_dict)
        critical_dict["criticalExtensions"] = ["future-required"]
        critical_root = artifact_root_manifest_from_dict(
            critical_dict, 512, f.limits)
        critical_signed = f.sign(critical_root)
        with self.assertRaisesRegex(
                ArtifactValidationError, "unsupported-critical-field"):
            verify_artifact_manifest_graph(
                critical_signed, f.artifact, [f.page], [f.chunk],
                f.capability, f.policy, f.limits)

    def test_corruption_truncation_and_mixed_resume_reject(self) -> None:
        f = self.fixture
        corrupt_page_dict = dict(f.page_dict)
        corrupt_page_dict["children"] = [dict(f.page_dict["children"][0])]
        corrupt_page_dict["children"][0]["digest"] = "f" * 64
        corrupt_page = artifact_manifest_page_from_dict(
            corrupt_page_dict, 256, f.limits)
        with self.assertRaisesRegex(
                ArtifactValidationError, "digest-mismatch"):
            verify_artifact_manifest_graph(
                f.signed_root, f.artifact, [corrupt_page], [f.chunk],
                f.capability, f.policy, f.limits)
        with self.assertRaisesRegex(
                ArtifactValidationError, "digest-mismatch"):
            verify_artifact_payload(f.artifact, f.payload[:-1])

        resumed_dict = dict(f.artifact_dict)
        resumed_dict["policyEpoch"] = "epoch-2"
        resumed = artifact_reference_from_dict(resumed_dict, f.limits)
        with self.assertRaisesRegex(ArtifactValidationError, "mixed-resume"):
            validate_artifact_resume_identity(
                f.artifact, f.root, resumed, f.root)


if __name__ == "__main__":
    unittest.main()
