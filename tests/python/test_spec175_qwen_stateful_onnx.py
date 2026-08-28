from __future__ import annotations

import argparse
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
                "contextLength": 96,
                "promptLength": 20,
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
