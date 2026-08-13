#!/usr/bin/env python3
"""Spec 168 model-first API and deferred-planning boundary tests."""

from __future__ import annotations

import unittest
import warnings

from ndnsf_distributed_inference.app_sdk.application import (
    ApplicationDefinitionSigner,
    InferenceApplication,
)
from ndnsf_distributed_inference.app_sdk.contracts import (
    ApplicationRuntimeConfig,
    GenerationConfig,
    GenerationInput,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    AutomaticPlanningCoordinator,
    ModelRef,
)


DIGEST = "sha256:" + "a" * 64
TOKENIZER = "sha256:" + "b" * 64


class Deployments:
    def authorize_application(self, *args):
        self.authorization = args


class Client:
    def __init__(self):
        self.deployments = Deployments()
        self.calls = []
        self.requests = object()

    def request_model(self, **kwargs):
        self.calls.append(("model", kwargs))
        return "model-handle"

    def request_preplanned(self, deployment, **kwargs):
        self.calls.append(("preplanned", deployment, kwargs))
        return "preplanned-handle"


class Spec168DeferredPlanningTest(unittest.TestCase):
    def setUp(self):
        self.client = Client()
        self.application = InferenceApplication(
            ApplicationDefinitionSigner.generate("/spec168/user"), self.client)
        self.model = ModelRef(
            name="Qwen/Qwen3-0.6B", revision="immutable-revision",
            content_digest=DIGEST, tokenizer_digest=TOKENIZER)
        self.input = GenerationInput(prompt="Explain data-driven execution.")
        self.generation = GenerationConfig(max_new_tokens=8, do_sample=False)

    def test_normal_application_config_rejects_deployment_plan_fields(self):
        valid = ApplicationRuntimeConfig.from_mapping({
            "application": {"identity": "/spec168/user"},
            "controller": "/spec168/controller",
            "service": "/Inference/Generate",
            "ack_timeout_ms": 500,
            "hard_deadline_ms": 900_000,
            "progress_idle_ms": 30_000,
        })
        self.assertEqual(valid.service, "/Inference/Generate")
        for field in ("roles", "dependencies", "shards", "deployment",
                      "deployment_revision"):
            payload = {
                "application": {"identity": "/spec168/user"},
                "controller": "/spec168/controller",
                "service": "/Inference/Generate",
                field: [],
            }
            with self.subTest(field=field), self.assertRaisesRegex(
                    ValueError, "preplanned"):
                ApplicationRuntimeConfig.from_mapping(payload)

    def test_model_identity_uses_name_revision_content_and_tokenizer_hash(self):
        self.assertEqual(self.model.model_name, "Qwen/Qwen3-0.6B")
        self.assertEqual(self.model.source_revision, "immutable-revision")
        self.assertEqual(self.model.semantics_digest, TOKENIZER)
        self.assertTrue(self.model.intent_digest.startswith("sha256:"))

    def test_public_model_request_requires_immutable_revision(self):
        moving = ModelRef(
            name="Qwen/Qwen3-0.6B", content_digest=DIGEST,
            tokenizer_digest=TOKENIZER)
        with self.assertRaisesRegex(ValueError, "immutable model revision"):
            self.application.request(
                model=moving, input=self.input, generation=self.generation)

    def test_wrong_request_ack_snapshot_is_rejected_before_planning(self):
        foreign = type("Closed", (), {
            "request_id": "/spec168/request/other",
            "candidates": (),
        })()
        with self.assertRaisesRegex(ValueError, "request binding mismatch"):
            AutomaticPlanningCoordinator._validate_ack_closed_binding(
                foreign, "/spec168/request/expected")
        wrong_candidate = type("Closed", (), {
            "request_id": "/spec168/request/expected",
            "candidates": (type("Ack", (), {
                "request_id": "/spec168/request/other",
            })(),),
        })()
        with self.assertRaisesRegex(ValueError, "candidate request binding"):
            AutomaticPlanningCoordinator._validate_ack_closed_binding(
                wrong_candidate, "/spec168/request/expected")

    def test_public_request_is_model_first_and_forwards_strategy(self):
        strategy = object()
        result = self.application.request(
            model=self.model, input=self.input, generation=self.generation,
            strategy=strategy)
        self.assertEqual(result, "model-handle")
        kind, kwargs = self.client.calls[-1]
        self.assertEqual(kind, "model")
        self.assertIs(kwargs["strategy"], strategy)
        self.assertEqual(kwargs["generation"].max_new_tokens, 8)

    def test_preplanned_path_is_explicit_and_positional_shim_is_counted(self):
        explicit = self.application.request_preplanned(
            "deployment-1", input=b"prompt")
        self.assertEqual(explicit, "preplanned-handle")
        self.assertEqual(self.application.preplanned_compatibility_uses, 0)
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            legacy = self.application.request("deployment-2", input=b"prompt")
        self.assertEqual(legacy, "preplanned-handle")
        self.assertEqual(self.application.preplanned_compatibility_uses, 1)
        self.assertTrue(any(item.category is DeprecationWarning for item in caught))
        with self.assertRaises(TypeError):
            self.application.request(deployment="deployment-3", input=b"prompt")

    def test_generation_contract_rejects_empty_prompt_and_unbounded_tokens(self):
        with self.assertRaises(ValueError):
            GenerationInput(prompt="")
        with self.assertRaises(ValueError):
            GenerationConfig(max_new_tokens=0)
        with self.assertRaises(ValueError):
            GenerationConfig(max_new_tokens=1_000_001)


if __name__ == "__main__":
    unittest.main()
