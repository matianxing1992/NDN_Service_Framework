from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import onnxruntime as ort

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference"))

from ndnsf_distributed_inference.adapters.qwen.stateful_onnx import (  # noqa: E402
    DecodeStateBundleV1,
    DecodeStateContractError,
    DecodeStateIdentityV1,
    DecodeStateTransaction,
    PersistentStatefulOnnxSession,
    StatefulOnnxIOContractV1,
    StateComponent,
)
from ndnsf_distributed_inference.adapters.qwen.placement import (  # noqa: E402
    QWEN36_27B_PRECISION,
    QWEN36_STAGE_ROLES,
    build_qwen36_27b_three_stage_adapter,
)


def load_exporter():
    path = ROOT / "tools/ndnsf-di/export_spec175_qwen36_stateful_onnx.py"
    spec = importlib.util.spec_from_file_location("spec175_qwen36_exporter", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_stage_manifest_builder():
    path = ROOT / (
        "specs/175-ndnsf-di-streamed-invocation/jobs/"
        "build-qwen-onnx-stage-manifest.py")
    spec = importlib.util.spec_from_file_location(
        "spec175_qwen_stage_manifest_builder", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_artifact_copier():
    path = ROOT / (
        "specs/175-ndnsf-di-streamed-invocation/jobs/"
        "copy-qwen-onnx-artifact.py")
    spec = importlib.util.spec_from_file_location(
        "spec175_qwen_artifact_copier", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_reference_generator():
    path = ROOT / (
        "specs/175-ndnsf-di-streamed-invocation/jobs/"
        "generate-qwen-onnx-reference.py")
    spec = importlib.util.spec_from_file_location(
        "spec175_qwen_reference_generator", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(char: str) -> str:
    return "sha256:" + char * 64


def identity(token_epoch: int = 0) -> DecodeStateIdentityV1:
    components = (digest("a"), digest("b"))
    return DecodeStateIdentityV1(
        model_digest=digest("1"), graph_semantic_digest=digest("2"),
        artifact_digest=digest("3"), adapter_digest=digest("4"),
        tokenizer_digest=digest("5"), runner_digest=digest("6"),
        role_name="/LLM/Pipeline/Stage/0", role_split_digest=digest("7"),
        layer_range=(0, 4),
        prefix_digest=digest("8" if token_epoch == 0 else "0"),
        prefix_token_count=token_epoch,
        position_digest=digest("9" if token_epoch == 0 else "1"),
        precision="float32", layout_digest=digest("c"),
        state_schema_digest=digest("d"), state_component_digests=components,
        runtime_abi_digest=digest("e"), security_domain_digest=digest("f"),
        provider_identity="/provider/0", provider_boot_id="boot-1",
        state_inference_epoch=token_epoch,
        predecessor_inference_epoch=(None if token_epoch == 0 else token_epoch - 1),
        cache_epoch=0, request_id="/request/1", attempt_epoch=1,
        generation_id="generation-1")


def bundle(token_epoch: int = 0) -> DecodeStateBundleV1:
    return DecodeStateBundleV1(
        identity(token_epoch),
        (StateComponent("attn.kv", "float16", (2, 1, 4, 8), digest("a")),),
        (StateComponent("linear.conv", "float16", (1, 8), digest("b")),),
        token_epoch=token_epoch)


class Spec175StatefulOnnxTests(unittest.TestCase):
    def test_artifact_promotion_rewrites_partial_manifest_paths(self) -> None:
        copier = load_artifact_copier()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "output"
            stages = output / "qwen-onnx-stage-artifacts"
            tokenizer = output / "qwen-onnx-tokenizer"
            stages.mkdir(parents=True)
            tokenizer.mkdir()
            stage = stages / "stage-0-qwen.onnx"
            stage.write_bytes(b"stage")
            (tokenizer / "tokenizer.json").write_text("{}")
            (output / "qwen-onnx-service-manifest.json").write_text(
                json.dumps({"stages": [{"path": str(stage)}]}))
            artifact = root / ".candidate.partial"
            with patch.object(sys, "argv", [
                    "copy-qwen-onnx-artifact.py", str(output), str(artifact)]):
                copier.main()
            manifest = json.loads(
                (artifact / "qwen-onnx-service-manifest.json").read_text())
            self.assertEqual(manifest["artifactRoot"], str(artifact))
            self.assertEqual(
                manifest["stages"][0]["path"],
                str(artifact / "qwen-onnx-stage-artifacts" / stage.name))
            self.assertTrue(Path(manifest["stages"][0]["path"]).is_file())

    def test_qwen_head_dim_uses_explicit_projection_width(self) -> None:
        from llm_pipeline.llm_pipeline_lib import _qwen_head_dim

        # Qwen3.6-27B has hidden_size=5120 and 24 attention heads, but its
        # projection/cache width is explicitly 256 (not integer division 213).
        config = argparse.Namespace(
            hidden_size=5120, num_attention_heads=24, head_dim=256)
        self.assertEqual(_qwen_head_dim(config), 256)

    def test_stateful_export_colocates_external_data_with_graph(self) -> None:
        import types
        import torch
        import onnx
        import llm_pipeline.llm_pipeline_lib as llm_lib

        config = types.SimpleNamespace(
            hidden_size=8,
            num_attention_heads=2,
            num_key_value_heads=1,
            head_dim=4,
            linear_num_value_heads=1,
            linear_key_head_dim=2,
            linear_value_head_dim=2,
            linear_num_key_heads=1,
            linear_conv_kernel_dim=2,
        )
        model = types.SimpleNamespace(
            config=config,
            ndnsf_stage_index=0,
            ndnsf_stage_count=2,
            ndnsf_stage_start=0,
            ndnsf_stage_end=2,
            ndnsf_model_type="qwen3_5",
            model=types.SimpleNamespace(layers=[
                types.SimpleNamespace(block_type="full_attention"),
                types.SimpleNamespace(block_type="linear_attention"),
            ]),
            parameters=lambda: iter((torch.zeros(1, dtype=torch.float32),)),
        )
        seen = {}

        def fake_export(_wrapper, _args, filename, **_kwargs):
            seen["cwd"] = Path.cwd()
            seen["filename"] = filename
            inputs = [
                onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, [1])
                for name in reversed(_kwargs["input_names"])
            ]
            outputs = [
                onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, [1])
                for name in reversed(_kwargs["output_names"])
            ]
            nodes = [onnx.helper.make_node(
                "Identity", ["hidden_states"], [name])
                for name in _kwargs["output_names"]]
            graph = onnx.helper.make_graph(nodes, "test", inputs, outputs)
            onnx.save(onnx.helper.make_model(
                graph, opset_imports=[onnx.helper.make_opsetid("", 17)]), filename)

        original_cwd = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nested" / "stage.onnx"
            with patch.object(llm_lib, "_onnx_stage_wrapper",
                              return_value=(object(), 0, 2, 0, 2)), \
                 patch.object(torch.onnx, "export", side_effect=fake_export):
                info = llm_lib._export_qwen_onnx_stage(
                    model, path, sample_input_ids=torch.ones((1, 1), dtype=torch.long),
                    export_dtype="float32", stateful=True)
            self.assertEqual(seen["cwd"], path.parent.resolve())
            self.assertEqual(seen["filename"], path.name)
            self.assertEqual(Path.cwd(), original_cwd)
            self.assertEqual(info["sequencePolicy"], "stateful-prefill-decode-v1")
            self.assertEqual(
                info["stateInputNames"],
                ["attention_kv_in", "recurrent_state_in", "convolution_state_in"])
            self.assertEqual(
                info["stateOutputNames"],
                ["attention_kv_out", "recurrent_state_out", "convolution_state_out"])

    def test_tiny_qwen_stateful_export_matches_eager_prefill_and_decode(self) -> None:
        """Exercise one graph for variable-length prefill and one-token decode.

        The host test environment used by the ordinary Python suite may not
        carry the adapter-certified Qwen3.5 Transformers release.  In that
        case this remains an explicit skip; the same test is executed inside
        the exporter SIF where the Qwen3.5 implementation is available.
        """
        try:
            import torch
            from transformers.models.qwen3_5.configuration_qwen3_5 import (
                Qwen3_5TextConfig,
            )
            from transformers.models.qwen3_5.modeling_qwen3_5 import (
                Qwen3_5ForCausalLM,
            )
            from llm_pipeline.llm_pipeline_lib import (  # noqa: WPS433
                _export_qwen_onnx_stage,
                _onnx_stage_wrapper,
            )
        except (ImportError, ModuleNotFoundError) as error:
            self.skipTest(f"Qwen3.5 exporter dependencies unavailable: {error}")

        config = Qwen3_5TextConfig(
            vocab_size=128,
            hidden_size=64,
            intermediate_size=128,
            num_hidden_layers=4,
            num_attention_heads=4,
            num_key_value_heads=2,
            head_dim=16,
            layer_types=[
                "linear_attention", "linear_attention",
                "linear_attention", "full_attention",
            ],
            linear_key_head_dim=8,
            linear_value_head_dim=8,
            linear_num_key_heads=2,
            linear_num_value_heads=4,
            linear_conv_kernel_dim=4,
            tie_word_embeddings=False,
        )
        model = Qwen3_5ForCausalLM(config).eval()
        model.ndnsf_stage_index = 0
        model.ndnsf_stage_count = 1
        model.ndnsf_stage_start = 0
        model.ndnsf_stage_end = 4
        model.ndnsf_model_type = "qwen3_5"
        wrapper, *_ = _onnx_stage_wrapper(model, stateful=True)

        input_ids = torch.tensor([[1, 2, 3]], dtype=torch.long)
        attention_mask = torch.ones((1, 3), dtype=torch.long)
        position_ids = torch.arange(3, dtype=torch.long).view(1, 1, -1).expand(
            4, 1, -1)
        hidden_states = torch.zeros((1, 3, 64), dtype=torch.float32)
        initial_state = (
            torch.empty((2, 1, 1, 2, 0, 16), dtype=torch.float32),
            torch.zeros((3, 1, 4, 8, 8), dtype=torch.float32),
            torch.zeros((3, 1, 64, 4), dtype=torch.float32),
        )

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tiny-qwen35-stateful.onnx"
            info = _export_qwen_onnx_stage(
                model,
                path,
                sample_input_ids=input_ids,
                export_dtype="float32",
                stateful=True,
            )
            self.assertEqual(info["sequencePolicy"], "stateful-prefill-decode-v1")
            document = onnx.load(str(path), load_external_data=False)

            def non_tensor_values(graph):
                values = [*graph.input, *graph.output, *graph.value_info]
                for value in values:
                    if value.type.WhichOneof("value") != "tensor_type":
                        yield value.name
                for node in graph.node:
                    for attribute in node.attribute:
                        if attribute.type == onnx.AttributeProto.GRAPH:
                            yield from non_tensor_values(attribute.g)
                        elif attribute.type == onnx.AttributeProto.GRAPHS:
                            for subgraph in attribute.graphs:
                                yield from non_tensor_values(subgraph)

            self.assertEqual(list(non_tensor_values(document.graph)), [])
            session = ort.InferenceSession(
                str(path), providers=["CPUExecutionProvider"])

            eager_prefill = wrapper(
                input_ids, attention_mask, hidden_states, position_ids,
                *initial_state)
            feed = {
                "input_ids": input_ids.numpy(),
                "attention_mask": attention_mask.numpy(),
                "position_ids": position_ids.numpy(),
                "hidden_states": hidden_states.numpy(),
                "attention_kv_in": initial_state[0].numpy(),
                "recurrent_state_in": initial_state[1].numpy(),
                "convolution_state_in": initial_state[2].numpy(),
            }
            feed = {
                value.name: feed[value.name]
                for value in session.get_inputs()
            }
            ort_prefill = session.run(None, feed)
            for expected, actual in zip(eager_prefill, ort_prefill):
                np.testing.assert_allclose(
                    expected.detach().numpy(), actual, rtol=2e-5, atol=2e-6)

            decode_ids = torch.tensor([[4]], dtype=torch.long)
            decode_mask = torch.ones((1, 4), dtype=torch.long)
            decode_positions = torch.full((4, 1, 1), 3, dtype=torch.long)
            decode_hidden = torch.zeros((1, 1, 64), dtype=torch.float32)
            decode_state = tuple(torch.from_numpy(value) for value in ort_prefill[1:])
            eager_decode = wrapper(
                decode_ids, decode_mask, decode_hidden, decode_positions,
                *decode_state)
            decode_feed = {
                "input_ids": decode_ids.numpy(),
                "attention_mask": decode_mask.numpy(),
                "position_ids": decode_positions.numpy(),
                "hidden_states": decode_hidden.numpy(),
                "attention_kv_in": decode_state[0].numpy(),
                "recurrent_state_in": decode_state[1].numpy(),
                "convolution_state_in": decode_state[2].numpy(),
            }
            decode_feed = {
                value.name: decode_feed[value.name]
                for value in session.get_inputs()
            }
            ort_decode = session.run(None, decode_feed)
            for expected, actual in zip(eager_decode, ort_decode):
                np.testing.assert_allclose(
                    expected.detach().numpy(), actual, rtol=2e-5, atol=2e-6)

    def test_reference_generator_carries_all_state_families_incrementally(self) -> None:
        reference = load_reference_generator()

        class Value:
            def __init__(self, name, shape, type_name="tensor(float)"):
                self.name = name
                self.shape = shape
                self.type = type_name

        class Session:
            def get_inputs(self):
                return [
                    Value("input_ids", [1, "tokens"], "tensor(int64)"),
                    Value("attention_mask", [1, "prefix"], "tensor(int64)"),
                    Value("position_ids", [1, "tokens"], "tensor(int64)"),
                    Value("hidden_states", [1, "tokens", 4]),
                    Value("attention_kv_in", [1, "past", 4]),
                    Value("recurrent_state_in", [1, 4]),
                    Value("convolution_state_in", [1, 4]),
                ]

            def get_outputs(self):
                return [
                    Value("hidden_states_out", [1, "tokens", 4]),
                    Value("attention_kv_out", [1, "past", 4]),
                    Value("recurrent_state_out", [1, 4]),
                    Value("convolution_state_out", [1, 4]),
                ]

        metadata = {
            "sequencePolicy": "stateful-prefill-decode-v1",
            "stateInputNames": [
                "attention_kv_in", "recurrent_state_in",
                "convolution_state_in"],
            "stateOutputNames": [
                "attention_kv_out", "recurrent_state_out",
                "convolution_state_out"],
            "tensorContracts": {
                "attention_kv_in": {"initialShape": [1, 0, 4]},
                "recurrent_state_in": {"initialShape": [1, 4]},
                "convolution_state_in": {"initialShape": [1, 4]},
            },
        }
        session = Session()
        ids = np.asarray([[1, 2]], dtype=np.int64)
        feed = reference.stage_feed(
            session, input_ids=ids,
            attention=np.ones((1, 2), dtype=np.int64),
            positions=np.asarray([[0, 1]], dtype=np.int64),
            hidden=np.zeros((1, 2, 4), dtype=np.float32),
            metadata=metadata, state={})
        self.assertEqual(feed["attention_kv_in"].shape, (1, 0, 4))
        outputs = [
            np.zeros((1, 2, 4), dtype=np.float32),
            np.ones((1, 2, 4), dtype=np.float32),
            np.ones((1, 4), dtype=np.float32) * 2,
            np.ones((1, 4), dtype=np.float32) * 3,
        ]
        state = reference.update_stage_state(
            session, outputs, metadata, {})
        decode_feed = reference.stage_feed(
            session, input_ids=np.asarray([[3]], dtype=np.int64),
            attention=np.ones((1, 3), dtype=np.int64),
            positions=np.asarray([[2]], dtype=np.int64),
            hidden=np.zeros((1, 1, 4), dtype=np.float32),
            metadata=metadata, state=state)
        np.testing.assert_array_equal(
            decode_feed["attention_kv_in"], outputs[1])
        np.testing.assert_array_equal(
            decode_feed["recurrent_state_in"], outputs[2])
        np.testing.assert_array_equal(
            decode_feed["convolution_state_in"], outputs[3])

    def test_stage_manifest_builder_rejects_legacy_past_key_graph(self) -> None:
        builder = load_stage_manifest_builder()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "qwen-onnx-stage-artifacts"
            artifact.mkdir()
            tokenizer = root / "qwen-onnx-tokenizer"
            tokenizer.mkdir()
            (tokenizer / "tokenizer.json").write_text("{}")
            stages = []
            for index, (start, end) in enumerate(((0, 21), (21, 42), (42, 64))):
                filename = f"stage-{index}-qwen.onnx"
                path = artifact / filename
                path.write_bytes(f"legacy-stage-{index}".encode())
                import hashlib
                stages.append({
                    "path": filename,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "stageIndex": index,
                    "layerRange": {"start": start, "endExclusive": end},
                    "inputNames": ["input_ids", "past_key.0"],
                    "outputNames": ["logits", "present_key.0"],
                    "stateInputNames": [],
                    "stateOutputNames": [],
                })
            service = root / "qwen-onnx-service-manifest.json"
            service.write_text(json.dumps({
                "schema": "ndnsf-di-qwen-onnx-service-manifest-v1",
                "runtime": "onnxruntime",
                "modelType": "qwen3_5",
                "modelRevision": "test-revision",
                "dtype": "float16",
                "sequencePolicy": "stateful-prefill-decode-v1",
                "positionInputPolicy": "qwen-causal-position-v1",
                "contextLength": 96,
                "promptLength": 20,
                "promptIds": [1] * 20,
                "padTokenId": 0,
                "layerCount": 64,
                "layerRanges": [s["layerRange"] for s in stages],
                "referenceTopToken": 1,
                "decodeMode": "single-token-autoregressive",
                "modality": "text-only",
                "mtpEnabled": False,
                "thinkingMode": "disabled",
                "graphComponents": ["token-embedding", "decoder-layers", "lm-head"],
                "stages": stages,
            }))
            output = root / "stage-manifest.json"
            args = [
                "build-qwen-onnx-stage-manifest.py",
                "--service-manifest", str(service),
                "--artifact-root", str(root),
                "--output", str(output),
                "--runtime-sif-sha256", "a" * 64,
                "--source-bundle-sha256", "b" * 64,
                "--capacity-decision-sha256", "c" * 64,
            ]
            with patch.object(sys, "argv", args):
                with self.assertRaises(SystemExit) as caught:
                    builder.main()
            self.assertIn("QWEN_STATE_STATEINPUTNAMES_REQUIRED", str(caught.exception))

    def test_stage_manifest_builder_emits_prefixed_stage_digests(self) -> None:
        builder = load_stage_manifest_builder()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "qwen-onnx-stage-artifacts"
            artifact.mkdir()
            tokenizer = root / "qwen-onnx-tokenizer"
            tokenizer.mkdir()
            (tokenizer / "tokenizer.json").write_text("{}")
            state_in = ["attention_kv_in", "recurrent_state_in",
                        "convolution_state_in"]
            state_out = ["attention_kv_out", "recurrent_state_out",
                         "convolution_state_out"]
            stages = []
            for index, (start, end) in enumerate(((0, 21), (21, 42), (42, 64))):
                filename = f"stage-{index}-qwen.onnx"
                path = artifact / filename
                path.write_bytes(f"stateful-stage-{index}".encode())
                stages.append({
                    "path": filename,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "stageIndex": index,
                    "layerRange": {"start": start, "endExclusive": end},
                    "inputNames": ["input_ids", "attention_mask", "position_ids",
                                   *state_in],
                    "outputNames": ["hidden_states_out", *state_out],
                    "stateInputNames": state_in,
                    "stateOutputNames": state_out,
                })
            service = root / "qwen-onnx-service-manifest.json"
            service.write_text(json.dumps({
                "schema": "ndnsf-di-qwen-onnx-service-manifest-v1",
                "runtime": "onnxruntime",
                "modelType": "qwen3_5",
                "modelRevision": "test-revision",
                "dtype": "float16",
                "sequencePolicy": "stateful-prefill-decode-v1",
                "positionInputPolicy": "qwen-causal-position-v1",
                "contextLength": 96,
                "promptLength": 20,
                "promptIds": [1] * 20,
                "padTokenId": 0,
                "layerCount": 64,
                "layerRanges": [[0, 21], [21, 42], [42, 64]],
                "referenceTopToken": 1,
                "decodeMode": "single-token-autoregressive",
                "modality": "text-only",
                "mtpEnabled": False,
                "thinkingMode": "disabled",
                "graphComponents": ["token-embedding", "decoder-layers", "lm-head"],
                "stages": stages,
            }))
            output = root / "stage-manifest.json"
            args = [
                "build-qwen-onnx-stage-manifest.py",
                "--service-manifest", str(service), "--artifact-root", str(root),
                "--output", str(output), "--runtime-sif-sha256", "a" * 64,
                "--source-bundle-sha256", "b" * 64,
                "--capacity-decision-sha256", "c" * 64,
            ]
            with patch.object(sys, "argv", args):
                builder.main()
            manifest = json.loads(output.read_text())
            self.assertTrue(all(stage["sha256"].startswith("sha256:")
                                for stage in manifest["stages"]))

    def test_stage_readiness_runner_is_onnx_only_and_paired(self) -> None:
        runner = ROOT / (
            "specs/175-ndnsf-di-streamed-invocation/jobs/"
            "run-qwen-stage-readiness.py")
        text = runner.read_text(encoding="utf-8")
        self.assertIn("onnxruntime", text)
        self.assertIn("io_binding", text)
        self.assertIn("stage-device-ids", text)
        self.assertIn("one distinct GPU per stage", text)
        self.assertIn('"cuda" if output_name in STATE_OUTPUTS else "cpu"', text)
        self.assertIn("attribute() if callable(attribute) else attribute", text)
        self.assertNotIn("copy_outputs_to_cpu", text)
        self.assertIn("pair_order", text)
        self.assertIn("pairCount", text)
        self.assertNotIn("import transformers", text.lower())
        self.assertNotIn("import torch", text.lower())

    def test_pinned_qwen_profile_is_fp16_text_only_single_token(self) -> None:
        adapter = build_qwen36_27b_three_stage_adapter(
            artifact_digests_by_role={
                role: digest(str(index + 1))
                for index, role in enumerate(QWEN36_STAGE_ROLES)
            },
            weight_bytes_by_role={
                role: 1024 for role in QWEN36_STAGE_ROLES
            },
        )
        self.assertEqual(QWEN36_27B_PRECISION, "float16")
        self.assertEqual(adapter.descriptor.precisions, ("float16",))

    def test_offline_sealer_rejects_multimodal_or_mtp_subject_drift(self) -> None:
        exporter = load_exporter()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "stage.onnx"
            model.write_bytes(b"test-graph")
            io_manifest = root / "io.json"
            raw = {
                "inputNames": ["input_ids", "attention_mask", "position_ids",
                               "attention_kv_in",
                               "recurrent_state_in", "convolution_state_in"],
                "outputNames": ["logits", "attention_kv_out",
                                "recurrent_state_out", "convolution_state_out"],
                "stateInputNames": ["attention_kv_in", "recurrent_state_in",
                                    "convolution_state_in"],
                "stateOutputNames": ["attention_kv_out", "recurrent_state_out",
                                     "convolution_state_out"],
                "sequencePolicy": "stateful-prefill-decode-v1",
                "positionInputPolicy": "qwen-causal-position-v1",
                "attentionMaskInputName": "attention_mask",
                "positionIdsInputName": "position_ids",
                "cachePositionInputName": "",
                "graphSemanticDigest": digest("1"),
                "adapterDigest": digest("2"),
                "tokenizerDigest": digest("3"),
                "runnerDigest": digest("4"),
                "decodeMode": "single-token-autoregressive",
                "modality": "text-only",
                "mtpEnabled": False,
                "thinkingMode": "disabled",
                "chatTemplateDigest": digest("5"),
                "graphComponents": ["token-embedding", "decoder-layers", "lm-head"],
            }
            args = argparse.Namespace(
                model=str(model), io_manifest=str(io_manifest), stage="Stage0",
                layer_begin=0, layer_end=21)
            io_manifest.write_text(json.dumps(raw), encoding="utf-8")
            sealed = exporter.seal(args)
            self.assertEqual(sealed["decodeMode"], "single-token-autoregressive")
            self.assertEqual(sealed["modality"], "text-only")
            self.assertIs(sealed["mtpEnabled"], False)
            self.assertEqual(sealed["thinkingMode"], "disabled")
            self.assertEqual(sealed["chatTemplateDigest"], digest("5"))
            self.assertEqual(
                sealed["positionInputPolicy"], "qwen-causal-position-v1")

            mutations = (
                {"modality": "multimodal"},
                {"mtpEnabled": True},
                {"decodeMode": "speculative"},
                {"thinkingMode": "enabled"},
                {"chatTemplateDigest": ""},
                {"positionInputPolicy": ""},
                {"graphComponents": ["decoder-layers", "vision-projector"]},
                {"graphComponents": ["decoder-layers", "mtp-head"]},
            )
            for mutation in mutations:
                changed = {**raw, **mutation}
                io_manifest.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaises(ValueError, msg=str(mutation)):
                    exporter.seal(args)

    def test_both_state_families_are_required(self) -> None:
        value = bundle()
        with self.assertRaises(DecodeStateContractError):
            DecodeStateBundleV1(value.identity, (), value.recurrent_convolution).validate()
        with self.assertRaises(DecodeStateContractError):
            DecodeStateBundleV1(value.identity, value.full_attention_kv, ()).validate()

    def test_candidate_commits_only_after_contiguous_admission(self) -> None:
        transaction = DecodeStateTransaction(bundle())
        with self.assertRaises(DecodeStateContractError):
            transaction.apply(bundle(2), lambda _: True)
        with self.assertRaises(DecodeStateContractError):
            transaction.apply(bundle(1), lambda _: False)
        self.assertEqual(transaction.committed.token_epoch, 0)
        transaction.apply(bundle(1), lambda evidence: evidence["tokenEpoch"] == 1)
        self.assertEqual(transaction.committed.token_epoch, 1)

    def test_state_and_predecessor_epochs_are_exact_and_contiguous(self) -> None:
        identity().validate()
        identity(1).validate()

        for token_epoch, predecessor in ((0, 0), (1, None), (1, 1)):
            value = dict(identity(token_epoch).__dict__)
            value["predecessor_inference_epoch"] = predecessor
            with self.assertRaises(DecodeStateContractError):
                DecodeStateIdentityV1(**value).validate()

        transaction = DecodeStateTransaction(bundle())
        candidate = bundle(1)
        wrong = dict(candidate.identity.__dict__)
        wrong["state_inference_epoch"] = 2
        wrong["predecessor_inference_epoch"] = 0
        with self.assertRaises(DecodeStateContractError):
            transaction.apply(
                DecodeStateBundleV1(
                    DecodeStateIdentityV1(**wrong), candidate.full_attention_kv,
                    candidate.recurrent_convolution, token_epoch=1),
                lambda _: True)

    def test_request_or_attempt_mutation_is_rejected(self) -> None:
        current = bundle()
        candidate = bundle(1)
        mutated = DecodeStateIdentityV1(
            **{**candidate.identity.__dict__, "request_id": "/request/other"})
        with self.assertRaises(DecodeStateContractError):
            DecodeStateTransaction(current).apply(
                DecodeStateBundleV1(mutated, candidate.full_attention_kv,
                                    candidate.recurrent_convolution,
                                    token_epoch=1),
                lambda _: True)

    def test_prefix_position_and_cache_epoch_mutations_are_rejected(self) -> None:
        current = bundle()
        candidate = bundle(1)
        for field_name, value in {
            "prefix_digest": current.identity.prefix_digest,
            "position_digest": current.identity.position_digest,
            "cache_epoch": current.identity.cache_epoch + 1,
        }.items():
            identity_value = dict(candidate.identity.__dict__)
            identity_value[field_name] = value
            mutated = DecodeStateIdentityV1(**identity_value)
            with self.assertRaises(DecodeStateContractError, msg=field_name):
                DecodeStateTransaction(current).apply(
                    DecodeStateBundleV1(
                        mutated, candidate.full_attention_kv,
                        candidate.recurrent_convolution, token_epoch=1),
                    lambda _: True)

    def test_state_component_layout_mutation_is_rejected(self) -> None:
        current = bundle()
        candidate = bundle(1)
        changed = StateComponent(
            "attn.kv", "float16", (2, 1, 4, 16), digest("a"))
        with self.assertRaises(DecodeStateContractError) as caught:
            DecodeStateTransaction(current).apply(
                DecodeStateBundleV1(
                    candidate.identity, (changed,),
                    candidate.recurrent_convolution, token_epoch=1),
                lambda _: True)
        self.assertIn("layout", str(caught.exception))

    def test_each_identity_field_is_exactly_bound(self) -> None:
        current = bundle()
        fields = (
            "model_digest", "graph_semantic_digest", "artifact_digest",
            "adapter_digest", "tokenizer_digest", "runner_digest",
            "role_name", "role_split_digest", "layer_range", "precision",
            "layout_digest", "state_schema_digest", "state_component_digests",
            "runtime_abi_digest", "security_domain_digest", "provider_identity",
            "provider_boot_id", "request_id", "attempt_epoch", "generation_id",
        )
        for field_name in fields:
            value = dict(bundle(1).identity.__dict__)
            original = value[field_name]
            if field_name == "layer_range":
                value[field_name] = (original[0], original[1] + 1)
            elif field_name == "state_component_digests":
                value[field_name] = (digest("9"), *original[1:])
            elif isinstance(original, int):
                value[field_name] = original + 1
            elif field_name == "role_name":
                value[field_name] = "/LLM/Pipeline/Stage/9"
            elif field_name == "precision":
                value[field_name] = "float16"
            else:
                value[field_name] = digest("9") if str(original).startswith("sha256:") else str(original) + "-other"
            mutated = DecodeStateIdentityV1(**value)
            with self.assertRaises(DecodeStateContractError, msg=field_name):
                DecodeStateTransaction(current).apply(
                    DecodeStateBundleV1(mutated, bundle(1).full_attention_kv,
                                        bundle(1).recurrent_convolution,
                                        token_epoch=1),
                    lambda _: True)

    def test_real_fixture_exposes_persistent_complete_stateful_io(self) -> None:
        session = ort.InferenceSession(
            str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx"),
            providers=["CPUExecutionProvider"],
        )
        contract = StatefulOnnxIOContractV1.from_session(session)
        runner = PersistentStatefulOnnxSession(session, contract)
        assert {"attention_kv_in", "recurrent_state_in", "convolution_state_in"} <= set(contract.state_input_names)
        assert {"attention_kv_out", "recurrent_state_out", "convolution_state_out"} <= set(contract.state_output_names)

        zero_state = {
            "attention_kv_in": np.zeros((4, 8), dtype=np.float32),
            "recurrent_state_in": np.zeros((4, 8), dtype=np.float32),
            "convolution_state_in": np.zeros((4, 8), dtype=np.float32),
        }
        prefill = runner.prefill({
            "input_ids": np.asarray([[4]], dtype=np.int64), **zero_state})
        assert prefill["logits"].shape == (1, 1, 32)
        decode = runner.decode({
            "input_ids": np.asarray([[5]], dtype=np.int64),
            "attention_kv_in": prefill["attention_kv_out"],
            "recurrent_state_in": prefill["recurrent_state_out"],
            "convolution_state_in": prefill["convolution_state_out"],
        })
        assert decode["logits"].shape == (1, 1, 32)
        with self.assertRaises(DecodeStateContractError):
            runner.decode({"input_ids": np.asarray([[5]], dtype=np.int64)})

    def test_decode_requires_every_declared_position_input(self) -> None:
        contract = StatefulOnnxIOContractV1(
            input_names=("input_ids", "attention_mask", "position_ids",
                         "attention_kv_in", "recurrent_state_in",
                         "convolution_state_in"),
            output_names=("logits", "attention_kv_out", "recurrent_state_out",
                          "convolution_state_out"),
            state_input_names=("attention_kv_in", "recurrent_state_in",
                               "convolution_state_in"),
            state_output_names=("attention_kv_out", "recurrent_state_out",
                                "convolution_state_out"),
        )
        feed = {
            "input_ids": object(),
            "attention_kv_in": object(),
            "recurrent_state_in": object(),
            "convolution_state_in": object(),
        }
        with self.assertRaisesRegex(
                DecodeStateContractError,
                "attention_mask,position_ids"):
            contract.validate_feed(feed, incremental=True)

    def test_state_io_rejects_unpaired_state_output(self) -> None:
        with self.assertRaisesRegex(
                DecodeStateContractError,
                "successor input"):
            StatefulOnnxIOContractV1(
                input_names=("input_ids", "attention_kv_in",
                             "recurrent_state_in", "convolution_state_in"),
                output_names=("logits", "attention_kv_out",
                              "recurrent_state_out", "convolution_state_out",
                              "orphan_out"),
                state_input_names=("attention_kv_in", "recurrent_state_in",
                                   "convolution_state_in"),
                state_output_names=("attention_kv_out", "recurrent_state_out",
                                    "convolution_state_out", "orphan_out"),
            ).validate()

    def test_causal_position_materialization_matches_epoch_policy(self) -> None:
        contract = StatefulOnnxIOContractV1(
            input_names=("input_ids", "attention_mask", "position_ids",
                         "cache_position", "attention_kv_in",
                         "recurrent_state_in", "convolution_state_in"),
            output_names=("logits", "attention_kv_out", "recurrent_state_out",
                          "convolution_state_out"),
            state_input_names=("attention_kv_in", "recurrent_state_in",
                               "convolution_state_in"),
            state_output_names=("attention_kv_out", "recurrent_state_out",
                                "convolution_state_out"),
        )
        values = contract.materialize_causal_position_inputs(
            logical_prefix_token_count=5, new_token_count=1)
        np.testing.assert_array_equal(
            values["attention_mask"], np.ones((1, 5), dtype=np.int64))
        np.testing.assert_array_equal(
            values["position_ids"], np.asarray([[4]], dtype=np.int64))
        np.testing.assert_array_equal(
            values["cache_position"], np.asarray([4], dtype=np.int64))


if __name__ == "__main__":
    unittest.main()
