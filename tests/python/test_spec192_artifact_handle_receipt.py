from __future__ import annotations

import gc
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from ndnsf import ServiceUser
from py_repoclient import (
    ArtifactApiError,
    ArtifactHandle,
    ArtifactReference,
    ArtifactRepositoryApi,
    LocalArtifactCommit,
    PersistentRepoClientConfig,
    RepoClient,
)
import py_repoclient


FIXTURE = b"spec192 native Python binding fixture"


def fixture_reference() -> ArtifactReference:
    return ArtifactReference(
        "/app/model/v1",
        hashlib.sha256(FIXTURE).hexdigest(),
        len(FIXTURE),
        "/app/manifest/v1",
        "/app/provider",
        "epoch-1",
    )


class Spec192ArtifactBindingTest(unittest.TestCase):
    def test_native_reference_receipt_handle_lookup_and_fetch(self):
        with tempfile.TemporaryDirectory(prefix="spec192-inapp-binding-") as tmp:
            root = Path(tmp)
            source = root / "source.bin"
            destination = root / "result.bin"
            source.write_bytes(FIXTURE)
            reference = fixture_reference()
            api = ArtifactRepositoryApi.make_in_app(
                durable_root=root / "repo",
                owner_id="/app/test-owner",
            )

            published = api.publish_file(source, reference)
            self.assertEqual(published.reference.to_json(), reference.to_json())
            self.assertIsNotNone(published.handle)
            self.assertIsNotNone(published.receipt)
            self.assertEqual(published.receipt.reference.to_json(), reference.to_json())

            reopened_receipt = api.get_receipt(published.receipt.receipt_id)
            local_commit = api.lookup_local_commit(reference)
            self.assertEqual(reopened_receipt.to_json(), published.receipt.to_json())
            self.assertIsNotNone(local_commit)
            self.assertEqual(local_commit.reference.to_json(), reference.to_json())

            restored_handle = type(published.handle).from_json(
                published.handle.to_json()
            )
            self.assertEqual(restored_handle.to_json(), published.handle.to_json())
            api.fetch_file(reference, destination)
            self.assertEqual(destination.read_bytes(), FIXTURE)
            self.assertEqual(
                hashlib.sha256(destination.read_bytes()).hexdigest(),
                reference.content_digest,
            )

    def test_persistent_api_retains_native_service_user_owner(self):
        trust_schema = Path("examples/trust-schema.conf").resolve()
        service_user = ServiceUser(
            group="/spec192/binding/group",
            controller="/spec192/binding/controller",
            user="/spec192/binding/user",
            trust_schema=str(trust_schema),
            permission_wait_ms=0,
            serve_certificates=False,
        )
        owner = service_user._native
        initial_references = sys.getrefcount(owner)
        config = PersistentRepoClientConfig()
        config.ack_collection_time_ms = 11
        config.request_timeout_ms = 13
        config.operation_wait_timeout_ms = 17

        api = ArtifactRepositoryApi.make_persistent(
            service_user,
            config,
        )
        self.assertEqual(config.ack_collection_time_ms, 11)
        self.assertEqual(config.request_timeout_ms, 13)
        self.assertEqual(config.operation_wait_timeout_ms, 17)
        self.assertIn("LocalArtifactCommit", py_repoclient.__all__)
        self.assertIs(LocalArtifactCommit, py_repoclient.ArtifactLocalCommitRecord)
        del service_user
        gc.collect()
        # The native API retains one owner reference as the high-level
        # ServiceUser wrapper releases its reference.
        self.assertEqual(sys.getrefcount(owner), initial_references)

        del api
        gc.collect()
        self.assertEqual(sys.getrefcount(owner), initial_references - 1)

    def test_repo_client_is_a_thin_native_artifact_api_facade(self):
        service_user = ServiceUser(
            group="/spec192/repo-client/group",
            controller="/spec192/repo-client/controller",
            user="/spec192/repo-client/user",
            trust_schema=str(Path("examples/trust-schema.conf").resolve()),
            permission_wait_ms=0,
            serve_certificates=False,
        )
        native_factory = ArtifactRepositoryApi.make_persistent
        captured_configs = []

        def capture_config(user, config):
            captured_configs.append(config)
            return native_factory(user, config)

        with patch.object(
            ArtifactRepositoryApi,
            "make_persistent",
            side_effect=capture_config,
        ):
            client = RepoClient(service_user)
            custom_client = RepoClient(
                service_user,
                repo_service_name="/spec192/custom-repo",
                ack_timeout_ms=11,
                timeout_ms=13,
            )

        self.assertEqual(client.ack_timeout_ms, 1000)
        self.assertEqual(client.timeout_ms, 30000)
        self.assertEqual(custom_client.ack_timeout_ms, 11)
        self.assertEqual(custom_client.timeout_ms, 13)
        self.assertEqual(len(captured_configs), 2)
        default_config = captured_configs[0]
        self.assertEqual(
            default_config.service_prefix, "/NDNSF/DistributedRepo/Object/v1"
        )
        self.assertEqual(default_config.ack_collection_time_ms, 1000)
        self.assertEqual(default_config.request_timeout_ms, 30000)
        self.assertEqual(default_config.operation_wait_timeout_ms, 30000)
        custom_config = captured_configs[1]
        self.assertEqual(
            custom_config.service_prefix, "/spec192/custom-repo/Object/v1"
        )
        self.assertEqual(custom_config.ack_collection_time_ms, 11)
        self.assertEqual(custom_config.request_timeout_ms, 13)
        self.assertEqual(custom_config.operation_wait_timeout_ms, 13)
        self.assertEqual(
            client.artifact_api._native.__class__.__name__,
            "NativeArtifactRepositoryApi",
        )
        native_facade = Mock()
        custom_client._artifact_api = native_facade
        reference = fixture_reference()
        selected = object()
        custom_client.begin_publish_file(
            "source.bin", reference, selected, "operation-key"
        )
        native_facade.begin_publish_file.assert_called_once_with(
            "source.bin", reference, selected, "operation-key", 13
        )
        with self.assertRaisesRegex(RuntimeError, "SPEC192_COMPATIBILITY_V1"):
            client.insert("/legacy/object", b"legacy")
        with self.assertRaisesRegex(RuntimeError, "SPEC192_COMPATIBILITY_V1"):
            client.configure_artifact_backend(object())
        with self.assertRaisesRegex(RuntimeError, "SPEC192_COMPATIBILITY_V1"):
            client.publish_file_async
        with self.assertRaisesRegex(RuntimeError, "SPEC192_COMPATIBILITY_V1"):
            client.fetch_file(fixture_reference(), "destination.bin")
        with self.assertRaisesRegex(RuntimeError, "SPEC192_COMPATIBILITY_V1"):
            client.lookup_local_commit
        with self.assertRaises(TypeError):
            client.publish_file("source.bin", fixture_reference())

    def test_native_fetch_error_keeps_structured_result(self):
        with tempfile.TemporaryDirectory(prefix="spec192-fetch-error-") as tmp:
            root = Path(tmp)
            service_user = ServiceUser(
                group="/spec192/fetch-error/group",
                controller="/spec192/fetch-error/controller",
                user="/spec192/fetch-error/user",
                trust_schema=str(Path("examples/trust-schema.conf").resolve()),
                permission_wait_ms=0,
                serve_certificates=False,
            )
            api = ArtifactRepositoryApi.make_persistent(service_user)
            reference = fixture_reference()
            handle = ArtifactHandle.from_json(json.dumps({
                "schemaVersion": 1,
                "reference": json.loads(reference.to_json()),
                "commitReceiptIds": ["receipt-1"],
                "selectedTargets": [{
                    "repoNode": "/repo/a",
                    "providerIdentity": "/repo/provider/a",
                    "servicePrefix": "/NDNSF/DistributedRepo/Object/v1",
                }],
                "replicaCommits": [{
                    "repoNode": "/repo/a",
                    "providerIdentity": "/repo/provider/a",
                    "receiptId": "receipt-1",
                    "operationId": "operation-1",
                    "reference": json.loads(reference.to_json()),
                    "generation": 1,
                    "contentDigest": hashlib.sha256(FIXTURE).hexdigest(),
                    "persistedBytes": len(FIXTURE),
                    "completedAtMs": 1,
                    "locationVersion": 1,
                    "routeState": "ROUTABLE",
                    "state": "COMMITTED",
                    "failureReason": "",
                    "forwardingHint": "/NDNSF/DistributedRepo/Object/v1",
                }],
                "seedRepoId": "/repo/a",
                "writeConsistency": "ONE",
                "lifecycleMode": "PERSISTENT",
            }))

            with self.assertRaises(ArtifactApiError) as captured:
                api.fetch_file(handle, root / "destination.bin", timeout_ms=100)
            self.assertEqual(
                captured.exception.result.reference.to_json(), reference.to_json()
            )
            self.assertEqual(len(captured.exception.result.attempts), 1)
            attempt = captured.exception.result.attempts[0]
            self.assertEqual(attempt.repo_node, "/repo/a")
            self.assertEqual(attempt.provider_identity, "/repo/provider/a")
            self.assertEqual(attempt.outcome, "FAILED")
            self.assertTrue(attempt.failure_class)


if __name__ == "__main__":
    unittest.main()
