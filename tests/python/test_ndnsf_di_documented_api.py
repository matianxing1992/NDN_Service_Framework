from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

from ndnsf_distributed_inference.app_sdk.application import (
    ApplicationDefinitionSigner, InferenceApplication,
)


ROOT = Path(__file__).resolve().parents[2]


class DocumentedApiTest(unittest.TestCase):
    def test_english_and_chinese_readmes_name_the_same_primary_roles(self):
        english = (ROOT / "NDNSF-DistributedInference/README.md").read_text()
        chinese = (ROOT / "NDNSF-DistributedInference/README_ch.md").read_text()
        for name in ("InferenceApplication", "InferenceClient", "InferenceProvider"):
            self.assertIn(name, english)
            self.assertIn(name, chinese)
        self.assertIn("application.request", english)
        self.assertIn("application.request", chinese)

    def test_canonical_example_builds_a_signed_definition_without_optional_backend(self):
        path = ROOT / "examples/python/NDNSF-DistributedInference/spec116_user_api.py"
        spec = importlib.util.spec_from_file_location("spec116_user_api", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        application = InferenceApplication(
            ApplicationDefinitionSigner.generate("/example/application"), None)
        definition = module.build_definition(application)
        self.assertTrue(definition.signed)
        self.assertEqual(definition.service, "/LLM/Qwen/Generate")


if __name__ == "__main__":
    unittest.main()
