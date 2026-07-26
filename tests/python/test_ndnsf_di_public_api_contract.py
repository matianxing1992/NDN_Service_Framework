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
    def test_preferred_namespace_matches_allowlist_and_owner_manifest(self):
        from ndnsf_distributed_inference import api

        self.assertEqual(set(api.__all__), set(MANIFEST["apiExports"]))
        for name, module in MANIFEST["owners"].items():
            self.assertEqual(getattr(api, name).__module__, module)
        self.assertNotIn("__getattr__", api.__dict__)

    def test_application_and_client_request_signatures_are_identical(self):
        from ndnsf_distributed_inference.api import (
            InferenceApplication, InferenceClient,
        )

        app = inspect.signature(InferenceApplication.request)
        client = inspect.signature(InferenceClient.request)
        self.assertEqual(
            tuple(app.parameters)[1:], tuple(client.parameters)[1:])
        self.assertEqual(tuple(app.parameters)[1], "deployment")
        self.assertFalse(any(name.startswith("_") for name in
                             tuple(app.parameters)[1:]))
        self.assertEqual(app.return_annotation, client.return_annotation)

    def test_preferred_request_has_no_numeric_or_empty_sentinel_mode(self):
        from ndnsf_distributed_inference.api import InferenceClient

        signature = inspect.signature(InferenceClient.request)
        self.assertIn("timeout", signature.parameters)
        self.assertIn("deadline", signature.parameters)
        self.assertNotIn("timeout_ms", signature.parameters)
        self.assertNotIn("deployment_revision", signature.parameters)
        self.assertNotIn("service", signature.parameters)

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
        with tempfile.TemporaryDirectory() as root, patch(
                "ndnsf_distributed_inference.app_sdk.client.InferenceClient.from_config",
                return_value=client) as factory:
            InferenceApplication.from_config(
                "app.yaml", state_root=root, optimization=suite)
        self.assertIs(factory.call_args.kwargs["optimization"], suite)
        self.assertEqual(len(authorized), 1)


if __name__ == "__main__":
    unittest.main()
