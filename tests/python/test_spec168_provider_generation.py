from __future__ import annotations

import hashlib
import importlib.util
import base64
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from ndnsf_distributed_inference.artifact_deployment import (
    ExecutionArtifact,
    ExecutionArtifactSpec,
    ExecutionContext,
    RuntimePreparationEvidence,
    prepare_runtime,
)
from ndnsf_distributed_inference.core import (
    DIDataDependencyV2,
    DIRequestEnvelopeV2,
    DIRoleAssignmentV2,
    DISelectionAssignmentV2,
    canonical_digest,
)
from ndnsf_distributed_inference.adapters.qwen.pilot import (
    QwenCudaRuntimePreparer,
    QwenGenerationResponse,
    QwenPilotOrchestrator,
    QwenTokenEvidence,
    publish_complete_generation,
)
from ndnsf_distributed_inference.provider import (
    DistributedInferenceProvider,
    ProviderRuntimeContext,
)
from ndnsf_distributed_inference.conversation import ConversationContinuation


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_DIR = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline")


def load_pipeline_provider():
    sys.path.insert(0, str(PIPELINE_DIR))
    try:
        spec = importlib.util.spec_from_file_location(
            "spec168_pipeline_provider", PIPELINE_DIR / "provider.py")
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.remove(str(PIPELINE_DIR))


