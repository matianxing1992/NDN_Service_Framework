from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

import torch


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_LIB = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py"
)
QWEN36_RUNTIME_LOCK = (
    ROOT
    / "packaging/ndnsf-di-container/oci/layered/locks/qwen36-overlay.lock.json"
)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Qwen36StageContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.pipeline = load_module("spec162_llm_pipeline_lib", PIPELINE_LIB)

    def test_placement_adapter_is_exact_three_stage_dependency_graph(self) -> None:
        from ndnsf_distributed_inference.adapters.qwen import (
            QWEN36_27B_LAYER_RANGES,
            QWEN36_STAGE_ROLES,
            build_qwen36_27b_three_stage_adapter,
        )

        artifact_digests = {
            role: "sha256:" + str(index + 1) * 64
            for index, role in enumerate(QWEN36_STAGE_ROLES)
        }
        weight_bytes = {
            role: (16 + index) * 1024 * 1024 * 1024
            for index, role in enumerate(QWEN36_STAGE_ROLES)
        }
        adapter = build_qwen36_27b_three_stage_adapter(
            artifact_digests_by_role=artifact_digests,
            weight_bytes_by_role=weight_bytes,
        )
        model = adapter.describe_model(
            "Qwen/Qwen3.6-27B",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
            source_revision="6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        )
        graph = adapter.graph.inspect(model)
        candidate = adapter.splitter.enumerate_candidates(model, graph)[0]
        self.assertEqual(QWEN36_27B_LAYER_RANGES, ((0, 21), (21, 42), (42, 64)))
        self.assertEqual(candidate.execution_plan.roles, QWEN36_STAGE_ROLES)
        self.assertEqual(
            tuple(
                (item.producer, item.consumer)
                for item in candidate.execution_plan.dependencies
            ),
            (
                (QWEN36_STAGE_ROLES[0], QWEN36_STAGE_ROLES[1]),
                (QWEN36_STAGE_ROLES[1], QWEN36_STAGE_ROLES[2]),
            ),
        )
        self.assertEqual(
            candidate.execution_plan.node_roles["layer-20"],
            QWEN36_STAGE_ROLES[0],
        )
        self.assertEqual(
            candidate.execution_plan.node_roles["layer-21"],
            QWEN36_STAGE_ROLES[1],
        )
        self.assertEqual(
            candidate.execution_plan.node_roles["layer-42"],
            QWEN36_STAGE_ROLES[2],
        )
        self.assertTrue(all(
            requirement.estimated_peak_gpu_memory_bytes is not None
            for requirement in candidate.requirements_by_role.values()
        ))

    def test_qwen3_small_manifest_builds_its_own_28_layer_graph(self) -> None:
        from ndnsf_distributed_inference.adapters.qwen import (
            QWEN36_STAGE_ROLES,
            build_qwen_three_stage_adapter,
            build_qwen36_27b_three_stage_adapter,
        )

        artifact_digests = {
            role: "sha256:" + str(index + 4) * 64
            for index, role in enumerate(QWEN36_STAGE_ROLES)
        }
        weight_bytes = {
            role: (index + 1) * 1000
            for index, role in enumerate(QWEN36_STAGE_ROLES)
        }
        adapter = build_qwen_three_stage_adapter(
            model_name="Qwen/Qwen3-0.6B",
            revision="e6de91484c29aa9480d55605af694f39b081c455",
            layer_ranges=((0, 9), (9, 18), (18, 28)),
            artifact_digests_by_role=artifact_digests,
            weight_bytes_by_role=weight_bytes,
        )
        model = adapter.describe_model(
            "Qwen/Qwen3-0.6B",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
            source_revision="e6de91484c29aa9480d55605af694f39b081c455",
        )
        graph = adapter.graph.inspect(model)
        candidate = adapter.splitter.enumerate_candidates(model, graph)[0]

        self.assertEqual(len(graph.nodes), 30)
        self.assertNotIn("layer-28", candidate.execution_plan.node_roles)
        self.assertEqual(
            candidate.cross_partition_tensors,
            ("hidden-layer-8-to-9", "hidden-layer-17-to-18"),
        )
        self.assertEqual(
            candidate.execution_plan.node_roles["layer-27"],
            QWEN36_STAGE_ROLES[2],
        )

        large_adapter = build_qwen36_27b_three_stage_adapter(
            artifact_digests_by_role=artifact_digests,
            weight_bytes_by_role=weight_bytes,
        )
        large_model = large_adapter.describe_model(
            "Qwen/Qwen3.6-27B",
            model.content_digest,
            model.semantics_digest,
            source_revision="6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        )
        large_graph = large_adapter.graph.inspect(large_model)
        large_candidate = large_adapter.splitter.enumerate_candidates(
            large_model, large_graph)[0]
        self.assertNotEqual(graph.graph_digest, large_graph.graph_digest)
        self.assertNotEqual(
            candidate.candidate_digest, large_candidate.candidate_digest)

    def test_nested_qwen35_text_config_is_normalized(self) -> None:
        model_type, config = self.pipeline._normalize_qwen_stage_config({
            "model_type": "qwen3_5",
            "text_config": {
                "model_type": "qwen3_5_text",
                "hidden_size": 2048,
                "num_hidden_layers": 64,
            },
            "vision_config": {"model_type": "qwen3_5_vision"},
        })

        self.assertEqual(model_type, "qwen3_5")
        self.assertEqual(config["model_type"], "qwen3_5_text")
        self.assertEqual(config["num_hidden_layers"], 64)
        self.assertNotIn("vision_config", config)

    def test_flat_qwen35_text_config_uses_qwen35_runtime_family(self) -> None:
        model_type, config = self.pipeline._normalize_qwen_stage_config({
            "model_type": "qwen3_5_text",
            "hidden_size": 2048,
            "num_hidden_layers": 64,
        })

        self.assertEqual(model_type, "qwen3_5")
        self.assertEqual(config["model_type"], "qwen3_5_text")
        self.assertEqual(config["num_hidden_layers"], 64)

    def test_qwen3_text_config_remains_qwen3_runtime_family(self) -> None:
        model_type, config = self.pipeline._normalize_qwen_stage_config({
            "model_type": "qwen3",
            "hidden_size": 1024,
            "num_hidden_layers": 28,
        })

        self.assertEqual(model_type, "qwen3")
        self.assertEqual(config["model_type"], "qwen3")
        self.assertEqual(config["num_hidden_layers"], 28)

    def test_transformers_runtime_guard_is_fail_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "QWEN3_5_TRANSFORMERS_TOO_OLD"):
            self.pipeline._require_qwen_transformers_runtime(
                "qwen3_5", installed_version="4.57.1")

        self.pipeline._require_qwen_transformers_runtime(
            "qwen3_5", installed_version="5.14.1")
        self.pipeline._require_qwen_transformers_runtime(
            "qwen2", installed_version="4.48.2")

    def test_qwen36_runtime_overlay_is_immutable_and_not_active(self) -> None:
        lock = json.loads(QWEN36_RUNTIME_LOCK.read_text(encoding="utf-8"))

        self.assertEqual(lock["schemaVersion"], "ndnsf-di-runtime-overlay-lock-v1")
        self.assertEqual(lock["state"], "CANDIDATE_UNMATERIALIZED")
        self.assertEqual(lock["baseLock"], "ml-runtime.lock.json")
        self.assertEqual(lock["pythonRuntime"], "3.10.18-bullseye-glibc2.31")
        self.assertEqual(lock["pythonPackages"]["transformers"]["version"], "5.14.1")
        self.assertEqual(
            lock["pythonPackages"]["transformers"]["sha256"],
            "9db974c4079ede2d1a3ea7ca5a240df33f2cc26fc2b36ba64c5f2a4f43b6e725",
        )
        self.assertFalse(lock["cpuFallbackAllowed"])

    def test_qwen35_conditional_state_dict_is_remapped_strictly(self) -> None:
        tensors = {
            "model.language_model.embed_tokens.weight": object(),
            "model.language_model.layers.20.input_layernorm.weight": object(),
            "model.language_model.layers.21.input_layernorm.weight": object(),
            "model.language_model.layers.41.mlp.down_proj.weight": object(),
            "model.language_model.layers.42.input_layernorm.weight": object(),
            "model.language_model.norm.weight": object(),
            "lm_head.weight": object(),
            "model.visual.blocks.0.weight": object(),
        }

        remapped = self.pipeline._remap_qwen_stage_state_dict(
            tensors,
            start=21,
            local_layer_count=21,
            include_embedding=False,
            include_final=False,
        )

        self.assertEqual(
            set(remapped),
            {
                "model.layers.0.input_layernorm.weight",
                "model.layers.20.mlp.down_proj.weight",
            },
        )
        self.assertIs(
            remapped["model.layers.0.input_layernorm.weight"],
            tensors["model.language_model.layers.21.input_layernorm.weight"],
        )

    def test_qwen35_stage_export_selects_nested_text_weights_only(self) -> None:
        tensors = {
            key: torch.tensor([index], dtype=torch.float32)
            for index, key in enumerate((
                "model.language_model.embed_tokens.weight",
                "model.language_model.layers.0.input_layernorm.weight",
                "model.language_model.layers.20.mlp.down_proj.weight",
                "model.language_model.layers.21.input_layernorm.weight",
                "model.language_model.norm.weight",
                "model.visual.blocks.0.weight",
                "mtp.layers.0.weight",
                "lm_head.weight",
            ))
        }
        spec = {
            "stageIndex": 0,
            "stageCount": 3,
            "layerRange": {"start": 0, "endExclusive": 21},
        }

        selected = self.pipeline._stage_state_dict(tensors, spec)

        self.assertEqual(
            set(selected),
            {
                "model.language_model.embed_tokens.weight",
                "model.language_model.layers.0.input_layernorm.weight",
                "model.language_model.layers.20.mlp.down_proj.weight",
            },
        )

    def test_qwen2_state_dict_mapping_is_preserved(self) -> None:
        tensors = {
            "model.embed_tokens.weight": object(),
            "model.layers.0.self_attn.q_proj.weight": object(),
            "model.layers.1.self_attn.q_proj.weight": object(),
            "model.norm.weight": object(),
            "lm_head.weight": object(),
        }
        remapped = self.pipeline._remap_qwen_stage_state_dict(
            tensors,
            start=0,
            local_layer_count=1,
            include_embedding=True,
            include_final=False,
        )
        self.assertEqual(
            set(remapped),
            {
                "model.embed_tokens.weight",
                "model.layers.0.self_attn.q_proj.weight",
            },
        )

    def test_qwen2_stage_loader_still_strict_loads(self) -> None:
        from transformers import Qwen2Config, Qwen2ForCausalLM

        config = Qwen2Config(
            vocab_size=32,
            hidden_size=16,
            intermediate_size=32,
            num_hidden_layers=1,
            num_attention_heads=2,
            num_key_value_heads=2,
            max_position_embeddings=32,
        )
        full_model = Qwen2ForCausalLM(config)
        full_model.to(dtype=torch.bfloat16)
        package = {
            "spec": {
                "stageIndex": 0,
                "stageCount": 1,
                "layerCount": 1,
                "layerRange": {"start": 0, "endExclusive": 1},
            },
            "config": config.to_dict(),
            "state_dict": full_model.state_dict(),
            "attnImplementation": "sdpa",
        }
        with tempfile.TemporaryDirectory(prefix="spec162-qwen2-") as tmp:
            artifact = Path(tmp) / "stage.pt"
            torch.save(package, artifact)
            loaded = self.pipeline.qwen_transformer_model_from_stage_package(
                artifact, device="cpu")

        self.assertEqual(loaded.ndnsf_model_type, "qwen2")
        self.assertEqual(loaded.ndnsf_stage_start, 0)
        self.assertEqual(next(loaded.parameters()).dtype, torch.bfloat16)
        self.assertEqual(loaded.ndnsf_stage_end, 1)
        self.assertFalse(loaded.ndnsf_cpu_fallback)

    def test_qwen35_positions_follow_four_plane_contract(self) -> None:
        input_ids = torch.tensor([[1, 2, 3], [4, 5, 6]])
        positions = self.pipeline._qwen_position_ids(input_ids, "qwen3_5")

        self.assertEqual(tuple(positions.shape), (4, 2, 3))
        self.assertTrue(torch.equal(positions[0], positions[1]))
        layer_positions, rotary_positions = (
            self.pipeline._qwen_layer_position_inputs(positions, "qwen3_5")
        )
        self.assertEqual(tuple(layer_positions.shape), (2, 3))
        self.assertEqual(tuple(rotary_positions.shape), (3, 2, 3))

    def test_qwen2_positions_remain_two_dimensional(self) -> None:
        input_ids = torch.tensor([[1, 2, 3]])
        positions = self.pipeline._qwen_position_ids(input_ids, "qwen2")
        layer_positions, rotary_positions = (
            self.pipeline._qwen_layer_position_inputs(positions, "qwen2")
        )

        self.assertEqual(tuple(positions.shape), (1, 3))
        self.assertIs(layer_positions, positions)
        self.assertIs(rotary_positions, positions)

    def test_hybrid_layer_selects_its_own_mask(self) -> None:
        full_mask = object()
        linear_mask = object()
        masks = {
            "full_attention": full_mask,
            "linear_attention": linear_mask,
        }

        class Layer:
            block_type = "linear_attention"

        self.assertIs(
            self.pipeline._qwen_attention_mask_for_layer(masks, Layer()),
            linear_mask,
        )
        self.assertIs(
            self.pipeline._qwen_attention_mask_for_layer(full_mask, object()),
            full_mask,
        )

    def test_qwen35_mask_builder_matches_transformers_contract(self) -> None:
        calls = []
        masking = types.ModuleType("transformers.masking_utils")

        def create_causal_mask(**kwargs):
            calls.append(("full", kwargs))
            return "full-mask"

        def create_recurrent_attention_mask(**kwargs):
            calls.append(("linear", kwargs))
            return "linear-mask"

        masking.create_causal_mask = create_causal_mask
        masking.create_recurrent_attention_mask = create_recurrent_attention_mask
        input_ids = torch.tensor([[1, 2, 3]])
        hidden = torch.zeros((1, 3, 4))
        positions = self.pipeline._qwen_position_ids(input_ids, "qwen3_5")
        base = types.SimpleNamespace(config=object())

        with mock.patch.dict(sys.modules, {
            "transformers.masking_utils": masking,
        }):
            masks = self.pipeline._build_qwen_attention_masks(
                base, input_ids, hidden, positions, "qwen3_5")

        self.assertEqual(masks, {
            "full_attention": "full-mask",
            "linear_attention": "linear-mask",
        })
        self.assertEqual([kind for kind, _kwargs in calls], ["full", "linear"])
        for _kind, kwargs in calls:
            self.assertIs(kwargs["config"], base.config)
            self.assertIs(kwargs["inputs_embeds"], hidden)
            self.assertIsNone(kwargs["attention_mask"])
            self.assertIsNone(kwargs["past_key_values"])
            self.assertEqual(tuple(kwargs["position_ids"].shape), (1, 3))

    def test_non_thinking_chat_format_is_explicit(self) -> None:
        calls = []

        class Tokenizer:
            def apply_chat_template(self, messages, **kwargs):
                calls.append((messages, kwargs))
                return [101, 102]

        token_ids = self.pipeline.format_qwen_chat_prompt(
            Tokenizer(), "Explain NDN briefly.")

        self.assertEqual(token_ids, [101, 102])
        self.assertEqual(calls[0][0], [
            {"role": "user", "content": "Explain NDN briefly."},
        ])
        self.assertFalse(calls[0][1]["enable_thinking"])
        self.assertTrue(calls[0][1]["add_generation_prompt"])
        self.assertTrue(calls[0][1]["tokenize"])
        self.assertFalse(calls[0][1]["return_dict"])


if __name__ == "__main__":
    unittest.main()
