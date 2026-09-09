from __future__ import annotations

from pathlib import Path
import unittest
import sys


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
        # The DI binding must not introduce a Python callback strategy or a
        # second planner implementation.  Native callbacks remain an explicit
        # C++ API concern and are deliberately not exported here.
        self.assertNotIn("py::function", source)
        self.assertNotIn("subprocess", source)

    def test_binding_source_has_no_python_strategy_trampoline(self):
        source = BINDINGS.read_text(encoding="utf-8")
        self.assertNotIn("subprocess", source)
        self.assertNotIn("py::eval", source)

    def test_native_dto_names_are_explicitly_mapped(self):
        source = BINDINGS.read_text(encoding="utf-8")
        for field in (
                "model_name", "content_digest", "adapter_id", "payload",
                "transport_mode", "timeout_ms", "ack_timeout_ms"):
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
        options.generation = generation
        options.stream = stream
        options.conversation = continuation
        self.assertEqual(options.generation.generation_id, "generation-1")
        self.assertEqual(options.stream.stream_epoch, 1)
        self.assertEqual(options.conversation.mode, "APPEND_DELTA")

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
        self.assertIsInstance(runtime, _ndnsf.NativeRequestRuntime)
        for name in (
                "NativeRequestCatalog", "NativeRequestPreparation",
                "NativeCanonicalPreparationCatalog", "NativeOfferAdmission",
                "NativeAuthenticatedGrantClient", "native_request_runtime_from_json"):
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

    def test_native_requester_cli_uses_shared_runtime_parser(self):
        source = REQUESTER.read_text(encoding="utf-8")
        self.assertIn("nativeRequestRuntimeFromJson", source)
        self.assertIn('"ndnsf-di-native-request-runtime-v1"', source)
        self.assertNotIn("NativeRequestRuntime runtime;", source)
        self.assertIn("catalog.stateMapping.inputs", source)


if __name__ == "__main__":
    unittest.main()
