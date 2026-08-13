from __future__ import annotations

import hashlib
import importlib.util
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
                self.tokens = iter((7, 2))

            def publish_large_reference(self, *_args, **_kwargs):
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
        self.assertEqual(len(ndnsf.responses), 1)
        response = json.loads(ndnsf.responses[0])
        self.assertEqual(response["requestId"], "request-168-full")
        self.assertEqual(response["inputTokenIds"], [10, 11])
        self.assertEqual(response["generatedTokenIds"], [7, 2])
        self.assertEqual(response["wireRequestCount"], 1)
        self.assertEqual(response["tokenRequestCount"], 0)
        self.assertEqual(response["tokenCompletionClock"], "CLOCK_MONOTONIC")
        completion_times = response["tokenCompletionMonotonicMs"]
        self.assertEqual(len(completion_times), 2)
        self.assertLessEqual(completion_times[0], completion_times[1])
        self.assertEqual(
            [item["tokenIndex"] for item in response["tokenEvidence"]],
            [0, 1],
        )
        self.assertEqual(
            {item["requestId"] for item in response["tokenEvidence"]},
            {"request-168-full"},
        )
        with self.assertRaisesRegex(RuntimeError, "terminal Response"):
            context.publish_final_response(b"duplicate")

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


if __name__ == "__main__":
    unittest.main()
