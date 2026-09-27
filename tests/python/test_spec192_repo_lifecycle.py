#!/usr/bin/env python3
"""Spec192 lifecycle boundary and backend ownership regressions."""

from __future__ import annotations

from contextlib import ExitStack
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import py_repoclient.orchestration as orchestration
from py_repoclient.artifact_api import (
    ArtifactDescriptor,
    ArtifactRepositoryApi,
    ArtifactSessionStatus,
)
from py_repoclient.local_artifact_backend import FilesystemArtifactApiBackend


class _FakePersistence:
    def __init__(self, *args, **kwargs):
        self.artifact_writes_enabled = True
        self.lock = object()
        self.connection = object()
        self.artifact_payload_store = object()


class _PendingPublishDriver:
    def __init__(self, operation_id, reference):
        self.operation_id = operation_id
        self.reference = reference
        self.state = "OPEN"

    def status(self):
        return ArtifactSessionStatus(
            self.operation_id, "PUBLISH", self.state, self.reference
        )

    def abort(self, preserve_progress):
        del preserve_progress
        self.state = "CANCELLED"
        return self.status()


class _RecordingNetworkBackend:
    def __init__(self):
        self.publish_calls = []

    def begin_publish(self, descriptor, operation_id, emit_progress):
        del emit_progress
        self.publish_calls.append((descriptor, operation_id))
        return _PendingPublishDriver(operation_id, descriptor.reference)


class Spec192RepoLifecycleTest(unittest.TestCase):
    def test_repo_node_normalizes_persistent_legacy_values_at_constructor(self):
        values = (
            ("omitted", {}),
            ("empty", {"repo_mode": ""}),
            ("Remote", {"repo_mode": "Remote"}),
            ("case-insensitive remote", {"repo_mode": "REMOTE"}),
            ("canonical", {"repo_mode": "persistent"}),
            ("case-insensitive persistent", {"repo_mode": "PERSISTENT"}),
        )
        for index, (label, options) in enumerate(values):
            with self.subTest(value=label), tempfile.TemporaryDirectory() as raw:
                storage_dir = Path(raw) / "repo"
                with ExitStack() as stack:
                    provider = stack.enter_context(
                        patch.object(orchestration, "ServiceProvider")
                    )
                    stack.enter_context(
                        patch.object(
                            orchestration,
                            "SqliteRepositoryPersistence",
                            _FakePersistence,
                        )
                    )
                    stack.enter_context(
                        patch.object(orchestration, "RepoDataPlaneProducer")
                    )
                    stack.enter_context(
                        patch.object(orchestration.RepoNodeApp, "_init_sqlite")
                    )
                    stack.enter_context(
                        patch.object(
                            orchestration.RepoNodeApp,
                            "_restore_serving_prefixes",
                        )
                    )
                    stack.enter_context(
                        patch.object(
                            orchestration.RepoNodeApp, "_restore_catalog_state"
                        )
                    )
                    app = orchestration.RepoNodeApp(
                        repo_node=f"/repo/spec192/{index}",
                        storage_dir=storage_dir,
                        **options,
                    )
                self.assertEqual(app.capability.repo_mode, "persistent")
                self.assertTrue(storage_dir.is_dir())
                provider.assert_called_once()

    def test_repo_node_rejects_nonpersistent_modes_before_side_effects(self):
        for value in ("embedded", "in-app", "local", "both", "unknown"):
            with self.subTest(repo_mode=value), tempfile.TemporaryDirectory() as raw:
                storage_dir = Path(raw) / "must-not-exist"
                with ExitStack() as stack:
                    provider = stack.enter_context(
                        patch.object(orchestration, "ServiceProvider")
                    )
                    persistence = stack.enter_context(
                        patch.object(
                            orchestration, "SqliteRepositoryPersistence"
                        )
                    )
                    data_plane = stack.enter_context(
                        patch.object(orchestration, "RepoDataPlaneProducer")
                    )
                    with self.assertRaisesRegex(
                        ValueError, "RepoNodeApp supports only Persistent mode"
                    ):
                        orchestration.RepoNodeApp(
                            repo_node="/repo/spec192",
                            storage_dir=storage_dir,
                            repo_mode=value,
                        )
                provider.assert_not_called()
                persistence.assert_not_called()
                data_plane.assert_not_called()
                self.assertFalse(storage_dir.exists())

    def test_repo_node_cli_rejects_mode_before_policy_generation(self):
        repo_root = Path(__file__).resolve().parents[2]
        entrypoint = (
            repo_root
            / "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_node.py"
        )
        config = (
            repo_root
            / "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
        )
        with tempfile.TemporaryDirectory() as raw:
            generated_policy_dir = Path(raw) / "generated-policy"
            env = os.environ.copy()
            source_paths = (
                str(repo_root / "NDNSF-DistributedInference"),
                str(repo_root / "NDNSF-DistributedRepo/pythonWrapper"),
            )
            env["PYTHONPATH"] = os.pathsep.join(
                (*source_paths, env.get("PYTHONPATH", ""))
            ).rstrip(os.pathsep)
            result = subprocess.run(
                [
                    sys.executable,
                    str(entrypoint),
                    "--config", str(config),
                    "--generated-policy-dir", str(generated_policy_dir),
                    "--provider-id", "A",
                    "--repo-node", "/repo/spec192/A",
                    "--repo-mode", "embedded",
                ],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("RepoNodeApp supports only Persistent mode", result.stderr)
            self.assertFalse(generated_policy_dir.exists())

    def test_local_and_network_api_instances_do_not_cross_route(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "model.bin"
            payload = b"spec192-isolated-backends"
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()

            local_backend = FilesystemArtifactApiBackend(root / "local-store")
            network_backend = _RecordingNetworkBackend()
            local_api = ArtifactRepositoryApi(
                local_backend, publisher_identity="/publisher"
            )
            network_api = ArtifactRepositoryApi(
                network_backend, publisher_identity="/publisher"
            )

            local_result = local_api.publish_file(
                source,
                name="/artifacts/local",
                expected_sha256=digest,
            )
            self.assertEqual(network_backend.publish_calls, [])

            descriptor = ArtifactDescriptor(
                reference=local_result.reference,
                requested_replicas=1,
                idempotency_key="spec192-network-route",
            )
            session = network_api.begin_upload(descriptor)
            try:
                self.assertEqual(len(network_backend.publish_calls), 1)
                self.assertIs(local_api.backend, local_backend)
                self.assertIs(network_api.backend, network_backend)
            finally:
                session.abort(preserve_progress=False)


if __name__ == "__main__":
    unittest.main()
