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

    def test_application_and_client_expose_model_first_and_explicit_preplanned(self):
        from ndnsf_distributed_inference.api import (
            InferenceApplication, InferenceClient,
        )

        app = inspect.signature(InferenceApplication.request)
        client = inspect.signature(InferenceClient.request_model)
        for name in ("model", "input", "generation", "strategy", "request_id"):
            self.assertIn(name, app.parameters)
            self.assertIn(name, client.parameters)
        self.assertIn("legacy_deployment", app.parameters)
        self.assertTrue(hasattr(InferenceApplication, "request_preplanned"))
        self.assertTrue(hasattr(InferenceClient, "request_preplanned"))

    def test_preferred_request_has_no_numeric_or_empty_sentinel_mode(self):
        from ndnsf_distributed_inference.api import InferenceApplication

        signature = inspect.signature(InferenceApplication.request)
        self.assertIn("generation", signature.parameters)
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
        runtime = {
            "application": {"identity": "/app/creator"},
            "controller": "/controller", "service": "/Inference/Generate"}
        with tempfile.TemporaryDirectory() as root, patch(
                "ndnsf_distributed_inference.policy.load_config",
                return_value=runtime), patch(
                "ndnsf_distributed_inference.app_sdk.client.InferenceClient.from_application_config",
                return_value=client) as factory:
            InferenceApplication.from_config(
                "app.yaml", state_root=root, optimization=suite)
        self.assertIs(factory.call_args.kwargs["optimization"], suite)
        self.assertEqual(len(authorized), 1)

    def test_application_uses_presplit_first_as_the_unmodified_v3_default(self):
        from ndnsf_distributed_inference.app_sdk.application import (
            InferenceApplication,
        )
        from ndnsf_distributed_inference.planner.presplit_first import (
            PreSplitFirstStrategy,
        )

        configured = {}
        core = SimpleNamespace(
            requester_identity="/app/creator",
            configure_automatic_planning=lambda **kwargs: configured.update(kwargs),
        )
        client = SimpleNamespace(
            _core=core,
            deployments=SimpleNamespace(authorize_application=lambda *_: None),
        )
        runtime = {
            "application": {"identity": "/app/creator"},
            "controller": "/controller", "service": "/Inference/Generate",
        }
        with tempfile.TemporaryDirectory() as root, patch(
                "ndnsf_distributed_inference.policy.load_config",
                return_value=runtime), patch(
                "ndnsf_distributed_inference.app_sdk.client.InferenceClient.from_application_config",
                return_value=client):
            InferenceApplication.from_config(
                "app.yaml", state_root=root, adapters=(object(),),
                verify_offer_signature=lambda _offer: True,
            )

        self.assertIs(type(configured["strategy"]), PreSplitFirstStrategy)
        self.assertEqual(configured["strategy"].placement_profile,
                         "DI_PLACEMENT_V3")


if __name__ == "__main__":
    unittest.main()
