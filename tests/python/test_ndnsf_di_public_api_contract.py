from __future__ import annotations

import inspect
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads((
    ROOT / "tests/fixtures/ndnsf-di-user-api/public-api.json"
).read_text(encoding="utf-8"))


class PublicApiContractTest(unittest.TestCase):
    def test_namespace_has_no_retired_symbol_exports(self):
        import importlib.util
        import ndnsf_distributed_inference as namespace
        from ndnsf_distributed_inference import api, sdk

        historical = json.loads((ROOT / "tests/fixtures/ndnsf-di-core-app-separation/"
                                 "compatibility-surface-ca1d6b2a.json").read_text())
        names = [entry["surfaceId"].split(":", 1)[1]
                 for entry in historical["entries"]
                 if entry["surfaceKind"] == "python-export"]
        self.assertTrue(names)
        for name in names:
            self.assertFalse(hasattr(namespace, name), name)
        self.assertIsNotNone(api.Runtime)
        self.assertIsNotNone(sdk.ModelPlacementStrategy)
        for name in ("app", "controller", "gui", "onnx_graph", "onnx_executor",
                     "qwen_pilot", "llm_runtime", "planner_registry", "split_planner"):
            self.assertIsNone(importlib.util.find_spec(
                "ndnsf_distributed_inference." + name), name)

    def test_preferred_namespace_matches_allowlist_and_owner_manifest(self):
        from ndnsf_distributed_inference import api, app_sdk, provider_api
        from ndnsf import _ndnsf

        self.assertEqual(set(api.__all__), set(MANIFEST["apiExports"]))
        for name, module in MANIFEST["owners"].items():
            self.assertEqual(getattr(api, name).__module__, module)
        self.assertIs(api.Runtime, _ndnsf.Runtime)
        self.assertIs(api.PreparedModel, _ndnsf.PreparedModel)
        self.assertIs(api.ProviderConfig, _ndnsf.NativeProviderConfig)
        self.assertIs(provider_api.ProviderConfig, api.ProviderConfig)
        self.assertFalse(hasattr(api, "NativeProviderConfig"))
        self.assertFalse(hasattr(provider_api, "NativeProviderConfig"))
        self.assertFalse(hasattr(app_sdk.APPProvider, "serve"))
        self.assertFalse({"Enum", "dataclass", "hashlib"} & set(app_sdk.__all__))
        self.assertNotIn("__getattr__", api.__dict__)



    def test_application_forwards_optimizer_and_does_not_own_runner_registry(self):
        from ndnsf_distributed_inference.app_sdk.application import InferenceApplication

        signature = inspect.signature(InferenceApplication.from_config)
        self.assertIn("optimization", signature.parameters)
        self.assertNotIn("runner_adapters", signature.parameters)
        authorized = []
        client = SimpleNamespace(
            _core=SimpleNamespace(requester_identity="/app/creator"),
            deployments=SimpleNamespace(
                authorize_application=lambda *args: authorized.append(args)))
        suite = object()
        runtime = {
            "application": {"identity": "/app/creator"},
            "controller": "/controller", "service": "/Inference/Generate",
            "group": "/test/group"}
        with tempfile.TemporaryDirectory() as root, patch(
                "ndnsf_distributed_inference.policy.load_config",
                return_value=runtime), patch(
                "ndnsf_distributed_inference.app_sdk.client.InferenceClient.from_application_config",
                return_value=client) as factory:
            InferenceApplication.from_config(
                "app.yaml", state_root=root, optimization=suite)
        self.assertIs(factory.call_args.kwargs["optimization"], suite)
        self.assertEqual(len(authorized), 1)



if __name__ == "__main__":
    unittest.main()
