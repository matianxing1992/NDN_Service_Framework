from __future__ import annotations

import ast
import importlib
from pathlib import Path
import unittest
import warnings
from unittest import mock
import tempfile
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
CANONICAL_CONTROLLER_MODULE = (
    "ndnsf_distributed_inference.app_sdk.controller")

from ndnsf_distributed_inference.app_sdk import (
    APPClient, APPDeployment, APPProvider, DistributedInferenceEngine,
    FileRequestEnvelopeKeyProvider, InfrastructureAllocationHandle,
    RuntimeAllocationHandoff,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    RequestEnvelopeKey,
    RuntimeJournalKeyError,
    RuntimeJournalUnsafeRootError,
    StaticRequestEnvelopeKeyProvider,
)
from ndnsf_distributed_inference.app_sdk.facades import (
    APPClient as RuntimeAPPClient,
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
