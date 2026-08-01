#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import sys
import tempfile
import types
import unittest
from pathlib import Path
from threading import Lock
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_DIR = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline")
PROVIDER = PIPELINE_DIR / "provider.py"


def load_provider():
    sys.path.insert(0, str(PIPELINE_DIR))
    try:
        spec = importlib.util.spec_from_file_location(
            "spec162_repo_provider", PROVIDER)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.remove(str(PIPELINE_DIR))


class FakeRepo:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.fetches = 0

    def fetch_file(self, reference, destination, **_kwargs):
        self.fetches += 1
        if reference.content_digest != hashlib.sha256(self.payload).hexdigest():
            raise AssertionError(reference.content_digest)
        if int(reference.size_bytes) != len(self.payload):
            raise AssertionError(reference.size_bytes)
        path = Path(destination)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.payload)
        return path


class QwenRepoSelectionPrepareTest(unittest.TestCase):
    def test_first_selection_fetches_once_and_warm_selection_reuses_gpu(self):
        provider_module = load_provider()
        payload = b"qwen-stage-fixture"
        digest = hashlib.sha256(payload).hexdigest()
        role = "/LLM/Pipeline/Stage/0"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("storage.key", "signing.key"):
                (root / name).write_bytes(b"k" * 32)
            (root / "residency.json").write_text(json.dumps({
                "role": role,
                "artifact_digest": "sha256:" + digest,
                "layer_start": 0,
                "layer_end": 21,
            }), encoding="utf-8")
            registration = root / "registration.json"
            registration.write_text(json.dumps({
                "schemaVersion": "ndnsf-di-qwen36-repo-registration-v1",
                "artifacts": [{
                    "role": role,
                    "fileSha256": "sha256:" + digest,
                    "fileBytes": len(payload),
                    "objectName": "/repo/qwen/stage-0",
                    "artifactReference": {
                        "logicalName": "/qwen/stage-0",
                        "digestAlgorithm": "sha256",
                        "contentDigest": digest,
                        "sizeBytes": len(payload),
                        "formatVersion": "artifact-manifest-v2",
                        "rootManifestName": "/qwen/stage-0/root",
                        "publisherIdentity": "/example/user",
                        "policyEpoch": "test",
                    },
                    "receipts": [],
                }],
            }), encoding="utf-8")
            args = types.SimpleNamespace(
                selection_dataflow_v2=True,
                provider_identity="/example/provider",
                provider_boot_epoch="stale-cli-epoch",
                selection_wal_path=str(root / "selection.wal"),
                selection_storage_key_file=str(root / "storage.key"),
                selection_signing_key_file=str(root / "signing.key"),
                selection_residency_json=str(root / "residency.json"),
                selection_gpu_capacity_mib=32760,
                selection_offered_gpu_mib=20000,
                selection_offer_lease_ms=120000,
                selection_max_prepare_ms=600000,
                selection_residency_ttl_ms=600000,
                selection_repo_registration=str(registration),
                selection_model_cache_dir=str(root / "model-cache"),
                config=str(root / "policy.yaml"),
                generated_policy_dir=str(root / "generated"),
                repo_client_state_root=str(root / "repo-client-state"),
                device="cuda:0",
                require_cuda=True,
            )
            fake_app_provider = types.SimpleNamespace(
                _network_provider=types.SimpleNamespace(
                    _provider=types.SimpleNamespace(
                        provider=types.SimpleNamespace(
                            provider_boot_epoch="core-boot-epoch-verified",
                        ),
                    ),
                ),
                deployment=types.SimpleNamespace(
                    service_policy=lambda _service: types.SimpleNamespace(
                        artifacts=[])))
            fake_repo = FakeRepo(payload)
            model = types.SimpleNamespace(
                ndnsf_execution_device="cuda:0",
                ndnsf_cpu_fallback=False,
            )
            cache = {}
            with self.assertRaisesRegex(
                    RuntimeError, "does not match NDNSF Core epoch"):
                provider_module._selection_v2_for_qwen(
                    fake_app_provider,
                    args,
                    model_cache=cache,
                    model_cache_lock=Lock(),
                )
            args.provider_boot_epoch = "core-boot-epoch-verified"
            with mock.patch(
                "py_repoclient.CollaborationArtifactApiBackend.from_config",
                return_value=object(),
            ), mock.patch(
                "py_repoclient.ArtifactRepositoryApi",
                return_value=fake_repo,
            ), mock.patch.object(
                provider_module,
                "qwen_transformer_model_from_stage_package",
                return_value=model,
            ) as loader:
                config = provider_module._selection_v2_for_qwen(
                    fake_app_provider,
                    args,
                    model_cache=cache,
                    model_cache_lock=Lock(),
                )
                participant = config["selection_participant"]
                context = types.SimpleNamespace(
                    request_id="request-1",
                    role=types.SimpleNamespace(
                        role=role,
                        artifact=types.SimpleNamespace(
                            data_name="/repo/qwen/stage-0"),
                    ),
                )
                participant._callbacks.prepare_role(context)
                context.request_id = "request-2"
                participant._callbacks.prepare_role(context)
            self.assertEqual(fake_repo.fetches, 1)
            self.assertEqual(loader.call_count, 1)
            self.assertEqual(
                config["selection_participant"].boot_epoch,
                "core-boot-epoch-verified",
            )
            self.assertEqual(
                config["selection_storage_key_epoch"],
                "core-boot-epoch-verified",
            )
            self.assertIs(cache["/repo/qwen/stage-0"], model)
            self.assertEqual(
                config["selection_cached_shards"]()[0]["tier"],
                "RELOAD_SAFE_GPU",
            )


if __name__ == "__main__":
    unittest.main()
