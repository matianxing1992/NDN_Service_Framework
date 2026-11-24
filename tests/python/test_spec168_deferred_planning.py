#!/usr/bin/env python3
"""Spec 168 model-first API and deferred-planning boundary tests."""

from __future__ import annotations

import unittest

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
            # The runtime identity now binds a group prefix and trust schema as well; without them the
            # identity check fires first and hides what this case is about.
            "group": "/spec168/group",
            "trust_schema": "tests/fixtures/trust.conf",
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
                "group": "/spec168/group",
                "trust_schema": "tests/fixtures/trust.conf",
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


    def test_preplanned_path_forwards_explicit_deployment(self):
        deployment = object()
        result = self.application.request_preplanned(
            deployment, input=b"prompt", timeout=5)
        self.assertEqual(result, "preplanned-handle")
        self.assertEqual(self.client.calls, [
            ("preplanned", deployment, {"input": b"prompt", "timeout": 5,
                                       "deadline": None, "options": None})])

    def test_generation_contract_rejects_empty_prompt_and_unbounded_tokens(self):
        with self.assertRaises(ValueError):
            GenerationInput(prompt="")
        with self.assertRaises(ValueError):
            GenerationConfig(max_new_tokens=0)
        with self.assertRaises(ValueError):
            GenerationConfig(max_new_tokens=1_000_001)


if __name__ == "__main__":
    unittest.main()
