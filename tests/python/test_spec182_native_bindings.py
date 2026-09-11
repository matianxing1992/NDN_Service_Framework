from __future__ import annotations

from pathlib import Path
import hashlib
import json
import unittest
import sys
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
BINDINGS = ROOT / "pythonWrapper/src/ndnsf/di_bindings.cpp"
MODULE = ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp"
SETUP = ROOT / "pythonWrapper/setup.py"
REQUESTER = ROOT / "examples/DI_NativeRequester.cpp"


class Spec182NativeBindingsTest(unittest.TestCase):
    def test_binding_is_a_single_native_translation_unit_entry(self):
        source = BINDINGS.read_text(encoding="utf-8")
        module = MODULE.read_text(encoding="utf-8")
        setup = SETUP.read_text(encoding="utf-8")
        self.assertIn("bindDistributedInference(py::module_& module)", source)
        self.assertIn("bindDistributedInference(m);", module)
        self.assertIn('"src/ndnsf/di_bindings.cpp"', setup)
        self.assertIn('"ndnsf-distributed-inference"', setup)
        self.assertNotIn("NativeGrantVerifier.cpp", setup)
        # A Python callback is exposed only as a terminal observer on an
        # already-created native handle; planning and execution remain native.
        self.assertIn("py::function observer", source)
        self.assertIn('.def("observe"', source)
        self.assertNotIn("subprocess", source)

    def test_binding_source_has_no_python_strategy_trampoline(self):
        source = BINDINGS.read_text(encoding="utf-8")
        self.assertNotIn("subprocess", source)
        self.assertNotIn("py::eval", source)

    def test_native_dto_names_are_explicitly_mapped(self):
        source = BINDINGS.read_text(encoding="utf-8")
        for field in (
                "model_name", "content_digest", "adapter_id", "payload",
                "transport_mode", "timeout_ms", "ack_timeout_ms",
                "application_request_id"):
            self.assertIn(f'"{field}"', source)

    def test_native_generation_stream_and_conversation_options_are_typed(self):
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf

        generation = _ndnsf.NativeGenerationExecutionContractV1()
        generation.enabled = True
        generation.mode = "TOKEN_DIAGNOSTIC"
        generation.max_generated_tokens = 32
        generation.token_input_name = "input_ids"
        generation.state_input_names = ["past_key_values.0"]
        generation.state_output_names = ["present.0"]
        generation.eos_token_ids = [2]
        generation.sampling_mode = "Greedy"
        generation.sampling_top_k = 1
        generation.sampling_seed = 1750001
        generation.generation_id = "generation-1"
        generation.committed_prefix_token_ids = [4, 5]

        continuation = _ndnsf.NativeConversationContinuation()
        continuation.conversation_id = "conversation-1"
        continuation.parent_context_epoch = 7
        continuation.service_name = "qwen"
        continuation.mode = "APPEND_DELTA"
        continuation.canonical_token_ids = [4, 5, 6]
        continuation.expected_roles = ["/LLM/Pipeline/Stage/0"]

        stream = _ndnsf.NativeStreamRequestOptions()
        stream.mode = _ndnsf.NativeInvocationMode.NORMAL
        stream.generation_id = [1] * 16
        stream.attempt_epoch = 1
        stream.stream_epoch = 1
        stream.event_key_commitment = [2] * 32
        stream.deadline_epoch_ms = 4102444800000
        stream.controller_version = _ndnsf.NativeControllerVersion()
        stream.controller_version.controller_generation_timestamp = 1
        stream.controller_version.controller_epoch = 1
        stream.validate()
        encoded = stream.wire_encode()
        decoded = _ndnsf.NativeStreamRequestOptions()
        self.assertTrue(decoded.wire_decode(encoded))
        self.assertEqual(decoded.generation_id, stream.generation_id)
        self.assertEqual(decoded.event_key_commitment, stream.event_key_commitment)

        options = _ndnsf.NativeRequestOptions()
        options.application_request_id = "wire-request"
        options.generation = generation
        options.stream = stream
        options.conversation = continuation
        self.assertEqual(options.generation.generation_id, "generation-1")
        self.assertEqual(options.stream.stream_epoch, 1)
        self.assertEqual(options.conversation.mode, "APPEND_DELTA")
        self.assertEqual(options.application_request_id, "wire-request")

    def test_native_stream_key_grant_uses_wire_bytes_and_none(self):
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf

        stream = _ndnsf.NativeStreamRequestOptions()
        self.assertIsNone(stream.event_key_grant_wire)
        stream.event_key_grant_wire = b"\x80\x00"
        self.assertEqual(stream.event_key_grant_wire, b"\x80\x00")
        stream.event_key_grant_wire = None
        self.assertIsNone(stream.event_key_grant_wire)

    def test_native_runtime_composition_types_are_exported(self):
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf

        runtime = _ndnsf.NativeRequestRuntime()
        runtime.contract = _ndnsf.NativeRequestContract()
        runtime.security = _ndnsf.NativeSecurityPolicySnapshot()
        runtime.budget = _ndnsf.NativeCandidateBudget()
        runtime.state_mapping = _ndnsf.NativeStateTensorMapping()
        runtime.contract.tokenizer_digest = "sha256:" + "a" * 64
        self.assertIsInstance(runtime, _ndnsf.NativeRequestRuntime)
        for name in (
                "NativeRequestCatalog", "NativeRequestPreparation",
                "NativeCanonicalPreparationCatalog", "NativeOfferAdmission",
                "NativeAuthenticatedGrantClient", "NativeConversationCoordinator",
                "native_request_runtime_from_json"):
            self.assertTrue(hasattr(_ndnsf, name), name)

    def test_runtime_config_facade_is_a_thin_native_pass_through(self):
        source = (ROOT / "pythonWrapper/ndnsf/service.py").read_text(encoding="utf-8")
        self.assertIn("def native_runtime_from_config", source)
        self.assertIn("_ndnsf.native_request_runtime_from_json", source)
        self.assertIn("NativeRequestRuntime nativeRequestRuntimeFromJson",
                      (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.cpp").read_text(encoding="utf-8"))

    def test_catalog_loader_keeps_source_validation_native(self):
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf

        with self.assertRaisesRegex(ValueError, "unsupported native request catalog schema"):
            _ndnsf.NativeRequestCatalog.load('{"schema":"invalid"}', b"")

        # The binding accepts bytes at the Python boundary; source parsing and
        # digest/format validation remain in NativeRequestCatalog::load.
        self.assertIn("NativeRequestCatalog::load", BINDINGS.read_text(encoding="utf-8"))
        native_user = MODULE.read_text(encoding="utf-8")
        self.assertIn("native_inference_client_configured", native_user)
        self.assertIn("native_grant_client_from_config", native_user)

    def test_native_grant_config_binds_requester_to_service_user(self):
        source = MODULE.read_text(encoding="utf-8")
        self.assertIn("requester != m_userIdentity", source)
        self.assertIn(
            "native grant requester identity must match the ServiceUser identity",
            source)
        self.assertIn("NativeAuthenticatedGrantClient::issueThroughCore", source)
        self.assertIn("native grant client configuration must not contain authority policy or content keys", source)

    def test_native_conversation_owner_stays_in_cpp_and_is_injected(self):
        source = MODULE.read_text(encoding="utf-8")
        coordinator = (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.cpp").read_text(encoding="utf-8")
        self.assertIn("nativeConversationCoordinatorFromConfig", source)
        self.assertIn('ndnsf-di-native-conversation-v1', coordinator)
        self.assertIn("native conversation key file must be owner-only", coordinator)
        self.assertIn("path escapes configuration directory", coordinator)
        self.assertIn("NativeConversationJournalConfig", coordinator)
        self.assertIn("NativeConversationCoordinator", coordinator)
        service = (ROOT / "pythonWrapper/ndnsf/service.py").read_text(encoding="utf-8")
        self.assertIn("native_conversation_coordinator_from_config", service)
        client = (ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py").read_text(encoding="utf-8")
        self.assertIn("native_conversation_coordinator_from_config", client)
        self.assertIn("_native_conversations", client)

    def test_native_requester_cli_uses_shared_runtime_parser(self):
        source = REQUESTER.read_text(encoding="utf-8")
        self.assertIn("nativeRequestRuntimeFromJson", source)
        self.assertIn('"ndnsf-di-native-request-runtime-v1"', source)
        self.assertNotIn("NativeRequestRuntime runtime;", source)
        self.assertIn("catalog.stateMapping.inputs", source)
        self.assertIn('request.contains("application_request_id")', source)
        self.assertIn("options.applicationRequestId", source)

    def test_native_provider_target_has_relocatable_origin_runpath(self):
        source = (ROOT / "examples/wscript").read_text(encoding="utf-8")
        target_start = source.index("bld.program(name='di-native-provider'")
        target_end = source.index(
            "bld.program(name='di-native-fault-provider'", target_start)
        target = source[target_start:target_end]
        self.assertIn(
            "linkflags=['-pthread', '-Wl,-rpath,$ORIGIN/..']", target)

    def test_native_config_qwen_helper_uses_operator_pinned_tokenizer_digest(self):
        # Execute the maintained helper with the real pybind DTOs.  The fake
        # transport only terminates the request; assertions inspect the exact
        # options and application bytes handed to the native facade.
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user

        tokenizer_digest = "sha256:" + "a" * 64
        observed = {}

        class Handle:
            request_id = "native-qwen-test"

            def result(self, _timeout):
                return SimpleNamespace(payload=b"native-result")

        class Client:
            native_tokenizer_digest = tokenizer_digest

            def publish_application_input_reference(self, *args, **kwargs):
                observed["publication"] = (args, kwargs)
                return {"reference": "pinned"}

            def request_native_reference(self, reference, **kwargs):
                observed["reference"] = reference
                observed["options"] = kwargs["options"]
                observed["application_options"] = kwargs["application_options"]
                observed["request_id"] = kwargs["request_id"]
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=2,
            native_requester_config="operator-pinned.json",
        )
        result = user._native_qwen_request(
            Client(), args, b"qwen-context",
            request_id="0123456789abcdef0123456789abcdef")

        self.assertEqual(result.payload, b"native-result")
        self.assertEqual(
            result.request_id, "0123456789abcdef0123456789abcdef")
        self.assertEqual(
            observed["request_id"], "0123456789abcdef0123456789abcdef")
        self.assertEqual(
            observed["options"].generation.tokenizer_digest,
            tokenizer_digest)
        application_options = json.loads(
            observed["application_options"].decode("utf-8"))
        self.assertEqual(application_options["tokenizerDigest"], tokenizer_digest)

        args.native_requester_config = "operator-pinned.json"
        with self.assertRaisesRegex(RuntimeError, "operator-pinned tokenizer digest"):
            user._native_qwen_request(
                type("UnconfiguredClient", (), {
                    "native_tokenizer_digest": "",
                    "publish_application_input_reference": Client.publish_application_input_reference,
                    "request_native_reference": Client.request_native_reference,
                })(), args, b"qwen-context",
                request_id="0123456789abcdef0123456789abcdef")

    def test_native_qwen_conversation_exports_native_continuation(self):
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user
        from ndnsf_distributed_inference.conversation import ConversationContinuation

        observed = {}
        digest = "sha256:" + "e" * 64
        opaque_checkpoint = b"native-checkpoint-wire"

        class Handle:
            request_id = "0123456789abcdef0123456789abcdef"
            application_request_id = "0123456789abcdef0123456789abcdef"
            conversation_checkpoint = opaque_checkpoint

            def result(self, _timeout):
                return SimpleNamespace(payload=b"native-result")

        class Core:
            _native_conversations = object()
            _native_runtime = SimpleNamespace(
                contract=SimpleNamespace(service_name="/AI/LLM/Pipeline/Fake"))

        class Client:
            _core = Core()
            native_tokenizer_digest = digest

            def publish_application_input_reference(self, *args, **kwargs):
                return {"reference": "native-conversation"}

            def request_native_reference(self, reference, **kwargs):
                observed["reference"] = reference
                observed["options"] = kwargs["options"]
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=2,
            native_requester_config="operator-pinned.json",
        )
        result = user._native_qwen_request(
            Client(), args, b"qwen-context",
            request_id="0123456789abcdef0123456789abcdef",
            conversation=ConversationContinuation("conversation-native-1"),
            canonical_token_ids=(11, 12),
        )
        self.assertEqual(result.conversation_checkpoint, opaque_checkpoint)
        native_conversation = observed["options"].conversation
        self.assertEqual(native_conversation.mode, "FULL_CONTEXT")
        self.assertEqual(native_conversation.service_name, "/AI/LLM/Pipeline/Fake")
        self.assertEqual(native_conversation.canonical_token_ids, [11, 12])
        self.assertEqual(
            native_conversation.generation_id,
            hashlib.sha256(
                b"0123456789abcdef0123456789abcdef").hexdigest()[:32],
        )

    def test_native_qwen_append_delta_maps_authenticated_parent_wire(self):
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user
        from ndnsf_distributed_inference.conversation import (
            ConversationCheckpointV1, ConversationContinuation,
            ConversationInputMode,
        )

        digest = "sha256:" + "f" * 64
        checkpoint = ConversationCheckpointV1(
            conversation_id="conversation-native-append",
            parent_context_epoch=0,
            context_epoch=1,
            service_name="/AI/LLM/Pipeline/Fake",
            requester_identity="/requester/native",
            security_domain_digest=digest,
            model_contract_digest=digest,
            plan_role_map_digest=digest,
            logical_prefix_digest=digest,
            prefix_token_count=2,
            role_receipt_digests={"/role/0": digest},
            issued_at_ms=1,
            expires_at_ms=4_102_444_800_000,
        ).sign(b"k" * 32).to_bytes()
        observed = {}

        class Handle:
            request_id = "abcdef0123456789abcdef0123456789"
            application_request_id = "abcdef0123456789abcdef0123456789"
            conversation_checkpoint = b"successor-wire"

            def result(self, _timeout):
                return SimpleNamespace(payload=b"native-result")

        class Core:
            _native_conversations = object()
            _native_runtime = SimpleNamespace(contract=SimpleNamespace(
                service_name="/AI/LLM/Pipeline/Fake"))

        class Client:
            _core = Core()
            native_tokenizer_digest = digest

            def publish_application_input_reference(self, *_args, **_kwargs):
                return {"reference": "native-append"}

            def request_native_reference(self, _reference, **kwargs):
                observed["options"] = kwargs["options"]
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=2,
            native_requester_config="operator-pinned.json",
        )
        result = user._native_qwen_request(
            Client(), args, b"qwen-delta",
            request_id="abcdef0123456789abcdef0123456789",
            conversation=ConversationContinuation(
                "conversation-native-append", ConversationInputMode.APPEND_DELTA,
                parent_checkpoint=checkpoint, expected_parent_context_epoch=1),
            canonical_token_ids=(11, 12, 13),
        )
        self.assertEqual(result.conversation_checkpoint, b"successor-wire")
        native_conversation = observed["options"].conversation
        self.assertEqual(native_conversation.mode, "APPEND_DELTA")
        self.assertEqual(native_conversation.parent_context_epoch, 1)
        self.assertEqual(native_conversation.plan_role_map_digest, digest)
        self.assertEqual(native_conversation.parent_checkpoint_digest,
                         json.loads(checkpoint)["checkpointDigest"])
        self.assertEqual(native_conversation.expected_roles, ["/role/0"])

    def test_native_config_qwen_full_generation_runs_production_branch(self):
        """Execute the maintained native-config full-generation caller path."""
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user

        tokenizer_digest = "sha256:" + "c" * 64
        observed = {}
        request_id = "qwen-native-full"

        class Handle:
            def __init__(self):
                self.request_id = request_id

            def result(self, _timeout):
                return SimpleNamespace(
                    request_id=request_id,
                    payload=json.dumps({
                        "schema": "NDNSF-DI-FINAL-V1",
                        "tokenIds": [7, 2],
                        "text": "ok",
                        "finishHint": "EOS",
                        "finishReason": "eos",
                    }, sort_keys=True, separators=(",", ":")).encode("utf-8"))

        class Client:
            native_tokenizer_digest = tokenizer_digest

            def publish_application_input_reference(self, *args, **kwargs):
                observed["publication"] = (args, kwargs)
                return {"reference": "native-qwen-full"}

            def request_native_reference(self, reference, **kwargs):
                observed["reference"] = reference
                observed["options"] = kwargs["options"]
                observed["application_options"] = kwargs["application_options"]
                observed["request_id"] = kwargs["request_id"]
                observer = kwargs.get("on_event")
                if observer is not None:
                    observer({
                        "terminal": False,
                        "payload": json.dumps({
                            "schema": "GenerationTokenEventV1",
                            "tokenId": 7,
                            "tokenEpoch": 1,
                        }, sort_keys=True, separators=(",", ":")).encode("utf-8"),
                    })
                    observer({"terminal": True})
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=2,
            native_requester_config="operator-pinned.json",
            diagnostic_token_loop=False,
            automatic_planning_manifest="",
            _qwen_model_type="qwen3_5",
        )
        outcome = user._run_qwen_transformer_generation_sample(
            Client(), args,
            prompt_case={
                "formattedInputIds": [1],
                "referenceGeneratedTokenIds": [7, 2],
                "eosTokenIds": [2],
            },
            generation_id="generation-native-full",
            decoder=lambda token_ids: "ok",
            request_id=request_id,
        )

        self.assertEqual(outcome.status, "OK")
        self.assertEqual(outcome.generated_token_ids, (7, 2))
        self.assertEqual(outcome.decoded_text, "ok")
        self.assertEqual(outcome.stop_reason, "EOS")
        self.assertEqual(observed["request_id"], request_id)
        self.assertEqual(outcome.token_steps[0]["metadata"]["streamEventCount"], 1)
        self.assertEqual(
            observed["options"].generation.tokenizer_digest,
            tokenizer_digest)
        application_options = json.loads(
            observed["application_options"].decode("utf-8"))
        self.assertEqual(application_options["tokenizerDigest"], tokenizer_digest)

    def test_native_config_qwen_full_conversation_returns_checkpoint(self):
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user
        from ndnsf_distributed_inference.conversation import ConversationContinuation

        tokenizer_digest = "sha256:" + "1" * 64
        checkpoint = b"opaque-native-checkpoint"

        class Handle:
            request_id = "qwen-native-conversation"
            application_request_id = "qwen-native-conversation"
            conversation_checkpoint = checkpoint

            def result(self, _timeout):
                return SimpleNamespace(
                    request_id=self.request_id,
                    payload=json.dumps({
                        "schema": "NDNSF-DI-FINAL-V1",
                        "tokenIds": [7, 2],
                        "text": "ok",
                        "finishHint": "EOS",
                        "finishReason": "eos",
                    }, sort_keys=True, separators=(",", ":")).encode("utf-8"))

        class Core:
            _native_conversations = object()
            _native_runtime = SimpleNamespace(contract=SimpleNamespace(
                service_name="/AI/LLM/Pipeline/Fake"))

        class Client:
            _core = Core()
            native_tokenizer_digest = tokenizer_digest

            def publish_application_input_reference(self, *_args, **_kwargs):
                return {"reference": "native-conversation"}

            def request_native_reference(self, _reference, **kwargs):
                observer = kwargs["on_event"]
                observer({
                    "terminal": False,
                    "payload": json.dumps({
                        "schema": "GenerationTokenEventV1",
                        "tokenId": 7,
                        "tokenEpoch": 1,
                    }, sort_keys=True, separators=(",", ":")).encode("utf-8"),
                })
                observer({"terminal": True})
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=2,
            native_requester_config="operator-pinned.json",
            diagnostic_token_loop=False,
            automatic_planning_manifest="",
            _qwen_model_type="qwen3_5",
        )
        outcome = user._run_qwen_transformer_generation_sample(
            Client(), args,
            prompt_case={
                "formattedInputIds": [1],
                "referenceGeneratedTokenIds": [7, 2],
                "eosTokenIds": [2],
            },
            generation_id="generation-native-conversation",
            decoder=lambda token_ids: "ok",
            request_id="qwen-native-conversation",
            conversation=ConversationContinuation("conversation-native-2"),
            canonical_token_ids=(1, 7, 2),
        )
        self.assertEqual(outcome.status, "OK")
        self.assertEqual(outcome.native_conversation_checkpoint, checkpoint)

    def test_native_qwen_append_delta_excludes_generation_oracle_suffix(self):
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user
        from ndnsf_distributed_inference.conversation import (
            ConversationContinuation, ConversationInputMode)
        from unittest import mock

        observed = {}
        checkpoint = b"opaque-successor"

        def fake_native_request(_client, _args, _payload, **kwargs):
            observed["canonical_token_ids"] = kwargs["canonical_token_ids"]
            kwargs["on_event"]({
                "terminal": False,
                "payload": json.dumps({
                    "schema": "GenerationTokenEventV1",
                    "tokenId": 7,
                    "tokenEpoch": 1,
                }, sort_keys=True, separators=(",", ":")).encode("utf-8"),
            })
            kwargs["on_event"]({"terminal": True})
            return SimpleNamespace(
                payload=json.dumps({
                    "schema": "NDNSF-DI-FINAL-V1",
                    "tokenIds": [7, 2],
                    "text": "ok",
                    "finishHint": "EOS",
                    "finishReason": "eos",
                }, sort_keys=True, separators=(",", ":")).encode("utf-8"),
                conversation_checkpoint=checkpoint,
            )

        args = SimpleNamespace(
            timeout_ms=1000,
            max_new_tokens=2,
            native_requester_config="operator-pinned.json",
            diagnostic_token_loop=False,
            automatic_planning_manifest="",
            _qwen_model_type="qwen3_5",
        )
        conversation = ConversationContinuation(
            "conversation-native-append", ConversationInputMode.APPEND_DELTA,
            parent_checkpoint=b"authenticated-parent",
            expected_parent_context_epoch=1)
        with mock.patch.object(user, "_native_qwen_request", side_effect=fake_native_request):
            outcome = user._run_qwen_transformer_generation_sample(
                object(), args,
                prompt_case={
                    "formattedInputIds": [3, 4],
                    "referenceGeneratedTokenIds": [7, 2],
                    "eosTokenIds": [2],
                },
                generation_id="generation-native-append",
                decoder=lambda token_ids: "ok",
                request_id="qwen-native-append",
                conversation=conversation,
                canonical_token_ids=(1, 2, 3, 4, 7, 2),
            )
        self.assertEqual(outcome.status, "OK")
        self.assertEqual(outcome.native_conversation_checkpoint, checkpoint)
        self.assertEqual(observed["canonical_token_ids"], (1, 2, 3, 4))

    def test_native_config_qwen_observer_rejects_non_mapping_payload(self):
        """Malformed decoded observer values must remain a caller-visible error."""
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user

        class Handle:
            request_id = "qwen-native-malformed-event"

            def result(self, _timeout):
                return SimpleNamespace(
                    request_id=self.request_id,
                    payload=b"{}")

        class Client:
            native_tokenizer_digest = "sha256:" + "d" * 64

            def publish_application_input_reference(self, *_args, **_kwargs):
                return {"reference": "malformed-event"}

            def request_native_reference(self, _reference, **kwargs):
                observer = kwargs["on_event"]
                observer([])
                observer({"terminal": True})
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=1,
            native_requester_config="operator-pinned.json",
            diagnostic_token_loop=False,
            automatic_planning_manifest="",
            _qwen_model_type="qwen3_5",
        )
        outcome = user._run_qwen_transformer_generation_sample(
            Client(), args,
            prompt_case={
                "formattedInputIds": [1],
                "referenceGeneratedTokenIds": [2],
                "eosTokenIds": [2],
            },
            generation_id="generation-malformed-event",
            decoder=lambda token_ids: "ok",
            request_id="qwen-native-malformed-event",
        )
        self.assertEqual(outcome.status, "FAILED")
        self.assertIn("observer event validation failed", outcome.error)

    def test_native_config_qwen_observer_rejects_json_scalar_payload(self):
        """A JSON scalar must not bypass the caller observer validation gate."""
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user

        class Handle:
            request_id = "qwen-native-scalar-event"

            def result(self, _timeout):
                return SimpleNamespace(
                    request_id=self.request_id,
                    payload=b"{}")

        class Client:
            native_tokenizer_digest = "sha256:" + "e" * 64

            def publish_application_input_reference(self, *_args, **_kwargs):
                return {"reference": "scalar-event"}

            def request_native_reference(self, _reference, **kwargs):
                observer = kwargs["on_event"]
                observer({
                    "terminal": False,
                    "payload": b"[]",
                })
                observer({"terminal": True})
                return Handle()

        args = SimpleNamespace(
            timeout_ms=1000,
            ack_timeout_ms=100,
            max_new_tokens=1,
            native_requester_config="operator-pinned.json",
            diagnostic_token_loop=False,
            automatic_planning_manifest="",
            _qwen_model_type="qwen3_5",
        )
        outcome = user._run_qwen_transformer_generation_sample(
            Client(), args,
            prompt_case={
                "formattedInputIds": [1],
                "referenceGeneratedTokenIds": [2],
                "eosTokenIds": [2],
            },
            generation_id="generation-scalar-event",
            decoder=lambda token_ids: "ok",
            request_id="qwen-native-scalar-event",
        )
        self.assertEqual(outcome.status, "FAILED")
        self.assertIn("observer payload is not a mapping", outcome.error)

    def test_native_config_qwen_rejects_legacy_diagnostic_loop(self):
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        sys.path.insert(0, str(ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"))
        import user

        args = SimpleNamespace(
            timeout_ms=1000,
            max_new_tokens=1,
            native_requester_config="operator-pinned.json",
            diagnostic_token_loop=True,
            _qwen_model_type="qwen3_5",
        )
        with self.assertRaisesRegex(RuntimeError, "diagnostic-token-loop is unsupported"):
            user._run_qwen_transformer_generation_sample(
                object(), args,
                prompt_case={
                    "formattedInputIds": [1],
                    "referenceGeneratedTokenIds": [2],
                    "eosTokenIds": [2],
                },
                generation_id="generation-diagnostic-rejected",
                decoder=lambda token_ids: "ok",
            )

    def test_native_requester_config_binds_streaming_tokenizer_digest(self):
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"))
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf
        from ndnsf_distributed_inference.app_sdk.client import APPClient

        digest = "sha256:" + "b" * 64
        with self.subTest("missing digest fails closed"):
            client = object.__new__(APPClient)
            client._network_client = object()
            config = {"schema": "ndnsf-di-native-requester-v1", "request": {
                "generation_mode": "TOKEN_STREAMING"}}
            path = ROOT / ".codex-tmp" / "spec182-native-config-missing-digest.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(config), encoding="utf-8")
            try:
                with self.assertRaisesRegex(ValueError, "tokenizer_digest is required"):
                    client.configure_native_requester_from_config(path)
            finally:
                path.unlink(missing_ok=True)

        with self.subTest("unsupported generation mode fails closed"):
            client = object.__new__(APPClient)
            client._network_client = object()
            config = {"schema": "ndnsf-di-native-requester-v1", "request": {
                "generation_mode": "UNSUPPORTED"}}
            path = ROOT / ".codex-tmp" / "spec182-native-config-unsupported-mode.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(config), encoding="utf-8")
            try:
                with self.assertRaisesRegex(ValueError, "generation_mode"):
                    client.configure_native_requester_from_config(path)
            finally:
                path.unlink(missing_ok=True)

        with self.subTest("configured digest is retained"):
            from unittest import mock
            from types import SimpleNamespace
            import tempfile

            class Adapter:
                adapter_id = "qwen"
                descriptor_digest = digest

            class Model:
                model_name = "Qwen/Fixture"
                content_digest = digest
                adapter_id = "qwen"
                adapter = Adapter()

            class Catalog:
                model_ref = Model()
                model_manifest_digest = digest
                canonical_source_digest = digest
                canonical_initializer_object_digest = digest
                state_mapping = SimpleNamespace(inputs={}, outputs={})

            class ServiceUser:
                def load_native_request_catalog(self, *_args, **_kwargs):
                    return Catalog()

                def native_grant_client_from_config(self, *_args, **_kwargs):
                    return object()

                def native_runtime_from_config(self, *_args, **_kwargs):
                    return SimpleNamespace(
                        catalog=Catalog(),
                        contract=SimpleNamespace(service_name="/Qwen"))

            class Network:
                service_user = ServiceUser()

            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                for name in ("model.bin", "trust", "requester.pem", "authority.pem", "offer.pem"):
                    (root / name).write_bytes(b"fixture")
                config = {
                    "schema": "ndnsf-di-native-requester-v1",
                    "core": {"requester_identity": "/user", "authority_identity": "/aa",
                             "group": "/group", "trust_schema_file": "trust"},
                    "limits": {"max_source_bytes": 1024, "max_assembled_bytes": 2048},
                    "catalog": {"source": {"file": "model.bin"},
                                "recipe": {"artifact_profile_digest": digest}},
                    "grant": {"authority_identity": "/aa", "authority_service": "/grant",
                              "authority_public_key_file": "authority.pem",
                              "protection_epoch": "epoch", "requester_private_key_file": "requester.pem"},
                    "offer_admission": {"policy": {}, "public_key_files": {"offer": "offer.pem"},
                                        "candidate_digest": digest},
                    "request": {"service": "/Qwen", "task": "generate",
                                "adapter_composition_digest": digest, "task_descriptor_digest": digest,
                                "input_layout_digest": digest, "security_policy_digest": digest,
                                "max_candidates": 1, "max_policy_ms": 100, "timeout_ms": 1000,
                                "ack_timeout_ms": 100, "generation_mode": "TOKEN_STREAMING",
                                "tokenizer_digest": digest},
                }
                config_path = root / "requester.json"
                config_path.write_text(json.dumps(config), encoding="utf-8")
                client = object.__new__(APPClient)
                client._network_client = Network()
                client.configure_native_requester = mock.Mock(return_value="native")
                original_admission = _ndnsf.NativeOfferAdmission
                _ndnsf.NativeOfferAdmission = lambda *_args: "admission"
                try:
                    self.assertEqual(
                        client.configure_native_requester_from_config(config_path),
                        "native")
                    self.assertEqual(client.native_tokenizer_digest, digest)
                    client.configure_native_requester.assert_called_once()

                    config["grant"]["authority_private_key_file"] = "authority.pem"
                    config_path.write_text(json.dumps(config), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "authority-owned fields"):
                        client.configure_native_requester_from_config(config_path)
                    del config["grant"]["authority_private_key_file"]

                    config["request"]["generation_mode"] = "TOKEN_DIAGNOSTIC"
                    config_path.write_text(json.dumps(config), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "Qwen.*TOKEN_STREAMING"):
                        client.configure_native_requester_from_config(config_path)
                finally:
                    _ndnsf.NativeOfferAdmission = original_admission


if __name__ == "__main__":
    unittest.main()
