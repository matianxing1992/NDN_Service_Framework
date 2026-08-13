#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path
from threading import Lock, Thread
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
        return types.SimpleNamespace(
            destination=path,
            transferred_bytes=len(self.payload),
            retransmitted_bytes=0,
            last_segment=0,
            delivered_segments=1,
            total_segments=1,
        )


class QwenRepoSelectionPrepareTest(unittest.TestCase):
    def test_explicit_cpu_stage_warmup_executes_without_cuda_sync(self):
        import torch

        provider_module = load_provider()

        class TinyBase(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.embed_tokens = torch.nn.Embedding(8, 4)
                self.layers = torch.nn.ModuleList()

        class TinyStage(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.model = TinyBase()
                self.config = types.SimpleNamespace(hidden_size=4)
                self.ndnsf_stage_index = 0
                self.ndnsf_stage_count = 2
                self.ndnsf_execution_device = "cpu"

        model = TinyStage()
        with mock.patch.object(torch.cuda, "synchronize") as synchronize:
            self.assertTrue(
                provider_module.warm_qwen_transformer_stage(model, "cpu"))
        synchronize.assert_not_called()

        with self.assertRaisesRegex(
                RuntimeError, "does not match committed assignment"):
            provider_module.warm_qwen_transformer_stage(model, "cuda:0")

    def test_explicit_cpu_logic_is_not_counted_as_cuda_fallback(self):
        provider_module = load_provider()
        explicit_cpu = types.SimpleNamespace(ndnsf_cpu_fallback=True)
        self.assertEqual(
            provider_module._unexpected_cpu_fallback_count(
                explicit_cpu, "cpu"),
            0,
        )
        self.assertEqual(
            provider_module._unexpected_cpu_fallback_count(
                explicit_cpu, "cuda:0"),
            1,
        )

    def test_provider_probes_required_qwen_family_before_network_ready(self):
        provider_module = load_provider()
        args = types.SimpleNamespace(
            runtime=provider_module.QWEN_TRANSFORMERS_RUNTIME,
            selection_dataflow_v2=True,
            selection_model_type="qwen3",
        )
        with mock.patch.object(
                provider_module,
                "probe_qwen_transformers_model_type",
        ) as probe:
            provider_module._preflight_qwen_runtime(args)
        probe.assert_called_once_with("qwen3")

        source = PROVIDER.read_text(encoding="utf-8")
        self.assertLess(
            source.index("_preflight_qwen_runtime(args)"),
            source.index("provider = APPProvider.from_config("),
        )

    def test_request_first_offer_lease_covers_preparation_window(self):
        provider_module = load_provider()
        with self.assertRaisesRegex(
                RuntimeError, "offer lease must cover"):
            provider_module._validate_selection_timing_window(
                offer_lease_ms=120000,
                max_prepare_ms=900000,
            )
        self.assertEqual(
            provider_module._validate_selection_timing_window(
                offer_lease_ms=900000,
                max_prepare_ms=900000,
            ),
            (900000, 900000),
        )

    def test_delayed_registration_fetches_once_and_warm_selection_reuses_gpu(self):
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
                "model_content_digest": "sha256:" + "1" * 64,
                "semantics_digest": "sha256:" + "2" * 64,
                "graph_digest": "sha256:" + "3" * 64,
                "partition_digest": "sha256:" + "4" * 64,
                "adapter_id": "qwen36-27b-pipeline",
                "adapter_version": "1.0.0",
                "precision": "bfloat16",
                "backend": "transformers",
                "layer_start": 0,
                "layer_end": 21,
            }), encoding="utf-8")
            registration = root / "registration.json"
            registration_payload = {
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
            }
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
                selection_offer_lease_ms=600000,
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
                    deadline_ms=int(time.time() * 1000) + 5000,
                    role=types.SimpleNamespace(
                        role=role,
                        artifact_digest="sha256:" + digest,
                        adapter_id="qwen36-27b-pipeline",
                        adapter_version="1.0.0",
                        backend="transformers",
                        device="cuda:0",
                        artifact=types.SimpleNamespace(
                            data_name="/repo/qwen/stage-0"),
                    ),
                )
                progress = []
                writer = Thread(target=lambda: (
                    time.sleep(0.05),
                    registration.write_text(
                        json.dumps(registration_payload), encoding="utf-8"),
                ))
                writer.start()
                with mock.patch.object(
                    provider_module, "warm_qwen_transformer_stage",
                    return_value=True,
                ) as warmup:
                    participant._callbacks.prepare_role(context)
                    evidence = config["runtime_preparer"](
                        types.SimpleNamespace(spec=types.SimpleNamespace(
                            role=role,
                            metadata={
                                "selectionRequestId": "request-1",
                                "selectionAttempt": 1,
                                "selectionArtifactDigest": "sha256:" + digest,
                            },
                        )),
                        lambda phase, value: progress.append((phase, value)),
                    )
                    participant._callbacks.release_assignment(
                        context, "RESPONSE_PUBLISHED")
                    context.request_id = "request-2"
                    context.deadline_ms = int(time.time() * 1000) + 5000
                    participant._callbacks.prepare_role(context)
                    warm_evidence = config["runtime_preparer"](
                        types.SimpleNamespace(spec=types.SimpleNamespace(
                            role=role,
                            metadata={
                                "selectionRequestId": "request-2",
                                "selectionAttempt": 1,
                                "selectionArtifactDigest": "sha256:" + digest,
                            },
                        )),
                        lambda phase, value: progress.append((phase, value)),
                    )
                    participant._callbacks.release_assignment(
                        context, "RESPONSE_PUBLISHED")
                writer.join(timeout=1.0)
                self.assertFalse(writer.is_alive())
            self.assertEqual(fake_repo.fetches, 1)
            self.assertEqual(loader.call_count, 1)
            self.assertEqual(warmup.call_count, 1)
            warmup.assert_called_once_with(model, "cuda:0")
            self.assertEqual(
                [item[0] for item in progress],
                ["LOADING", "WARMING", "LOADING", "WARMING"],
            )
            self.assertEqual(evidence.device, "cuda:0")
            self.assertEqual(warm_evidence.device, "cuda:0")
            self.assertEqual(evidence.cpu_fallback_count, 0)
            self.assertEqual(
                config["selection_participant"].boot_epoch,
                "core-boot-epoch-verified",
            )
            self.assertEqual(
                config["selection_storage_key_epoch"],
                "core-boot-epoch-verified",
            )
            self.assertIs(cache["/repo/qwen/stage-0"], model)
            self.assertIs(cache[role], model)
            self.assertEqual(
                config["selection_cached_shards"]()[0]["tier"],
                "RELOAD_SAFE_GPU",
            )


if __name__ == "__main__":
    unittest.main()
