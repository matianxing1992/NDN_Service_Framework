#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
BUILDER = (
    ROOT
    / "specs/162-itiger-qwen36-generation/jobs/"
    "build-automatic-planning-manifest.py"
)
PREPARE = (
    ROOT
    / "specs/162-itiger-qwen36-generation/jobs/prepare-qwen36.py"
)
REPO_REGISTER = (
    ROOT
    / "specs/162-itiger-qwen36-generation/jobs/register-qwen36-repo.py"
)
PROVIDER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py"
)


def load_builder():
    spec = importlib.util.spec_from_file_location(
        "spec162_automatic_manifest", BUILDER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_prepare():
    spec = importlib.util.spec_from_file_location(
        "spec162_prepare_for_repo", PREPARE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Qwen36RepoRegistrationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_builder()
        self.roles = [
            "/LLM/Pipeline/Stage/0",
            "/LLM/Pipeline/Stage/1",
            "/LLM/Pipeline/Stage/2",
        ]

    def test_only_exact_repo_registration_activates_catalog(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stage_manifest = {
                "repository": "Qwen/Qwen3.6-27B",
                "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
                "modelDigest": "sha256:" + "f" * 64,
                "dtype": "bf16",
                "quantization": "none",
                "layerRanges": [[0, 21], [21, 42], [42, 64]],
                "tokenizer": {"digest": "sha256:" + "e" * 64},
                "stages": [
                    {
                        "role": role,
                        "stageIndex": index,
                        "bytes": 10_000 + index,
                        "sha256": f"{index + 1:064x}",
                    }
                    for index, role in enumerate(self.roles)
                ],
            }
            stage_path = root / "stage-manifest.json"
            stage_path.write_text(
                json.dumps(stage_manifest, sort_keys=True), encoding="utf-8")
            registration = {
                "schemaVersion": "ndnsf-di-qwen36-repo-registration-v1",
                "stageManifestSha256": "sha256:" + hashlib.sha256(
                    stage_path.read_bytes()).hexdigest(),
                "artifacts": [
                    {
                        "role": item["role"],
                        "fileSha256": "sha256:" + item["sha256"],
                        "fileBytes": item["bytes"],
                        "objectName": f"/repo/qwen/stage-{item['stageIndex']}",
                    }
                    for item in stage_manifest["stages"]
                ],
            }
            registration_path = root / "registration.json"
            registration_path.write_text(
                json.dumps(registration), encoding="utf-8")
            output = root / "automatic.json"
            argv = [
                str(BUILDER),
                "--stage-manifest", str(stage_path),
                "--output", str(output),
                "--repository-prefix", "/unused",
                "--repo-registration", str(registration_path),
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(self.module.main(), 0)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                result["preSplitCatalog"]["publicationState"], "ACTIVE")
            self.assertTrue(
                result["preSplitCatalog"]["registrationDigest"].startswith(
                    "sha256:"))
            self.assertEqual(
                [item["dataName"] for item in result["stages"]],
                [f"/repo/qwen/stage-{index}" for index in range(3)],
            )

    def test_registration_digest_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stage_path = root / "stage-manifest.json"
            stage_path.write_text("{}", encoding="utf-8")
            registration_path = root / "registration.json"
            registration_path.write_text(json.dumps({
                "schemaVersion": "ndnsf-di-qwen36-repo-registration-v1",
                "stageManifestSha256": "sha256:" + "0" * 64,
                "artifacts": [],
            }), encoding="utf-8")
            argv = [
                str(BUILDER),
                "--stage-manifest", str(stage_path),
                "--output", str(root / "automatic.json"),
                "--repository-prefix", "/unused",
                "--repo-registration", str(registration_path),
            ]
            with mock.patch.object(sys, "argv", argv):
                with self.assertRaises((KeyError, RuntimeError)):
                    self.module.main()

    def test_prepared_policy_authorizes_repo_publish_and_provider_fetch(self) -> None:
        import yaml
        prepare = load_prepare()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy.yaml"
            path.write_text("services: []\n", encoding="utf-8")
            prepare._add_distributed_repo_services(
                path,
                user="/example/user",
                provider_prefix="/example/provider",
            )
            services = yaml.safe_load(
                path.read_text(encoding="utf-8"))["services"]
            self.assertGreater(len(services), 1)
            for service in services:
                self.assertEqual(
                    service["users"],
                    [
                        "/example/user",
                        "/example/provider",
                        "/example/provider/1",
                        "/example/provider/2",
                    ],
                )
                self.assertEqual(
                    [item["identity"] for item in service["providers"]],
                    [
                        "/example/provider",
                        "/example/provider/1",
                        "/example/provider/2",
                    ],
                )
            artifact = next(
                item for item in services
                if item["name"]
                == "/NDNSF/DistributedRepo/Artifact/v2/STORE"
            )
            self.assertEqual(artifact["roles"], ["artifact-replica-0"])
            self.assertTrue(all(
                provider["roles"] == ["artifact-replica-0"]
                for provider in artifact["providers"]
            ))

    def test_qwen_uses_public_whole_artifact_transport(self) -> None:
        registration = REPO_REGISTER.read_text(encoding="utf-8")
        provider = PROVIDER.read_text(encoding="utf-8")
        self.assertIn("ArtifactRepositoryApi", registration)
        self.assertIn(
            "CollaborationArtifactApiBackend.from_config", registration)
        self.assertIn("repo.publish_file(", registration)
        self.assertNotIn(".put_file(", registration)
        self.assertIn('"artifactReference": reference', registration)
        self.assertIn("committed_receipts=repo_receipts", provider)
        self.assertIn('repo_holder["artifact_api"].fetch_file(', provider)
        self.assertNotIn('repo_holder["repo"].get_file(', provider)


if __name__ == "__main__":
    unittest.main()
