from __future__ import annotations

import ast
import importlib
from pathlib import Path
import unittest
import warnings
from unittest import mock
import tempfile
from types import SimpleNamespace
import json


ROOT = Path(__file__).resolve().parents[2]
CANONICAL_CONTROLLER_MODULE = (
    "ndnsf_distributed_inference.app_sdk.controller")

from ndnsf_distributed_inference.app_sdk import (
    APPClient, APPDeployment, APPProvider, DistributedInferenceEngine,
    FileRequestEnvelopeKeyProvider, InfrastructureAllocationHandle,
    RuntimeAllocationHandoff, InferenceClient,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    RequestEnvelopeKey,
    RuntimeJournal,
    RuntimeJournalKeyError,
    RuntimeJournalUnsafeRootError,
    StaticRequestEnvelopeKeyProvider,
)
from ndnsf_distributed_inference.app_sdk.facades import (
    APPClient as RuntimeAPPClient,
)
from ndnsf_distributed_inference.app_sdk.client import (
    InferenceClient as PublicInferenceClient,
)
from ndnsf_distributed_inference.planner.defaults import DefaultOptimizationSuite
from ndnsf_distributed_inference.sdk import RunnerAdapterRegistry, ObserverRegistry


class AppSdkCompatibilityTest(unittest.TestCase):
    def test_owner_key_file_provider_is_exported_by_public_app_sdk(self):
        self.assertEqual(
            FileRequestEnvelopeKeyProvider.__module__,
            "ndnsf_distributed_inference.app_sdk.runtime_journal",
        )

    def test_production_factories_require_persistent_root_and_owner_key(self):
        network_client = SimpleNamespace(
            deployment=SimpleNamespace(user="/test/requester"),
            optimization_engine=object(),
        )
        network_deployment = SimpleNamespace(deployment=object())
        provider = StaticRequestEnvelopeKeyProvider(
            RequestEnvelopeKey("owner-v1", b"k" * 32))
        with mock.patch(
                "ndnsf_distributed_inference.app_sdk.facades.APPClient.from_config",
                return_value=network_client), mock.patch(
                "ndnsf_distributed_inference.app_sdk.facades.APPDeployment.from_config",
                return_value=network_deployment):
            with self.assertRaisesRegex(
                    RuntimeJournalUnsafeRootError, "persistent state root"):
                APPClient.from_config("policy.yaml")
            with self.assertRaisesRegex(
                    RuntimeJournalUnsafeRootError, "persistent state root"):
                APPDeployment.from_config("policy.yaml")
            with tempfile.TemporaryDirectory(dir=Path.home()) as state_root:
                with self.assertRaisesRegex(
                        RuntimeJournalKeyError, "key provider"):
                    APPClient.from_config(
                        "policy.yaml", state_root=state_root)
                client = APPClient.from_config(
                    "policy.yaml",
                    state_root=state_root,
                    envelope_key_provider=provider,
                )
                deployment = APPDeployment.from_config(
                    "policy.yaml", state_root=state_root)

        self.assertIsInstance(client, APPClient)
        self.assertIsInstance(deployment, APPDeployment)

    def test_canonical_app_types_own_network_factory_adaptation(self):
        network_client = SimpleNamespace(
            deployment=SimpleNamespace(user="/test/requester"),
            optimization_engine=object(),
            deploy_plan=mock.Mock(return_value="network-session"),
            shutdown=mock.Mock(return_value=None),
        )
        network_provider = SimpleNamespace(
            deployment=object(),
            roles_for_service=mock.Mock(return_value=["/role/0"]),
            stop=mock.Mock(return_value=0),
        )
        network_deployment = SimpleNamespace(
            deployment=object(),
            roles_for_service=mock.Mock(return_value=["/role/0"]),
        )
        with tempfile.TemporaryDirectory() as state_root, \
                mock.patch(
                    "ndnsf_distributed_inference.app_sdk.facades.APPClient.from_config",
                    return_value=network_client,
                ), mock.patch(
                    "ndnsf_distributed_inference.app_sdk.facades.APPProvider.from_config",
                    return_value=network_provider,
                ), mock.patch(
                    "ndnsf_distributed_inference.app_sdk.facades.APPDeployment.from_config",
                    return_value=network_deployment,
                ):
            client = APPClient.from_config(
                "policy.yaml",
                state_root=state_root,
                test_only_allow_ephemeral_state_root=True,
            )
            provider = APPProvider.from_config("policy.yaml")
            deployment = APPDeployment.from_config(
                "policy.yaml",
                state_root=state_root,
                test_only_allow_ephemeral_state_root=True,
            )

        self.assertIsInstance(client, APPClient)
        self.assertIsInstance(provider, APPProvider)
        self.assertIsInstance(deployment, APPDeployment)
        self.assertIs(client.deployment, network_client.deployment)
        self.assertEqual(
            client.prepare_session("plan", freshness_ms=120000),
            "network-session",
        )
        network_client.deploy_plan.assert_called_once_with(
            "plan", freshness_ms=120000)
        self.assertEqual(provider.roles_for_service("/service"), ["/role/0"])
        self.assertEqual(deployment.roles_for_service("/service"), ["/role/0"])

    def test_maintained_examples_do_not_import_internal_network_facades(self):
        offenders = []
        for base in (ROOT / "Experiments", ROOT / "examples/python"):
            for path in base.rglob("*.py"):
                if "build" in path.parts:
                    continue
                text = path.read_text(encoding="utf-8")
                if "ndnsf_distributed_inference.app_sdk.facades" in text:
                    offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])

    def test_app_controller_has_one_canonical_owner(self):
        controller_module = importlib.import_module(
            "ndnsf_distributed_inference.app_sdk.controller")
        facades_module = importlib.import_module(
            "ndnsf_distributed_inference.app_sdk.facades")
        app_sdk_module = importlib.import_module(
            "ndnsf_distributed_inference.app_sdk")
        root_module = importlib.import_module("ndnsf_distributed_inference")

        app_controller = controller_module.APPController
        self.assertEqual(
            app_controller.__module__,
            "ndnsf_distributed_inference.app_sdk.controller",
        )
        self.assertIs(facades_module.APPController, app_controller)
        self.assertIs(app_sdk_module.APPController, app_controller)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            self.assertIs(root_module.APPController, app_controller)

    def test_public_app_classes_have_one_canonical_owner(self):
        compatibility_module = importlib.import_module(
            "ndnsf_distributed_inference.app")
        root_module = importlib.import_module("ndnsf_distributed_inference")
        canonical = {
            "APPClient": APPClient,
            "APPDeployment": APPDeployment,
            "APPProvider": APPProvider,
        }
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            for name, expected in canonical.items():
                self.assertIs(getattr(compatibility_module, name), expected)
                self.assertIs(getattr(root_module, name), expected)

    def test_maintained_runtime_callers_use_canonical_controller_owner(self):
        checked = []
        for base in (ROOT / "Experiments", ROOT / "examples/python"):
            for path in base.rglob("*.py"):
                if "build" in path.parts:
                    continue
                text = path.read_text(encoding="utf-8")
                if "APPController" not in text:
                    continue
                tree = ast.parse(text, filename=str(path))
                references = []
                for node in ast.walk(tree):
                    if isinstance(node, ast.ImportFrom) and any(
                            alias.name == "APPController" for alias in node.names):
                        references.append(node.module or "")
                    if (isinstance(node, ast.Constant)
                            and isinstance(node.value, str)
                            and "APPController" in node.value
                            and "from ndnsf_distributed_inference" in node.value):
                        prefix = node.value.split(" import APPController", 1)[0]
                        references.append(
                            prefix[5:] if prefix.startswith("from ") else prefix)
                self.assertTrue(references, f"unclassified APPController caller: {path}")
                self.assertEqual(
                    set(references), {CANONICAL_CONTROLLER_MODULE}, str(path))
                checked.append(path)
        self.assertGreaterEqual(len(checked), 7)

    def test_public_facade_engine_suite_adapter_observer_construct(self):
        self.assertIsNotNone(DistributedInferenceEngine(DefaultOptimizationSuite()))
        self.assertIsNotNone(RunnerAdapterRegistry()); self.assertIsNotNone(ObserverRegistry())

    def test_public_inference_client_exposes_explicit_native_route(self):
        core = SimpleNamespace(
            native_client=object(),
            configure_native_requester=mock.Mock(return_value="native"),
            request_native=mock.Mock(return_value="handle"),
        )
        client = PublicInferenceClient.__new__(PublicInferenceClient)
        client._core = core
        self.assertIs(client.native_client, core.native_client)
        self.assertEqual(client.configure_native_requester("runtime", "admission"), "native")
        self.assertEqual(client.request_native(
            model="model", input="input", split_strategy="split",
            placement_strategy="placement", options="options"), "handle")
        core.configure_native_requester.assert_called_once_with("runtime", "admission")
        core.request_native.assert_called_once_with(
            model="model", input="input", split_strategy="split",
            placement_strategy="placement", options="options")

    def test_public_inference_client_forwards_native_reference_route(self):
        core = SimpleNamespace(request_native_reference=mock.Mock(return_value="handle"))
        client = PublicInferenceClient.__new__(PublicInferenceClient)
        client._core = core
        callback = mock.Mock()
        self.assertEqual(
            client.request_native_reference(
                {"dataName": "/input", "manifestDigest": "manifest"},
                options="options", task_name="task",
                application_options=b"opts", on_event=callback),
            "handle")
        core.request_native_reference.assert_called_once_with(
            {"dataName": "/input", "manifestDigest": "manifest"},
            options="options", task_name="task",
            application_options=b"opts", on_event=callback)

    def test_public_inference_client_forwards_native_application_request_id(self):
        core = SimpleNamespace(request_native_reference=mock.Mock(return_value="handle"))
        client = PublicInferenceClient.__new__(PublicInferenceClient)
        client._core = core

        self.assertEqual(
            client.request_native_reference(
                {"dataName": "/input", "manifestDigest": "manifest"},
                options="options", request_id="wire-request"),
            "handle")
        core.request_native_reference.assert_called_once_with(
            {"dataName": "/input", "manifestDigest": "manifest"},
            options="options", task_name=None, application_options=None,
            on_event=None, request_id="wire-request")

    def test_core_native_reference_route_preserves_identity_and_avoids_planner(self):
        from ndnsf_distributed_inference.app_sdk.client import APPClient as CoreAPPClient
        from ndnsf_distributed_inference.repo_reference import LargeDataReference
        import ndnsf

        reference = LargeDataReference(
            data_name="/service/input/v=1",
            manifest_digest="sha256:" + "a" * 64,
            plaintext_size=4,
            ciphertext_digest="sha256:" + "b" * 64,
            authorization_scope="/SERVICE/service",
            protection_epoch="epoch-1",
            object_id="object-1",
        )
        with tempfile.TemporaryDirectory() as state_root:
            journal = RuntimeJournal.for_test(state_root, "native")
            journal.append("application-input-publication", {
                "referenceDigest": reference.digest(),
            })
            handle = SimpleNamespace(observe=mock.Mock())
            native = SimpleNamespace(request=mock.Mock(return_value=handle))
            planner = SimpleNamespace(request=mock.Mock())
            client = CoreAPPClient(
                journal, automatic_planner=planner, native_client=native)
            client._native_model = SimpleNamespace(adapter=SimpleNamespace(
                input_schema_digest="sha256:" + "c" * 64,
                options_schema_digest="sha256:" + "d" * 64))
            client._native_runtime = SimpleNamespace(
                contract=SimpleNamespace(task_name="default-task"))
            client._native_splitter = "split"
            options = SimpleNamespace(task_name="")
            callback = mock.Mock()

            class FakeInput:
                def __init__(self):
                    self.payload = []
                    self.options = []

            fake_ndnsf = SimpleNamespace(
                NativeApplicationInput=FakeInput,
                NativeInputTransportMode=SimpleNamespace(
                    REPOSITORY_REFERENCE="repo"),
                NativePreSplitFirstPlacement=lambda: "placement",
            )
            with mock.patch.object(ndnsf, "_ndnsf", fake_ndnsf):
                returned = client.request_native_reference(
                    reference, options=options, application_options=b"cfg",
                    on_event=callback, request_id="wire-request")

            self.assertIs(returned, handle)
            planner.request.assert_not_called()
            native.request.assert_called_once()
            args, kwargs = native.request.call_args
            self.assertEqual(args[0], client._native_model)
            native_input = args[1]
            self.assertEqual(native_input.transport_mode, "repo")
            self.assertEqual(native_input.payload, [])
            self.assertEqual(native_input.options, b"cfg")
            self.assertEqual(
                native_input.repository_reference,
                json.dumps(reference.to_dict(), sort_keys=True,
                           separators=(",", ":"), ensure_ascii=False))
            self.assertEqual(options.task_name, "default-task")
            self.assertEqual(options.application_request_id, "wire-request")
            handle.observe.assert_called_once_with(callback)

    def test_public_inference_client_forwards_conversation_owner(self):
        core = SimpleNamespace(
            configure_native_requester=mock.Mock(return_value="native"),
        )
        client = PublicInferenceClient.__new__(PublicInferenceClient)
        client._core = core
        conversations = object()

        self.assertEqual(
            client.configure_native_requester(
                "runtime", "admission", conversations), "native")
        core.configure_native_requester.assert_called_once_with(
            "runtime", "admission", conversations)

    def test_inference_client_is_exported_from_public_app_sdk(self):
        self.assertIs(InferenceClient, PublicInferenceClient)

    def test_core_native_route_does_not_fallback_to_automatic_planner(self):
        with tempfile.TemporaryDirectory() as state_root:
            native = SimpleNamespace(request=mock.Mock(return_value="handle"))
            planner = SimpleNamespace(request=mock.Mock())
            client = APPClient(
                RuntimeJournal.for_test(state_root, "native"),
                automatic_planner=planner,
                native_client=native,
            )
            self.assertEqual(client.request_native(
                model="model", input="input", split_strategy="split",
                placement_strategy="placement", options="options"), "handle")
            native.request.assert_called_once_with(
                "model", "input", "split", "placement", "options")
            planner.request.assert_not_called()

    def test_generic_task_native_route_preserves_input_identity_and_handle_shape(self):
        from ndnsf_distributed_inference.adapters import ApplicationInput
        from ndnsf_distributed_inference.app_sdk.client import APPClient as CoreAPPClient
        from ndnsf_distributed_inference.app_sdk.placement import InferenceTaskRef, TaskOptions
        import ndnsf

        digest = lambda char: "sha256:" + char * 64
        input_value = ApplicationInput.from_inline(
            task_name="object-detection",
            input_schema_digest=digest("a"),
            options_schema_digest=digest("b"),
            payload=b"encoded-input",
            options=b"{}",
        )
        task = InferenceTaskRef(
            task_name="object-detection",
            adapter_name="yolo",
            adapter_descriptor_digest=digest("c"),
            adapter_composition_digest=digest("d"),
            task_descriptor_digest=digest("e"),
        )
        model = SimpleNamespace(
            model_name="yolo26n",
            content_digest=digest("f"),
            semantics_digest=digest("1"),
            source_revision="rev-1",
        )
        native_model = SimpleNamespace(
            model_name="yolo26n",
            content_digest=digest("f"),
            semantics_digest=digest("1"),
            source_revision="rev-1",
            adapter=SimpleNamespace(
                input_schema_digest=digest("a"),
                options_schema_digest=digest("b"),
            ),
        )

        class FakeOptions:
            def __init__(self):
                self.timeout_ms = 30000
                self.ack_timeout_ms = 5000
                self.task_name = ""
                self.output_mode = "FULL"
                self.application_request_id = ""

        class FakeInput:
            def __init__(self):
                self.task_name = ""
                self.input_schema_digest = ""
                self.options_schema_digest = ""
                self.payload = b""
                self.options = b""
                self.transport_mode = "inline"
                self.repository_reference = ""

        class FakePlacement:
            pass

        native_result = SimpleNamespace(payload=b"native-result")
        native_handle = SimpleNamespace(
            request_id="/NDNSF/DI/REQUEST/test-1",
            application_request_id="compat-request",
            status_name="SUCCEEDED",
            result=mock.Mock(return_value=native_result),
            cancel=mock.Mock(),
        )
        native = SimpleNamespace(request=mock.Mock(return_value=native_handle))
        planner = SimpleNamespace(request=mock.Mock())
        with tempfile.TemporaryDirectory() as state_root:
            client = CoreAPPClient(
                RuntimeJournal.for_test(state_root, "native"),
                automatic_planner=planner,
                native_client=native,
            )
            client._native_model = native_model
            client._native_runtime = SimpleNamespace(
                contract=SimpleNamespace(task_name="object-detection"))
            client._native_splitter = "split"
            fake_ndnsf = SimpleNamespace(
                NativeRequestOptions=FakeOptions,
                NativeApplicationInput=FakeInput,
                NativePreSplitFirstPlacement=FakePlacement,
            )
            with mock.patch.object(ndnsf, "_ndnsf", fake_ndnsf):
                returned = client.request_task(
                    model=model,
                    task=task,
                    input=input_value,
                    timeout_ms=4000,
                    options=TaskOptions(digest("b"), b"{}"),
                    request_id="compat-request",
                )

        self.assertEqual(returned.request_id, "/NDNSF/DI/REQUEST/test-1")
        self.assertEqual(returned.response(1000).payload, b"native-result")
        planner.request.assert_not_called()
        native.request.assert_called_once()
        native_input = native.request.call_args.args[1]
        native_options = native.request.call_args.args[4]
        self.assertEqual(native_input.task_name, "object-detection")
        self.assertEqual(native_input.payload, b"encoded-input")
        self.assertEqual(native_input.options, b"{}")
        self.assertEqual(native_options.timeout_ms, 4000)
        self.assertEqual(native_options.ack_timeout_ms, 2000)
        self.assertEqual(native_options.application_request_id, "compat-request")

    def test_generic_task_native_route_rejects_model_or_option_drift(self):
        from ndnsf_distributed_inference.adapters import ApplicationInput
        from ndnsf_distributed_inference.app_sdk.client import APPClient as CoreAPPClient
        from ndnsf_distributed_inference.app_sdk.placement import InferenceTaskRef, TaskOptions

        digest = lambda char: "sha256:" + char * 64
        value = ApplicationInput.from_inline(
            task_name="task",
            input_schema_digest=digest("a"),
            options_schema_digest=digest("b"), payload=b"x", options=b"{}")
        task = InferenceTaskRef(
            task_name="task", adapter_name="adapter",
            adapter_descriptor_digest=digest("c"),
            adapter_composition_digest=digest("d"),
            task_descriptor_digest=digest("e"))
        with tempfile.TemporaryDirectory() as state_root:
            client = CoreAPPClient(
                RuntimeJournal.for_test(state_root, "native"),
                automatic_planner=SimpleNamespace(request=mock.Mock()),
                native_client=object(),
            )
            client._native_model = SimpleNamespace(
                model_name="native-model", content_digest=digest("f"),
                semantics_digest=digest("1"), source_revision="rev",
                adapter=SimpleNamespace(
                    input_schema_digest=digest("a"),
                    options_schema_digest=digest("b")))
            client._native_runtime = SimpleNamespace(
                contract=SimpleNamespace(task_name="task"))
            client._native_splitter = object()
            with self.assertRaisesRegex(RuntimeError, "NATIVE_MODEL_IDENTITY_MISMATCH"):
                client.request_task(
                    model=SimpleNamespace(
                        model_name="other-model", content_digest=digest("f"),
                        semantics_digest=digest("1"), source_revision="rev"),
                    task=task, input=value, timeout_ms=1000,
                    options=TaskOptions(digest("b"), b"{}"))

    def test_generic_stream_native_route_preserves_callbacks_and_planner_non_fallback(self):
        from ndnsf_distributed_inference.adapters import ApplicationInput
        from ndnsf_distributed_inference.app_sdk.client import APPClient as CoreAPPClient
        from ndnsf_distributed_inference.app_sdk.placement import InferenceTaskRef
        import ndnsf

        digest = lambda char: "sha256:" + char * 64
        input_value = ApplicationInput.from_inline(
            task_name="object-detection",
            input_schema_digest=digest("a"),
            options_schema_digest=digest("b"),
            payload=b"encoded-input", options=b"{}")
        task = InferenceTaskRef(
            task_name="object-detection", adapter_name="yolo",
            adapter_descriptor_digest=digest("c"),
            adapter_composition_digest=digest("d"),
            task_descriptor_digest=digest("e"))
        model = SimpleNamespace(
            model_name="yolo26n", content_digest=digest("f"),
            semantics_digest=digest("1"), source_revision="rev-1")
        native_model = SimpleNamespace(
            model_name="yolo26n", content_digest=digest("f"),
            semantics_digest=digest("1"), source_revision="rev-1",
            adapter=SimpleNamespace(
                input_schema_digest=digest("a"),
                options_schema_digest=digest("b")))

        class FakeStream:
            def __init__(self):
                self.mode = None
                self.generation_id = []
                self.attempt_epoch = 0
                self.stream_epoch = 0
                self.deadline_epoch_ms = 0
                self.allow_replacement = False
                self.max_replacements = 0

        class FakeOptions:
            def __init__(self):
                self.timeout_ms = 30000
                self.ack_timeout_ms = 5000
                self.task_name = ""
                self.output_mode = "FULL"
                self.application_request_id = ""
                self.stream = None

        class FakeInput:
            def __init__(self):
                self.task_name = ""
                self.input_schema_digest = ""
                self.options_schema_digest = ""
                self.payload = b""
                self.options = b""
                self.transport_mode = "inline"
                self.repository_reference = ""

        class FakePlacement:
            pass

        class FakeNativeHandle:
            request_id = "/NDNSF/DI/REQUEST/stream-1"
            application_request_id = "stream-request"
            status_name = "SUCCEEDED"

            def __init__(self):
                self.callback = None
                self.result = mock.Mock(
                    return_value=SimpleNamespace(payload=b"stream-result"))
                self.cancel = mock.Mock()

            def observe(self, callback):
                self.callback = callback

        native_handle = FakeNativeHandle()
        native = SimpleNamespace(request=mock.Mock(return_value=native_handle))
        planner = SimpleNamespace(request_streaming=mock.Mock())
        events, completed = [], []
        with tempfile.TemporaryDirectory() as state_root:
            client = CoreAPPClient(
                RuntimeJournal.for_test(state_root, "native-stream"),
                automatic_planner=planner, native_client=native)
            client._native_model = native_model
            client._native_runtime = SimpleNamespace(
                contract=SimpleNamespace(task_name="object-detection",
                                          generation_mode="TOKEN_DIAGNOSTIC"))
            client._native_splitter = "split"
            fake_ndnsf = SimpleNamespace(
                NativeRequestOptions=FakeOptions,
                NativeStreamRequestOptions=FakeStream,
                NativeApplicationInput=FakeInput,
                NativePreSplitFirstPlacement=FakePlacement,
                NativeInvocationMode=SimpleNamespace(NORMAL="normal", TARGETED="targeted"),
            )
            with mock.patch.object(ndnsf, "_ndnsf", fake_ndnsf):
                returned = client.request_streaming(
                    model=model, task=task, input=input_value, timeout_ms=4000,
                    stream_options={"generation_id": "a" * 32},
                    on_event=events.append, on_complete=completed.append,
                    request_id="stream-request")
                native_handle.callback({
                    "request_id": native_handle.request_id,
                    "payload": b"chunk", "terminal": False})
                native_handle.callback({
                    "request_id": native_handle.request_id,
                    "payload": b"", "terminal": True})

        self.assertEqual(returned.request_id, native_handle.request_id)
        self.assertEqual(returned.generation_id, "a" * 32)
        self.assertEqual(returned.stream_events, (b"chunk",))
        self.assertEqual(returned.stream_complete, b"stream-result")
        self.assertEqual(events, [b"chunk"])
        self.assertEqual(completed, [b"stream-result"])
        self.assertEqual(returned.response(1000).payload, b"stream-result")
        planner.request_streaming.assert_not_called()
        native.request.assert_called_once()
        native_options = native.request.call_args.args[4]
        self.assertEqual(native_options.output_mode, "STREAM")
        self.assertEqual(native_options.application_request_id, "stream-request")
        self.assertEqual(native_options.stream.generation_id, list(bytes.fromhex("a" * 32)))

    def test_generic_stream_native_route_rejects_python_conversation(self):
        from ndnsf_distributed_inference.adapters import ApplicationInput
        from ndnsf_distributed_inference.app_sdk.client import APPClient as CoreAPPClient
        from ndnsf_distributed_inference.app_sdk.placement import InferenceTaskRef
        from ndnsf_distributed_inference.conversation import ConversationContinuation

        digest = lambda char: "sha256:" + char * 64
        value = ApplicationInput.from_inline(
            task_name="task", input_schema_digest=digest("a"),
            options_schema_digest=digest("b"), payload=b"x", options=b"{}")
        task = InferenceTaskRef(
            task_name="task", adapter_name="adapter",
            adapter_descriptor_digest=digest("c"),
            adapter_composition_digest=digest("d"),
            task_descriptor_digest=digest("e"))
        with tempfile.TemporaryDirectory() as state_root:
            client = CoreAPPClient(
                RuntimeJournal.for_test(state_root, "native-stream"),
                automatic_planner=SimpleNamespace(request_streaming=mock.Mock()),
                native_client=object())
            client._native_model = SimpleNamespace(
                model_name="model", content_digest=digest("f"),
                semantics_digest=digest("1"), source_revision="rev",
                adapter=SimpleNamespace(input_schema_digest=digest("a"),
                                        options_schema_digest=digest("b")))
            client._native_runtime = SimpleNamespace(
                contract=SimpleNamespace(task_name="task",
                                          generation_mode="TOKEN_DIAGNOSTIC"))
            client._native_splitter = object()
            conversation = ConversationContinuation(
                conversation_id="conversation-0001")
            with self.assertRaisesRegex(RuntimeError, "configured native owner"):
                client.request_streaming(
                    model=SimpleNamespace(
                        model_name="model", content_digest=digest("f"),
                        semantics_digest=digest("1"), source_revision="rev"),
                    task=task, input=value, timeout_ms=4000,
                    conversation=conversation,
                    on_event=lambda payload: None,
                    on_complete=lambda payload: None,
                    on_error=lambda error: None)

    def test_native_stream_terminal_failure_uses_on_error(self):
        from ndnsf_distributed_inference.app_sdk.client import NativeStreamingHandle

        native = SimpleNamespace(status_name="FAILED", request_id="native-1")
        errors, completed = [], []
        handle = NativeStreamingHandle(
            native, timeout_ms=1000, on_complete=completed.append,
            on_error=errors.append, generation_id="b" * 32)
        handle._native_event({"payload": b"", "terminal": True})

        self.assertEqual(completed, [])
        self.assertEqual(errors[0]["code"], "NATIVE_STREAM_FAILED")
        self.assertEqual(handle.stream_error["status"], "FAILED")

    def test_app_client_constructor_resolves_canonical_engine_and_defaults(self):
        client = RuntimeAPPClient(object(), object())

        self.assertIsInstance(
            client.optimization_engine, DistributedInferenceEngine)
        self.assertIsInstance(
            client.optimization_engine.suite, DefaultOptimizationSuite)

    def test_runtime_plan_async_preserves_canonical_durable_request_id(self):
        future = object()
        network = SimpleNamespace(infer_async=mock.Mock(return_value=future))
        client = RuntimeAPPClient(object(), network)
        client._with_service_dependencies = mock.Mock(return_value="bound-plan")

        returned = client.infer_async(
            "plan", b"payload", ack_timeout_ms=11, timeout_ms=22,
            freshness_ms=33, request_id="durable-request-id")

        self.assertIs(returned, future)
        network.infer_async.assert_called_once_with(
            "bound-plan", b"payload", ack_timeout_ms=11, timeout_ms=22,
            freshness_ms=33, on_result=None, on_error=None,
            request_id="durable-request-id")

    def test_runtime_handoff_is_immutable_and_scheduler_state_is_distinct(self):
        digest = "sha256:" + "a" * 64
        handoff = RuntimeAllocationHandoff(
            digest, digest, digest, digest, digest, (digest,),
            ("prefill", "decode"), digest, digest, digest, digest)
        self.assertTrue(handoff.digest().startswith("sha256:"))
        handle = InfrastructureAllocationHandle(
            "spec110-slurm", handoff.digest(), "123", {"gpu": 2},
            {"gpu": 2}, "RUNNING")
        self.assertEqual(handle.scheduler_state, "RUNNING")
        with self.assertRaisesRegex(ValueError, "infrastructure"):
            InfrastructureAllocationHandle(
                "spec110-slurm", handoff.digest(), "123", {}, {}, "ACTIVE")


if __name__=="__main__": unittest.main()