def sha256(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


class _ResponseContext:
    session_id = "request-168"

    def __init__(self) -> None:
        self.responses = []

    def publish_final_response(self, payload: bytes) -> None:
        self.responses.append(bytes(payload))


class _NetworkProvider:
    provider = "/provider/a"
    provider_boot_epoch = "boot-epoch-a"

    def __init__(self) -> None:
        self.handler = None

    def configure_opaque_selection_store(self, **_kwargs) -> None:
        pass

    def register_opaque_selection_participant(self, *_args, **_kwargs) -> None:
        pass

    def add_collaboration_handler(
        self, _service, _roles, handler, _ack, **_kwargs,
    ) -> None:
        self.handler = handler


class _SelectionParticipant:
    PARTICIPANT_ID = "ndnsf-di-v2"
    PARTICIPANT_VERSION = 2

    def __init__(self) -> None:
        self.releases = []

    def prepare(self, *_args, **_kwargs):
        return {}

    def on_committed(self, *_args, **_kwargs) -> None:
        pass

    def on_aborted(self, *_args, **_kwargs) -> None:
        pass

    def wait_role_prepared(self, *_args, **_kwargs) -> str:
        return "txn-168"

    def mark_input_ready(self, *_args, **_kwargs) -> None:
        pass

    def mark_dependency_input_ready(self, *_args, **_kwargs) -> None:
        pass

    def mark_role_terminal(self, _payload, role, *, reason):
        self.releases.append((role, reason))
        return True


class _CollaborationContext:
    local_provider = "/provider/a"

    def __init__(
        self, assignment_payload: bytes, *, assigned_artifact: str = "",
    ) -> None:
        self.session_id = "request-168"
        self.assignment = SimpleNamespace(
            role="stage-0", service="/Inference/Generic",
            assigned_artifact=assigned_artifact,
            assignment_payload=assignment_payload,
        )
        self.statuses = []
        self.responses = []
        self.failures = []

    def report_operation_status(self, status) -> None:
        self.statuses.append(status)

    def publish_final_response(self, payload: bytes) -> None:
        self.responses.append(bytes(payload))

    def fail(self, reason: str) -> None:
        self.failures.append(str(reason))

    def fetch_artifact(self, *_args, **_kwargs):
        raise AssertionError(
            "V2 assignedArtifact is DI-owned and must not enter V1 fetch")


class Spec168ProviderGenerationTest(unittest.TestCase):
    def _streaming_generation_envelope(self, *, use_cache: bool) -> bytes:
        options = json.dumps({
            "maxNewTokens": 8,
            "eosTokenIds": [2],
            "outputMode": "TOKEN_STREAMING",
            "useCache": use_cache,
            "tokenizerDigest": "sha256:" + "7" * 64,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return DIRequestEnvelopeV2(
            invocation_id="invocation:175",
            request_id="request-175",
            attempt=1,
            service="/LLM/Qwen",
            model_name="Qwen3.6-27B",
            model_identity_hash="sha256:" + "1" * 64,
            task_kind="generation",
            input_manifest_digest="sha256:" + "2" * 64,
            input_payload_b64="",
            options_payload_b64=base64.b64encode(options).decode("ascii"),
            plan_deadline_ms=9_999_999_999_999,
            security_domain="tenant",
            task={"name": "generation", "generation_mode": "TOKEN_STREAMING"},
        ).to_bytes()

    def test_python_provider_requires_stateful_onnx_for_streaming_contract(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires useCache=true"):
            self.pipeline_provider._qwen_generation_spec(
                self._streaming_generation_envelope(use_cache=False))
        spec = self.pipeline_provider._qwen_generation_spec(
            self._streaming_generation_envelope(use_cache=True))
        self.assertEqual(spec["generation_mode"], "TOKEN_STREAMING")
        self.assertTrue(spec["use_cache"])
        with self.assertRaisesRegex(
                RuntimeError, "device-resident stateful ONNX Provider runtime"):
            self.pipeline_provider._require_python_qwen_generation_mode(spec)
        self.pipeline_provider._require_python_qwen_generation_mode(
            spec, stateful_onnx=True)

    def test_final_response_mode_matches_delivery_mode(self) -> None:
        self.assertEqual(
            self.pipeline_provider._qwen_response_generation_mode(True),
            "TOKEN_STREAMING")
        self.assertEqual(
            self.pipeline_provider._qwen_response_generation_mode(False),
            "FULL")

    def test_python_provider_validates_conversation_contract_before_runner(self) -> None:
        payload = b"conversation-turn"
        input_digest = sha256(payload)
        continuation = ConversationContinuation(
            "c" * 32,
            turn_input_digest=input_digest,
        )
        continuation = ConversationContinuation(
            **{
                **continuation.__dict__,
                "request_contract_digest": continuation.request_contract(
                    input_digest=input_digest),
            })
        options = json.dumps({
            "maxNewTokens": 8,
            "eosTokenIds": [2],
            "outputMode": "TOKEN_STREAMING",
            "useCache": True,
            "tokenizerDigest": "sha256:" + "7" * 64,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")
        envelope = DIRequestEnvelopeV2(
            invocation_id="invocation:conversation",
            request_id="request-conversation",
            attempt=1,
            service="/LLM/Qwen",
            model_name="Qwen3.6-27B",
            model_identity_hash="sha256:" + "1" * 64,
            task_kind="generation",
            input_manifest_digest="sha256:" + "2" * 64,
            input_payload_b64=base64.b64encode(payload).decode("ascii"),
            options_payload_b64=base64.b64encode(options).decode("ascii"),
            plan_deadline_ms=9_999_999_999_999,
            security_domain="tenant",
            task={
                "name": "generation",
                "generation_mode": "TOKEN_STREAMING",
                "conversation": continuation.to_dict(),
            },
        )
        spec = self.pipeline_provider._qwen_generation_spec(envelope.to_bytes())
        self.assertEqual(spec["conversation"]["conversationId"], "c" * 32)

        changed = continuation.to_dict()
        changed["turnInputDigest"] = sha256(b"tampered")
        invalid = DIRequestEnvelopeV2(
            **{
                **envelope.__dict__,
                "task": {
                    **envelope.task,
                    "conversation": changed,
                },
            })
        with self.assertRaisesRegex(ValueError, "conversation continuation"):
            self.pipeline_provider._qwen_generation_spec(invalid.to_bytes())

    def test_qwen_residency_template_requires_complete_partition_identity(self):
        digest = sha256(b"identity")
        template = {
            "artifact_digest": digest,
            "model_content_digest": sha256(b"model"),
            "graph_digest": sha256(b"graph"),
            "partition_digest": sha256(b"partition"),
            "adapter_id": "qwen-transformers",
            "adapter_version": "1",
            "backend": "transformers",
        }
        identity = self.pipeline_provider._qwen_residency_identity(
            template,
            artifact_digest=digest,
            adapter_id="qwen-transformers",
            adapter_version="1",
            backend="transformers",
            device="cuda:0",
            provider_boot_epoch="boot-168",
        )
        self.assertEqual(template["partition_digest"], identity.partition_digest)

        incomplete = dict(template)
        incomplete.pop("partition_digest")
        with self.assertRaisesRegex(
                ValueError, "partition_digest must be a canonical sha256 digest"):
            self.pipeline_provider._qwen_residency_identity(
                incomplete,
                artifact_digest=digest,
                adapter_id="qwen-transformers",
                adapter_version="1",
                backend="transformers",
                device="cuda:0",
                provider_boot_epoch="boot-168",
            )

    def test_cpu_logic_preparation_is_explicit_and_never_cuda_evidence(self):
        evidence = RuntimePreparationEvidence(
            adapter_id="qwen-transformers", adapter_version="1",
            backend="transformers-cpu", device="cpu",
            artifact_digests=("sha256:" + "a" * 64,),
            load_completed=True, warmup_completed=True,
            cpu_fallback_count=0, prepared_at_ms=10_000,
            device_class="CPU_LOGIC",
        )
        self.assertEqual(evidence.device_class, "CPU_LOGIC")
        with self.assertRaisesRegex(ValueError, "CPU logic"):
            RuntimePreparationEvidence(
                adapter_id="qwen-transformers", adapter_version="1",
                backend="transformers", device="cpu",
                artifact_digests=("sha256:" + "a" * 64,),
                load_completed=True, warmup_completed=True,
                cpu_fallback_count=0, prepared_at_ms=10_000,
                device_class="CPU_LOGIC",
            )

    @classmethod
    def setUpClass(cls) -> None:
        cls.pipeline_provider = load_pipeline_provider()

    def test_qwen_onnx_full_generation_reuses_one_provider_owned_state(self) -> None:
        class Session:
            @staticmethod
            def get_providers():
                return ["CUDAExecutionProvider", "CPUExecutionProvider"]

        metadata = {
            "stageIndex": 0,
            "stageCount": 3,
            "sequencePolicy": "stateful-prefill-decode-v1",
            "stateInputNames": [
                "attention_kv_in", "recurrent_state_in",
                "convolution_state_in"],
            "stateOutputNames": [
                "attention_kv_out", "recurrent_state_out",
                "convolution_state_out"],
        }
        request = self.pipeline_provider.encode_qwen_pipeline_context(
            [[10, 11]], request_id="state-owned-by-provider",
            session_id="generation-state-owned-by-provider",
            generation={
                "maxNewTokens": 2,
                "eosTokenIds": [2],
                "outputMode": "FULL",
                "useCache": True,
            },
        )
        context = SimpleNamespace(
            execution=SimpleNamespace(
                artifact_paths={"model": "/model/stage-0.onnx"}),
            role="/LLM/Pipeline/Stage/0",
            request=request,
            request_id="state-owned-by-provider",
        )
        observed_states = []

        def fake_stage_runner(_payload, **kwargs):
            observed_states.append(kwargs.get("state"))
            kwargs["state"]["epoch"] = len(observed_states)
            return b"stage-output"

        def fake_full_generation(_ctx, **kwargs):
            kwargs["stage_runner"](b"prefill", 0.0)
            kwargs["stage_runner"](b"decode", 0.0)
            return {}

        with mock.patch.object(
                self.pipeline_provider, "run_qwen_onnx_stage",
                side_effect=fake_stage_runner), mock.patch.object(
                    self.pipeline_provider,
                    "_handle_qwen_transformer_full_generation",
                    side_effect=fake_full_generation):
            self.pipeline_provider.handle_qwen_onnx_stage(
                context,
                stages=3,
                session_cache={"/model/stage-0.onnx": Session()},
                metadata_cache={"/model/stage-0.onnx": metadata},
                compute_delay_ms=0.0,
                device="cuda:0",
                require_cuda=True,
            )
        self.assertEqual(len(observed_states), 2)
        self.assertIsNotNone(observed_states[0])
        self.assertIs(observed_states[0], observed_states[1])

    def test_qwen_conversation_state_moves_device_host_device(self) -> None:
        class DeviceOrtValue:
            def __init__(self, value):
                self.value = value
                self.numpy_calls = 0

            @staticmethod
            def device_name():
                return "cuda"

            def numpy(self):
                self.numpy_calls += 1
                return self.value

        import numpy as np

        device_value = DeviceOrtValue(np.asarray([[1.0, 2.0]], dtype=np.float32))
        state = {
            "attention_kv_in": device_value,
            "__ndnsf_prefix_token_count__": 2,
        }
        host = self.pipeline_provider._qwen_state_to_host(state)
        self.assertEqual(device_value.numpy_calls, 1)
        self.assertIsInstance(host["attention_kv_in"], np.ndarray)
        self.assertIsNot(host["attention_kv_in"], device_value.value)
        self.assertEqual(host["__ndnsf_prefix_token_count__"], 2)

        gpu_value = object()
        factory = mock.Mock(return_value=gpu_value)
        restored = self.pipeline_provider._qwen_state_to_gpu(
            host, device_id=0, ortvalue_factory=factory)
        factory.assert_called_once()
        self.assertIs(restored["attention_kv_in"], gpu_value)
        self.assertEqual(restored["__ndnsf_prefix_token_count__"], 2)

    def test_copy_onnx_state_keeps_opaque_device_buffers_but_copies_owner(self) -> None:
        class DeviceOrtValue:
            @staticmethod
            def device_name():
                return "cuda"

        value = DeviceOrtValue()
        source = {"attention_kv_in": value, "nested": [1, 2]}
        copied = self.pipeline_provider._copy_onnx_state(source)
        self.assertIsNot(copied, source)
        self.assertIs(copied["attention_kv_in"], value)
        self.assertIsNot(copied["nested"], source["nested"])

    def test_qwen_onnx_generation_uses_conversation_manager_and_host_tier(self) -> None:
        class Session:
            @staticmethod
            def get_providers():
                return ["CUDAExecutionProvider", "CPUExecutionProvider"]

            @staticmethod
            def io_binding():
                return object()

            @staticmethod
            def run_with_iobinding(_binding):
                return None

        metadata = {
            "stageIndex": 0,
            "stageCount": 3,
            "sequencePolicy": "stateful-prefill-decode-v1",
            "stateInputNames": [
                "attention_kv_in", "recurrent_state_in",
                "convolution_state_in"],
            "stateOutputNames": [
                "attention_kv_out", "recurrent_state_out",
                "convolution_state_out"],
        }
        request = self.pipeline_provider.encode_qwen_pipeline_context(
            [[10, 11]], request_id="conversation-child",
            session_id="conversation-provider-state",
            generation={
                "maxNewTokens": 1, "eosTokenIds": [2],
                "outputMode": "FULL", "useCache": True,
            })
        context = SimpleNamespace(
            execution=SimpleNamespace(
                artifact_paths={"model": "/model/stage-0.onnx"},
                spec=SimpleNamespace(metadata={"generationId": "g-child"})),
            role="/LLM/Pipeline/Stage/0", request=request,
            request_id="conversation-child",
            stream_cancelled=lambda: False,
        )
        state_cache = {}
        parent_state = {"attention_kv_in": object()}
        binding = SimpleNamespace(conversation_id="conversation-provider-state")
        continuation = SimpleNamespace(mode=SimpleNamespace(value="append-delta"))
        parent_receipt = object()
        final_receipt = SimpleNamespace(
            conversation_id="conversation-provider-state",
            successor_context_epoch=2)
        manager = mock.Mock()
        manager.pause_to_host.return_value = SimpleNamespace(
            logical_bytes=64, transfer_bytes=64, transfer_latency_ms=1)

        def fake_prepare(_ctx, **kwargs):
            kwargs["state_cache"][("conversation-child", 0)] = parent_state.copy()
            return binding, None, continuation, parent_receipt

        observed = []

        def fake_runner(_payload, **kwargs):
            observed.append(kwargs["state"])
            return b"output"

        def fake_generation(_ctx, **kwargs):
            kwargs["stage_runner"](b"prefill", 0.0)
            kwargs["stage_runner"](b"decode", 0.0)
            return {"input_token_ids": (10, 11), "generated_token_ids": (12,)}

        with mock.patch.object(
                self.pipeline_provider, "_conversation_prepare",
                side_effect=fake_prepare) as prepare, mock.patch.object(
                    self.pipeline_provider, "run_qwen_onnx_stage",
                    side_effect=fake_runner), mock.patch.object(
                        self.pipeline_provider,
                        "_handle_qwen_transformer_full_generation",
                        side_effect=fake_generation), mock.patch.object(
                            self.pipeline_provider, "_conversation_finalize",
                            return_value=final_receipt) as finalize:
            self.pipeline_provider.handle_qwen_onnx_stage(
                context, stages=3,
                session_cache={"/model/stage-0.onnx": Session()},
                metadata_cache={"/model/stage-0.onnx": metadata},
                compute_delay_ms=0.0, device="cuda:0", require_cuda=True,
                state_cache=state_cache, conversation_manager=manager,
                conversation_receipts={}, conversation_prefixes={},
                spec175_host_tier_after_commit=True)

        self.assertEqual(len(observed), 2)
        self.assertIs(observed[0], observed[1])
        self.assertEqual(observed[0]["attention_kv_in"],
                         parent_state["attention_kv_in"])
        self.assertTrue(prepare.called)
        self.assertTrue(finalize.called)
        manager.pause_to_host.assert_called_once()
        self.assertIs(
            manager.pause_to_host.call_args.kwargs["copy_state"],
            self.pipeline_provider._qwen_state_to_host)
        manager.release_for_request.assert_called_once()
        self.assertNotIn(("conversation-child", 0), state_cache)

    def _execution(self, root: Path) -> tuple[ExecutionContext, str]:
        payload = b"immutable-qwen-stage"
        digest = sha256(payload)
        path = root / "stage.safetensors"
        path.write_bytes(payload)
        return ExecutionContext(
            spec=ExecutionArtifactSpec(
                role="stage-0", backend="transformers-cuda",
                entrypoint="",
                artifacts=[ExecutionArtifact(
                    name="model", data_name="/repo/model/stage-0",
                    filename=path.name, sha256=digest[7:],
                    kind="model", chunks=[], executable=False,
                    cache_name="",
                )],
                metadata={},
            ),
            artifact_paths={"model": path},
            work_dir=root,
        ), digest

    def test_qwen_adapter_loads_and_warms_exact_cuda_assignment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            execution, digest = self._execution(Path(tmp))
            events = []
            handle = object()
            preparer = QwenCudaRuntimePreparer(
                adapter_id="qwen-transformers",
                adapter_version="1",
                backend="transformers-cuda",
                device="cuda:0",
                load_shard=lambda value, device: (
                    events.append(("load", value.path("model"), device)),
                    handle,
                )[1],
                warmup=lambda value: events.append(("warmup", value)) or True,
                runtime_probe=lambda value: {
                    "backend": "transformers-cuda",
                    "device": "cuda:0",
                    "cpuFallbackCount": 0,
                    "handleMatches": value is handle,
                },
                clock_ms=lambda: 10_000,
            )
            progress = []
            with mock.patch.object(
                    Path, "read_bytes",
                    side_effect=AssertionError(
                        "runtime verification must stream large artifacts")):
                prepared = prepare_runtime(
                    execution,
                    preparer=preparer,
                    expected_adapter_id="qwen-transformers",
                    expected_adapter_version="1",
                    expected_backend="transformers-cuda",
                    expected_device="cuda:0",
                    expected_artifact_digest=digest,
                    progress=lambda phase, value: progress.append((phase, value)),
                )
            self.assertEqual([item[0] for item in events], ["load", "warmup"])
            self.assertEqual([item[0] for item in progress], ["LOADING", "WARMING"])
            self.assertTrue(prepared.runtime_evidence.load_completed)
            self.assertTrue(prepared.runtime_evidence.warmup_completed)
            self.assertEqual(prepared.runtime_evidence.device, "cuda:0")
            self.assertEqual(prepared.runtime_evidence.cpu_fallback_count, 0)

    def test_runtime_evidence_rejects_cpu_or_unwarmed_success(self) -> None:
        common = dict(
            adapter_id="qwen-transformers", adapter_version="1",
            backend="transformers-cuda",
            artifact_digests=("sha256:" + "a" * 64,),
            load_completed=True, warmup_completed=True,
            cpu_fallback_count=0, prepared_at_ms=10_000,
        )
        with self.assertRaisesRegex(ValueError, "CUDA"):
            RuntimePreparationEvidence(device="cpu", **common)
        with self.assertRaisesRegex(ValueError, "load and warmup"):
            RuntimePreparationEvidence(
                device="cuda:0", **{**common, "warmup_completed": False})
        with self.assertRaisesRegex(ValueError, "CPU fallback"):
            RuntimePreparationEvidence(
                device="cuda:0", **{**common, "cpu_fallback_count": 1})

    def test_one_prompt_uses_one_internal_ordered_token_loop(self) -> None:
        tokenizations = []
        contexts = []
        tokens = iter([7, 8, 2])

        def tokenizer(prompt: str):
            tokenizations.append(prompt)
            return [10, 11]

        def staged_logits(context):
            contexts.append(tuple(context))
            token = next(tokens)
            logits = [0.0] * 12
            logits[token] = 1.0
            return logits

        orchestrator = QwenPilotOrchestrator(tokenizer, staged_logits)
        request = orchestrator.request("Explain NDN.", 64)
        result = orchestrator.generate_complete(
            request, request_id="request-168", eos_token_ids={2},
            decode=lambda values: "answer:" + ",".join(map(str, values)),
        )
        self.assertEqual(tokenizations, ["Explain NDN."])
        self.assertEqual(contexts, [(10, 11), (10, 11, 7), (10, 11, 7, 8)])
        self.assertEqual(result.generated_token_ids, (7, 8, 2))
        self.assertEqual(result.decoded_text, "answer:7,8,2")
        self.assertEqual(result.stop_reason, "EOS")
        self.assertEqual(
            [item.token_index for item in result.token_evidence], [0, 1, 2])
        self.assertEqual(
            {item.request_id for item in result.token_evidence}, {"request-168"})
        self.assertEqual(
            QwenGenerationResponse.from_bytes(result.to_bytes()), result)

    def test_complete_generation_publishes_one_terminal_response(self) -> None:
        ndnsf = _ResponseContext()
        context = ProviderRuntimeContext(
            ndnsf=ndnsf, execution=object(), request=b"prompt",
            role="stage-2",
        )
        response = QwenGenerationResponse(
            request_id="request-168", input_token_ids=(10, 11),
            generated_token_ids=(7, 2), decoded_text="complete answer",
            stop_reason="EOS", token_evidence=(
                QwenTokenEvidence("request-168", 0, 2, 7),
                QwenTokenEvidence("request-168", 1, 3, 2),
            ),
        )
        publish_complete_generation(context, response)
        self.assertEqual(len(ndnsf.responses), 1)
        self.assertEqual(
            QwenGenerationResponse.from_bytes(ndnsf.responses[0]), response)
        with self.assertRaisesRegex(RuntimeError, "terminal Response"):
            publish_complete_generation(context, response)
        self.assertEqual(len(ndnsf.responses), 1)

    def _selection_assignment(self) -> DISelectionAssignmentV2:
        digest = "sha256:" + "a" * 64
        dependency = DIDataDependencyV2(
            producers=("stage-0",), consumers=("stage-1",),
            key_scope="tensor-dynamic-selection-scope",
            topic_prefix="/activation", tensors=("hidden-state",),
        )
        role = DIRoleAssignmentV2(
            role="stage-0", graph_node_id="layer-0", layer_start=0,
            layer_end=1, artifact_digest=digest,
            dependency_digest=canonical_digest((dependency,)),
            adapter_id="qwen-transformers", adapter_version="1",
            dependencies=(dependency,),
            required_gpu_mib=1024,
            input_grant_digests=("sha256:" + "c" * 64,),
            backend="transformers-cuda", device="cuda:0",
        )
        return DISelectionAssignmentV2(
            invocation_id="invocation-168", request_id="request-168",
            attempt=1, plan_digest="sha256:" + "d" * 64,
            provider="/provider/a", provider_boot_epoch="boot-epoch-a",
            offer_digest="sha256:" + "e" * 64, resource_sequence=1,
            roles=(role,), artifact_set_digest="sha256:" + "f" * 64,
            dependency_graph_digest="sha256:" + "1" * 64,
            deadline_ms=9_000_000_000_000, generation=1,
        )

    def _registered_provider(self, preparer, handler):
        network = _NetworkProvider()
        participant = _SelectionParticipant()
        provider = DistributedInferenceProvider(network)
        provider.add_capability_handler(
            "/Inference/Generic", ["stage-0"], handler,
            backends=("transformers-cuda",), has_model=True,
            local_artifacts={
                "stage-0": {"path": "/verified/stage-0.bin",
                            "backend": "transformers-cuda"}},
            selection_offer_issuer=object(),
            selection_participant=participant,
            selection_wal_path="/tmp/spec168-selection.wal",
            selection_storage_key=b"spec168-test-key",
            selection_storage_key_epoch="epoch-168",
            runtime_preparer=preparer,
        )
        return network, participant

    def test_v2_provider_reaches_ready_only_after_adapter_cuda_proof(self) -> None:
        assignment = self._selection_assignment()
        observed = []

        def preparer(_execution, progress):
            progress("LOADING", 0.70)
            progress("WARMING", 0.90)
            return RuntimePreparationEvidence(
                adapter_id="qwen-transformers", adapter_version="1",
                backend="transformers-cuda", device="cuda:0",
                artifact_digests=(assignment.roles[0].artifact_digest,),
                load_completed=True, warmup_completed=True,
                cpu_fallback_count=0, prepared_at_ms=10_000,
            )

        network, participant = self._registered_provider(
            preparer,
            lambda ctx: (
                observed.append((
                    ctx.execution.runtime_evidence,
                    ctx.dependencies.output().key_scope,
                )),
                ctx.publish_final_response(b"complete-answer"),
            ),
        )
        context = _CollaborationContext(assignment.to_bytes())
        network.handler(context, b"one-prompt")
        phases = [
            __import__("json").loads(item.details_payload)["phase"]
            for item in context.statuses
        ]
        self.assertEqual(phases, ["ACCEPTED", "LOADING", "WARMING", "READY"])
        self.assertEqual(observed[0][0].device, "cuda:0")
        self.assertEqual(
            observed[0][1], "tensor-dynamic-selection-scope")
        self.assertEqual(context.responses, [b"complete-answer"])
        self.assertFalse(context.failures)
        self.assertEqual(
            participant.releases,
            [("stage-0", "RESPONSE_PUBLISHED")],
        )

    def test_v2_repo_artifact_identity_never_enters_v1_spec_fetch(self) -> None:
        assignment = self._selection_assignment()

        def preparer(_execution, progress):
            progress("LOADING", 0.75)
            progress("WARMING", 0.90)
            return RuntimePreparationEvidence(
                adapter_id="qwen-transformers", adapter_version="1",
                backend="transformers-cuda", device="cuda:0",
                artifact_digests=(assignment.roles[0].artifact_digest,),
                load_completed=True, warmup_completed=True,
                cpu_fallback_count=0, prepared_at_ms=10_000,
            )

        network, _participant = self._registered_provider(
            preparer, lambda ctx: ctx.publish_final_response(b"complete"))
        context = _CollaborationContext(
            assignment.to_bytes(),
            assigned_artifact="/repo/NDNSF-ARTIFACT/sha256/deadbeef",
        )
        network.handler(context, b"one-prompt")
        self.assertEqual(context.responses, [b"complete"])
        self.assertFalse(context.failures)

    def test_v2_provider_never_reports_ready_when_cuda_proof_fails(self) -> None:
        assignment = self._selection_assignment()
        handled = []

        def failing_preparer(_execution, progress):
            progress("LOADING", 0.70)
            raise RuntimeError("CUDA warmup failed")

        network, participant = self._registered_provider(
            failing_preparer, lambda _ctx: handled.append(True))
        context = _CollaborationContext(assignment.to_bytes())
        network.handler(context, b"one-prompt")
        phases = [
            __import__("json").loads(item.details_payload)["phase"]
            for item in context.statuses
        ]
        self.assertEqual(phases, ["ACCEPTED", "LOADING", "FAILED"])
        self.assertNotIn("READY", phases)
        self.assertEqual(handled, [])
        self.assertEqual(len(context.failures), 1)
        self.assertEqual(
            participant.releases,
            [("stage-0", "PREPARATION_FAILED")],
        )

    def test_real_full_generation_path_uses_one_wire_request_and_one_response(
        self,
    ) -> None:
        class Edge:
            key_scope = "tensor-0"

            @staticmethod
            def topic(suffix=""):
                return "/activation/" + str(suffix)

        edge = Edge()

        class Dependencies:
            inputs = ()
            outputs = (edge,)

            @staticmethod
            def output(_scope=""):
                return edge

        class Ndnsf:
            session_id = "request-168-full"

            def __init__(self):
                self.responses = []
                self.published = []
                self.tokens = iter((7, 2))

            def publish_large_reference(self, *args, **_kwargs):
                self.published.append(bytes(args[3]))
                return "/dependency/hidden"

            def wait_one(self, *_args, **_kwargs):
                token = next(self.tokens)
                epoch = 0 if token == 7 else 1
                return SimpleNamespace(payload=json.dumps({
                    "schema": "ndnsf-di-qwen-generation-control-v1",
                    "kind": "TOKEN", "epoch": epoch, "token": token,
                }, sort_keys=True, separators=(",", ":")).encode())

            def publish(self, *_args, **_kwargs):
                pass

            def publish_final_response(self, payload):
                self.responses.append(bytes(payload))

        ndnsf = Ndnsf()
        request = self.pipeline_provider.encode_qwen_pipeline_context(
            [[10, 11]], request_id="request-168-full",
            session_id="invocation-168", context_epoch=0,
            generation={
                "maxNewTokens": 3, "eosTokenIds": [2],
                "outputMode": "FULL",
            },
        )
        context = ProviderRuntimeContext(
            ndnsf=ndnsf, execution=object(), request=request,
            role="stage-0", dependencies=Dependencies(),
            deadline_ms=9_000_000_000_000,
        )
        self.pipeline_provider._handle_qwen_transformer_full_generation(
            context, model=object(), stages=3, stage_index=0,
            compute_delay_ms=0.0,
            spec={
                "max_new_tokens": 3, "eos_token_ids": (2,),
                "session_id": "invocation-168",
            },
            stage_runner=lambda payload, _delay: b"hidden:" + payload[:8],
        )
        self.assertEqual(ndnsf.responses, [])
        stop = json.loads(ndnsf.published[-1])
        self.assertEqual(stop["kind"], "STOP")
        self.assertEqual(stop["inputTokenIds"], [10, 11])
        self.assertEqual(stop["generatedTokenIds"], [7, 2])
        self.assertEqual(stop["stopReason"], "EOS")
        completion_times = stop["tokenCompletionMonotonicMs"]
        self.assertEqual(len(completion_times), 2)
        self.assertLessEqual(completion_times[0], completion_times[1])

    def test_full_generation_terminal_role_owns_events_and_response(self) -> None:
        from concurrent.futures import Future

        class Edge:
            key_scope = "tensor-0"
            producer_role = "stage-1"

            @staticmethod
            def topic(suffix=""):
                return "/activation/" + str(suffix)

        edge = Edge()

        class Dependencies:
            inputs = (edge,)
            outputs = ()

            @staticmethod
            def input(_scope=""):
                return edge

        stop = self.pipeline_provider._generation_step_control(
            "STOP", 2,
            inputTokenIds=[10, 11],
            generatedTokenIds=[7, 2],
            stopReason="EOS",
            tokenCompletionMonotonicMs=[1.0, 2.0],
        )

        class Prefetcher:
            def __init__(self):
                self.values = iter((b"hidden-0", b"hidden-1", stop))

            def prefetch_large(self, *_args, **_kwargs):
                future = Future()
                future.set_result(SimpleNamespace(payload=next(self.values)))
                return future

        class Ndnsf:
            session_id = "request-168-recovery"

            def __init__(self):
                self.feedback = []

            def publish(self, _scope, _topic, payload):
                self.feedback.append(bytes(payload))

        class Writer:
            def __init__(self):
                self.events = []
                self.completions = []

            def publish_event(self, payload, **_kwargs):
                self.events.append(bytes(payload))
                return len(self.events)

            def finish_stream(self, payload, **kwargs):
                self.completions.append((bytes(payload), kwargs))
                return True

        ndnsf = Ndnsf()
        writer = Writer()
        context = ProviderRuntimeContext(
            ndnsf=ndnsf, execution=object(), request=b"request-envelope",
            role="stage-2", dependencies=Dependencies(),
            prefetcher=Prefetcher(), stream_writer=writer,
            deadline_ms=9_000_000_000_000,
        )
        tokens = iter((7, 2))
        self.pipeline_provider._handle_qwen_transformer_full_generation(
            context, model=object(), stages=3, stage_index=2,
            compute_delay_ms=0.0,
            spec={
                "max_new_tokens": 3, "eos_token_ids": (2,),
                "session_id": "invocation-168",
                "committed_prefix_token_ids": (7,),
            },
            stage_runner=lambda _payload, _delay: json.dumps({
                "topToken": next(tokens),
            }).encode(),
        )

        self.assertEqual(len(writer.events), 1)
        event = json.loads(writer.events[0])
        self.assertEqual(event["tokenEpoch"], 2)
        self.assertEqual(event["tokenId"], 2)
        self.assertEqual(len(writer.completions), 1)
        response = json.loads(writer.completions[0][0])
        self.assertEqual(response["requestId"], "request-168-recovery")
        self.assertEqual(response["tokenIds"], [7, 2])
        self.assertEqual(response["generatedTokenIds"], [7, 2])
        self.assertEqual(response["stopReason"], "EOS")
        self.assertEqual(len(ndnsf.feedback), 2)

    def test_full_generation_middle_role_forwards_complete_stop_transcript(self) -> None:
        """A middle role must not strip terminal STOP evidence while forwarding."""
        from concurrent.futures import Future

        class Edge:
            key_scope = "tensor-0"
            producer_role = "stage-0"

            @staticmethod
            def topic(suffix=""):
                return "/activation/" + str(suffix)

        input_edge = Edge()
        output_edge = SimpleNamespace(
            key_scope="tensor-1",
            producer_role="stage-1",
            topic=lambda suffix="": "/activation/" + str(suffix),
        )

        class Dependencies:
            inputs = (input_edge,)
            outputs = (output_edge,)

            @staticmethod
            def input(_scope=""):
                return input_edge

            @staticmethod
            def output(_scope=""):
                return output_edge

        stop = self.pipeline_provider._generation_step_control(
            "STOP", 1,
            inputTokenIds=[10, 11],
            generatedTokenIds=[7, 2],
            stopReason="EOS",
            tokenCompletionMonotonicMs=[1.0, 2.0],
        )

        class Prefetcher:
            def __init__(self):
                self.values = iter((b"hidden-0", stop))

            def prefetch_input_large(self, *_args, **_kwargs):
                future = Future()
                future.set_result(SimpleNamespace(payload=next(self.values)))
                return future

        class Ndnsf:
            def __init__(self):
                self.feedback = []
                self.tokens = iter((7,))

            def wait_one(self, *_args, **_kwargs):
                return SimpleNamespace(payload=self.pipeline_provider._generation_step_control(
                    "TOKEN", 0, token=next(self.tokens)))

            def publish(self, _scope, _topic, payload):
                self.feedback.append(bytes(payload))

        class Context:
            request_id = "request-168-middle"
            role = "stage-1"
            stream_writer = None

            def __init__(self):
                self.dependencies = Dependencies()
                self.prefetcher = Prefetcher()
                self.ndnsf = Ndnsf()
                self.published = []

            def dependency_timeout_ms(self, fallback_ms=30000):
                return fallback_ms

            def prefetch_input_large(self, **kwargs):
                return self.prefetcher.prefetch_input_large(**kwargs)

            def wait_prefetched_input_large(self, future, **_kwargs):
                return future.result().payload

            def publish_output_large_reference(self, payload, **_kwargs):
                self.published.append(bytes(payload))
                return "/dependency/output"

        # Use a closure instead of importing the module as a global in the
        # nested fake classes; the test remains independent of host imports.
        context = Context()
        context.ndnsf.pipeline_provider = self.pipeline_provider
        self.pipeline_provider._handle_qwen_transformer_full_generation(
            context,
            model=object(),
            stages=3,
            stage_index=1,
            compute_delay_ms=0.0,
            spec={
                "max_new_tokens": 3,
                "eos_token_ids": (2,),
                "session_id": "invocation-168-middle",
            },
            stage_runner=lambda _payload, _delay: b"hidden-1",
        )

        self.assertEqual(json.loads(context.published[-1]), json.loads(stop))
        self.assertEqual(
            json.loads(context.published[-1])["generatedTokenIds"], [7, 2])

    def test_causal_provider_markers_carry_monotonic_timestamps(self) -> None:
        source = (PIPELINE_DIR / "provider.py").read_text(encoding="utf-8")
        for marker in (
            "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED",
            "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED",
            "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED",
        ):
            offset = source.index(f'"{marker}"')
            excerpt = source[offset:offset + 500]
            self.assertIn("monotonicMs=", excerpt, marker)
        self.assertIn("LLM_PIPELINE_QWEN_EPOCH_DATAFLOW_TIMING", source)
        self.assertIn("activationFetchMs=", source)
        self.assertIn("feedbackPublishMs=", source)
        self.assertIn("eventPublishMs=", source)
        self.assertIn("LLM_PIPELINE_QWEN_REQUEST_STATE_CLEANUP", source)


if __name__ == "__main__":
    unittest.main()
