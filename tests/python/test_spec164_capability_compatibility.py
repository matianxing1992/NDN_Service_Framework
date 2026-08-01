#!/usr/bin/env python3
"""Fail-closed v2 negotiation and explicit exact-packet-v1 boundary tests."""

from __future__ import annotations

from types import SimpleNamespace
import json
import unittest

from ndnsf import (
    AckCandidate,
    ProviderCapabilityHint,
    encode_provider_capability_ack,
)
from py_repoclient import (
    ArtifactApiError,
    ArtifactCapabilityRequirements,
    ArtifactErrorCode,
    RepoClient,
    artifact_capability_from_ack,
    artifact_capability_from_dict,
    artifact_reference_from_dict,
    negotiate_artifact_capabilities,
)
from py_repoclient.orchestration import (
    ExactPacketRepositoryApi,
    StorageCapability,
    _artifact_capability_payload,
)


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


def capability(repo_node, **overrides):
    values = {
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
    values.update(overrides)
    return artifact_capability_from_dict(values)


class CapabilityCompatibilityTest(unittest.TestCase):
    def test_unconfigured_repo_advertises_legacy_only(self):
        advertised = artifact_capability_from_dict(
            _artifact_capability_payload(
                StorageCapability("/repo/legacy", free_bytes=1 << 30)
            )
        )
        self.assertEqual(
            tuple(advertised.format_versions), ("exact-packet-v1",)
        )
        self.assertFalse(advertised.supports_resume)
        self.assertFalse(advertised.supports_replica_receipts)

    def test_ack_and_capability_operation_expose_validated_v2_fields(self):
        value = capability("/repo/a")
        fields = {
            "repoNode": value.repo_node,
            "formatVersions": list(value.format_versions),
            "digestAlgorithms": list(value.digest_algorithms),
            "signatureAlgorithms": list(value.signature_algorithms),
            "maxArtifactBytes": value.max_artifact_bytes,
            "maxChunkBytes": value.max_chunk_bytes,
            "maxRootEncodedBytes": value.max_root_encoded_bytes,
            "maxPageEncodedBytes": value.max_page_encoded_bytes,
            "maxPageEntries": value.max_page_entries,
            "maxManifestDepth": value.max_manifest_depth,
            "supportsResume": value.supports_resume,
            "supportsReplicaReceipts": (
                value.supports_replica_receipts
            ),
            "policyEpoch": value.policy_epoch,
        }
        payload = encode_provider_capability_ack(ProviderCapabilityHint(
            provider_name="/repo/a",
            service_name="/NDNSF/DistributedRepo/CAPABILITY",
            service_payload_schema="ndnsf-repo-capability-v2",
            service_payload={"artifactCapability": fields},
        ))
        decoded = artifact_capability_from_ack(AckCandidate(
            provider_name="/repo/a",
            service_name="/NDNSF/DistributedRepo/CAPABILITY",
            request_id="request-1",
            status=True,
            payload=payload,
        ))
        self.assertIsNotNone(decoded)
        self.assertEqual(decoded.repo_node, "/repo/a")

        class FakeUser:
            user = "/publisher"

            def request_service(self, *_args, **_kwargs):
                return SimpleNamespace(
                    status=True,
                    payload=json.dumps({
                        "repoNode": "/repo/a",
                        "artifactCapability": fields,
                    }).encode(),
                    error="",
                )

        client = RepoClient(FakeUser())
        fetched = client.artifact_capabilities()
        self.assertEqual(len(fetched), 1)
        self.assertEqual(fetched[0].repo_node, "/repo/a")

    def test_v2_filters_format_algorithm_limits_features_and_policy(self):
        artifact = reference()
        good_a = capability("/repo/a")
        good_b = capability("/repo/b")
        legacy = capability(
            "/repo/legacy", formatVersions=["exact-packet-v1"]
        )
        wrong_signature = capability(
            "/repo/signature", signatureAlgorithms=["rsa-sha256"]
        )
        undersized = capability(
            "/repo/small", maxArtifactBytes=4096, maxChunkBytes=4096
        )
        no_durability = capability(
            "/repo/no-receipt",
            supportsResume=False,
            supportsReplicaReceipts=False,
        )
        stale_policy = capability("/repo/stale", policyEpoch="epoch-0")

        result = negotiate_artifact_capabilities(
            [
                good_a, legacy, wrong_signature, undersized,
                no_durability, stale_policy, good_b,
            ],
            artifact,
            requested_replicas=2,
        )
        self.assertEqual(
            [item.repo_node for item in result.eligible],
            ["/repo/a", "/repo/b"],
        )
        rejected = {
            item.repo_node: set(item.reasons) for item in result.rejected
        }
        self.assertIn("format-version", rejected["/repo/legacy"])
        self.assertIn(
            "root-signature-algorithm", rejected["/repo/signature"]
        )
        self.assertIn("artifact-size-limit", rejected["/repo/small"])
        self.assertIn("chunk-size-limit", rejected["/repo/small"])
        self.assertEqual(
            rejected["/repo/no-receipt"],
            {"resume", "replica-receipts"},
        )
        self.assertIn("policy-epoch", rejected["/repo/stale"])

    def test_no_compatible_peer_is_explicit_unsupported_capability(self):
        with self.assertRaises(ArtifactApiError) as captured:
            negotiate_artifact_capabilities(
                [capability("/repo/legacy",
                            formatVersions=["exact-packet-v1"])],
                reference(),
                requested_replicas=1,
            )
        self.assertEqual(
            captured.exception.code,
            ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
        )

    def test_insufficient_distinct_peers_is_explicit_durability_failure(self):
        one = capability("/repo/a")
        with self.assertRaises(ArtifactApiError) as captured:
            negotiate_artifact_capabilities(
                [one, one],
                reference(),
                requested_replicas=2,
            )
        self.assertEqual(
            captured.exception.code,
            ArtifactErrorCode.DURABILITY_NOT_ACHIEVED,
        )
        self.assertEqual(captured.exception.achieved_replicas, 1)

    def test_v2_api_never_silently_accepts_exact_packet_reference(self):
        with self.assertRaises(ArtifactApiError) as captured:
            negotiate_artifact_capabilities(
                [capability("/repo/a")],
                reference(format_version="exact-packet-v1"),
                requested_replicas=1,
                requirements=ArtifactCapabilityRequirements(),
            )
        self.assertEqual(
            captured.exception.code,
            ArtifactErrorCode.UNSUPPORTED_CAPABILITY,
        )

    def test_exact_packet_api_is_explicit_and_delegates_without_rewrite(self):
        packet = object()
        manifest = object()
        calls = []

        def put(name, packets, **kwargs):
            calls.append(("put", name, packets, kwargs))
            return manifest

        def get(name, supplied_manifest, *, repo_node):
            calls.append(("get", name, supplied_manifest, repo_node))
            return [packet]

        api = ExactPacketRepositoryApi(SimpleNamespace(
            put_signed_packets=put,
            get_signed_packets=get,
        ))
        self.assertEqual(api.format_version, "exact-packet-v1")
        self.assertIs(
            api.put_signed_packets(
                "/object", [packet], object_type="signed-data",
                object_size=1, object_sha256=DIGEST,
            ),
            manifest,
        )
        self.assertEqual(
            api.get_signed_packets(
                "/object", manifest, repo_node="/repo/a"
            ),
            [packet],
        )
        self.assertIs(calls[0][2][0], packet)
        self.assertIs(calls[1][2], manifest)


if __name__ == "__main__":
    unittest.main()
