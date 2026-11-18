#!/usr/bin/env python3

import hashlib
import importlib.util
from importlib.machinery import SourceFileLoader
import json
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
# Tracked copies of the historical job scripts. These were read from
# `specs/162-itiger-qwen36-generation/jobs/`, an ignored Spec directory that does not exist in this
# checkout, so every case below failed on a missing file rather than on the behaviour it guards.
BUILDER = (
    ROOT
    / "tests/fixtures/legacy-qwen-source-bundle/jobs/"
    "build-automatic-planning-manifest.py.txt"
)
PREPARE = ROOT / "tests/fixtures/legacy-exact-sif-gate/prepare-qwen36.py.txt"
REPO_REGISTER = (
    ROOT / "tests/fixtures/legacy-exact-sif-gate/register-qwen36-repo.py.txt"
)
PROVIDER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py"
)


def load_builder():
    spec = importlib.util.spec_from_loader(
        "spec162_automatic_manifest", SourceFileLoader("spec162_automatic_manifest", str(BUILDER)))
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_prepare():
    spec = importlib.util.spec_from_loader(
        "spec162_prepare_for_repo", SourceFileLoader("spec162_prepare_for_repo", str(PREPARE)))
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

    def test_injected_backend_registration_publishes_all_three_stages(self) -> None:
        from ndnsf_distributed_inference.adapters.qwen import repo_registration

        class Backend:
            last_receipts = ()

        backend = Backend()
        calls = []

        class Repo:
            def __init__(self, injected, **kwargs):
                self.backend = injected
                self.kwargs = kwargs

            def publish_file(self, path, **kwargs):
                index = len(calls)
                calls.append((Path(path), dict(kwargs)))
                self.backend.last_receipts = ({
                    "dataName": f"/repo/committed/stage-{index}",
                },)
                return types.SimpleNamespace(
                    achieved_replicas=1,
                    requested_replicas=1,
                    operation_id=f"operation-{index}",
                    reference=types.SimpleNamespace(
                        to_dict=lambda: {"stage": index}),
                )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stages = []
            for index, role in enumerate(self.roles):
                path = root / f"stage-{index}.bin"
                path.write_bytes(bytes([index + 1]) * (index + 1))
                stages.append({
                    "role": role,
                    "stageIndex": index,
                    "path": str(path),
                    "bytes": path.stat().st_size,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                })
            manifest = root / "stage-manifest.json"
            manifest.write_text(json.dumps({
                "modelDigest": "sha256:" + "d" * 64,
                "revision": "test-revision",
                "stages": stages,
            }, sort_keys=True), encoding="utf-8")
            output = root / "registration.json"
            with mock.patch.object(repo_registration, "ArtifactRepositoryApi", Repo):
                result = repo_registration.publish_qwen_stage_manifest(
                    backend=backend,
                    stage_manifest_path=manifest,
                    output_path=output,
                    publisher_identity="/example/user",
                    object_prefix="/repo/qwen",
                    deadline_ms=int(time.time() * 1000) + 10_000,
                )

            persisted = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(len(calls), 3)
        self.assertEqual(result, persisted)
        self.assertEqual(
            [item["objectName"] for item in result["artifacts"]],
            [f"/repo/committed/stage-{index}" for index in range(3)],
        )
        self.assertTrue(all(call[1]["timeout_ms"] > 0 for call in calls))

    def test_spec170_uses_content_addressed_canonical_layer_names(self) -> None:
        from ndnsf_distributed_inference.adapters.qwen import repo_registration

        class Backend:
            last_receipts = ()

        backend = Backend()
        calls = []

        class Repo:
            def __init__(self, injected, **kwargs):
                self.backend = injected

            def publish_file(self, path, **kwargs):
                index = len(calls)
                calls.append((Path(path), dict(kwargs)))
                self.backend.last_receipts = ({
                    "dataName": f"/repo/transport/stage-{index}",
                },)
                return types.SimpleNamespace(
                    achieved_replicas=1, requested_replicas=1,
                    operation_id=f"operation-{index}",
                    reference=types.SimpleNamespace(
                        to_dict=lambda: {"stage": index}),
                )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stages = []
            ranges = ((0, 9), (9, 18), (18, 28))
            for index, role in enumerate(self.roles):
                path = root / f"stage-{index}.bin"
                path.write_bytes(bytes([index + 1]) * (index + 1))
                stages.append({
                    "role": role, "stageIndex": index,
                    "layerRange": {
                        "start": ranges[index][0],
                        "endExclusive": ranges[index][1],
                    },
                    "path": str(path), "bytes": path.stat().st_size,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                })
            manifest = root / "stage-manifest.json"
            manifest.write_text(json.dumps({
                "modelDigest": "sha256:" + "d" * 64,
                "modelProfile": "qwen3-0.6b",
                "repository": "Qwen/Qwen3-0.6B",
                "revision": "test-revision",
                "layerRanges": [list(value) for value in ranges],
                "stages": stages,
            }, sort_keys=True), encoding="utf-8")
            output = root / "registration.json"
            with mock.patch.object(repo_registration, "ArtifactRepositoryApi", Repo):
                result = repo_registration.publish_qwen_stage_manifest(
                    backend=backend,
                    stage_manifest_path=manifest,
                    output_path=output,
                    publisher_identity="/example/user",
                    object_prefix="/legacy/qwen",
                    deadline_ms=int(time.time() * 1000) + 10_000,
                    canonical_model_name="Qwen/Qwen3-0.6B",
                    canonical_graph_digest="sha256:" + "e" * 64,
                    canonical_profile="qwen3-0.6b",
                    canonical_publisher="/8=example/8=user",
                )

        self.assertEqual(len(calls), 3)
        from ndn.encoding import Component
        from ndnsf_distributed_inference.naming import parse_producer_data_name
        for index, call in enumerate(calls):
            content = parse_producer_data_name(call[1]["name"], "/example/user")
            # The canonical object category is `model` in the NDNSF-DI namespace; the same value is
            # what `_publication_idempotency_key` requires before it will publish. This assertion
            # said `BLOB` and had never run, because the fixture path it loaded did not exist.
            self.assertEqual(content.category, "model")
            self.assertEqual(content.name_space, "NDNSF-DI")
            # The name is content-addressed: it carries the model identity, the layer coordinate of
            # this stage, and the payload digest as the final component. Asserting a one-component
            # suffix pinned the retired BLOB layout instead of this contract.
            labels = [bytes(Component.get_value(c)) for c in content.suffix]
            self.assertIn(b"name=Qwen/Qwen3-0.6B", labels)
            start, end = ranges[index]
            self.assertIn(f"layers={start}-{end - 1}".encode(), labels)
            self.assertIn(b"file", labels)
            self.assertIn(bytes.fromhex(stages[index]["sha256"]), labels)
        self.assertTrue(all(item["canonicalName"] for item in result["artifacts"]))
        self.assertEqual(
            [item["objectName"] for item in result["artifacts"]],
            [f"/repo/transport/stage-{index}" for index in range(3)],
        )

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

    def test_repo_storage_processes_can_use_distinct_provider_identities(self) -> None:
        import yaml
        prepare = load_prepare()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy.yaml"
            path.write_text("services: []\n", encoding="utf-8")
            prepare._add_distributed_repo_services(
                path,
                user="/example/user",
                provider_prefix="/example/compute",
                repo_provider_prefix="/example/repo",
            )
            services = yaml.safe_load(
                path.read_text(encoding="utf-8"))["services"]

        expected_users = [
            "/example/user",
            "/example/compute", "/example/compute/1", "/example/compute/2",
            "/example/repo", "/example/repo/1", "/example/repo/2",
        ]
        expected_repos = [
            "/example/repo", "/example/repo/1", "/example/repo/2",
        ]
        for service in services:
            self.assertEqual(service["users"], expected_users)
            self.assertEqual(
                [item["identity"] for item in service["providers"]],
                expected_repos,
            )

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


def test_qwen_runtime_selection_dataflow_mode_is_retired_at_argument_time():
    """The retired Repo publication must fail on the command line, not mid-run.

    `_QwenDeferredRepoPublisher.publish` builds `CollaborationArtifactApiBackend`,
    which raises `SPEC192_COMPATIBILITY_V1` on construction. A Qwen-runtime run of
    the selection-dataflow profiles would therefore reach that point only after
    NFD and the actors were already up. The CLI now refuses the combination and
    names the maintained replacement.
    """
    import subprocess

    completed = subprocess.run(
        [sys.executable, str(ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"),
         "--runtime", "qwen-onnx", "--selection-dataflow-v3"],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    combined = completed.stdout + completed.stderr
    assert "--selection-dataflow-v3 with a Qwen runtime is retired" in combined, combined
    assert "ndnsf-di cluster up" in combined
    assert "tiny-onnx fixture path is still supported" in combined
